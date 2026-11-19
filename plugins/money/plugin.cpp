// 金币修改插件：消费宿主的 mayday.money 服务（il2cpp 细节由宿主封装），
// 提供金币/债务查看与修改浮窗。语义与参考实现一致：
// 设为该值直接写金币字段；增加该值走游戏 ServerAddMoney 记账入口；
// 清空债务通过 ServerAddDebt 的差额语义归零。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"
#include "anomaly/sdk/services/mayday.h"
#include "anomaly/sdk/services/ui.h"

#include <array>
#include <limits>
#include <string>
#include <string_view>

namespace {

const AnomalyHostApiV1* g_host{};
const AnomalyMaydayMoneyServiceV1* g_money{};
anomaly::sdk::Localizer g_localizer;
std::uint32_t g_amount = 100000;
// Update 发布快照，Draw 只读；按钮点击只记录意图，写操作在 Update 里执行，
// 不在 Render 的绘制路径上调用游戏服务。
std::int32_t g_money_value{};
std::int32_t g_debt_value{};
bool g_values_ready{};
std::string g_status;

enum class PendingCommand : std::uint32_t { None, SetMoney, AddMoney, ClearDebt };
PendingCommand g_pending_command{PendingCommand::None};
std::uint32_t g_pending_amount{};

bool QueryMoneyService() {
    if (g_money != nullptr) return true;
    if (g_host == nullptr) return false;
    const auto service = anomaly::sdk::Host(g_host).Query<AnomalyMaydayMoneyServiceV1>(
        ANOMALY_MAYDAY_MONEY_SERVICE_V1_ID, ANOMALY_MAYDAY_MONEY_SERVICE_V1_VERSION);
    if (!service) return false;
    g_money = service.get();
    return true;
}

// 服务失败时 message 是稳定诊断 key（见 services/mayday.h），在这里翻译。
void SetStatusFromService(const AnomalyStatusV1& status) {
    std::string_view message;
    if (status.message.data != nullptr) {
        message = std::string_view(status.message.data, status.message.size);
    }
    if (message == "status.unavailable") {
        g_status = g_localizer.Text("status.unavailable", "Game symbols are not ready yet.");
    } else if (message == "status.not_in_flight") {
        g_status = g_localizer.Text("status.not_in_flight", "Enter a flight to edit money.");
    } else if (message == "status.attach_failed") {
        g_status = g_localizer.Text(
            "status.attach_failed", "Could not attach to the game runtime.");
    } else {
        g_status = g_localizer.Text("status.failed", "Operation failed.");
    }
}

bool RefreshStatus(const AnomalyMaydayMoneyServiceV1* money) {
    g_status.clear();
    if (money == nullptr) {
        g_status = g_localizer.Text(
            "status.waiting", "Waiting for the host money service.");
        g_values_ready = false;
        return false;
    }
    const auto money_status = money->get_money(money->user, &g_money_value);
    if (!anomaly::sdk::Succeeded(money_status)) {
        SetStatusFromService(money_status);
        g_values_ready = false;
        return false;
    }
    const auto debt_status = money->get_debt(money->user, &g_debt_value);
    if (!anomaly::sdk::Succeeded(debt_status)) {
        SetStatusFromService(debt_status);
        g_values_ready = false;
        return false;
    }
    g_values_ready = true;
    return true;
}

// 游戏金币字段与 set_money/add_money 契约是 int32，而输入控件接受完整 uint32：
// 超过 int32 上限的金额钳到上限，避免 static_cast 把 30 亿静默回绕成负数。
std::int32_t MoneyAmount(std::uint32_t amount) noexcept {
    constexpr std::uint32_t kMaximum =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    return amount <= kMaximum ? static_cast<std::int32_t>(amount)
                              : (std::numeric_limits<std::int32_t>::max)();
}

void ApplyPendingCommand(const AnomalyMaydayMoneyServiceV1* money) {
    const PendingCommand command = g_pending_command;
    const std::uint32_t amount = g_pending_amount;
    g_pending_command = PendingCommand::None;
    g_pending_amount = 0;
    if (money == nullptr || command == PendingCommand::None) return;
    AnomalyStatusV1 status{ANOMALY_STATUS_V1_OK, 0, {}};
    switch (command) {
    case PendingCommand::SetMoney:
        status = money->set_money(money->user, MoneyAmount(amount));
        break;
    case PendingCommand::AddMoney:
        status = money->add_money(money->user, MoneyAmount(amount));
        break;
    case PendingCommand::ClearDebt:
        status = money->set_debt(money->user, 0);
        break;
    case PendingCommand::None:
        return;
    }
    if (!anomaly::sdk::Succeeded(status)) SetStatusFromService(status);
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    g_host = host;
    g_localizer = anomaly::sdk::Localizer(host);
    static_cast<void>(QueryMoneyService());
    *context = &g_amount;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) { return anomaly::sdk::Ok(); }
AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) { return anomaly::sdk::Ok(); }

void ANOMALY_CALL Unload(void*) {
    g_host = nullptr;
    g_money = nullptr;
    g_status.clear();
    g_values_ready = false;
    g_pending_command = PendingCommand::None;
    g_pending_amount = 0;
}

void ANOMALY_CALL Update(void*, double) {
    // 服务表是宿主探测 GameAssembly 后才发布的，这里懒查询补挂。
    static_cast<void>(QueryMoneyService());
    ApplyPendingCommand(g_money);
    static_cast<void>(RefreshStatus(g_money));
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) return;
    int open = 1;
    const std::string title = g_localizer.Text("window.title", "Money Editor");
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    if (!g_values_ready) {
        if (!g_status.empty()) ui->text(ui->user, anomaly::sdk::StringView(g_status));
        return;
    }

    const std::string money_text = std::to_string(g_money_value);
    const std::array<std::string_view, 1> money_arguments{money_text};
    const std::string money_label =
        g_localizer.Format("money.label", "Money ${0}", money_arguments);
    ui->text(ui->user, anomaly::sdk::StringView(money_label));

    const std::string debt_text = std::to_string(g_debt_value);
    const std::array<std::string_view, 1> debt_arguments{debt_text};
    const std::string debt_label =
        g_localizer.Format("debt.label", "Debt ${0}", debt_arguments);
    ui->text(ui->user, anomaly::sdk::StringView(debt_label));
    ui->separator(ui->user);

    ui->text(ui->user, anomaly::sdk::StringView(
        g_localizer.Text("amount.label", "Amount")));
    ui->input_uint32(ui->user, anomaly::sdk::StringView("##amount"), &g_amount, 0, 0);

    // 点击只排队；Update 在同一插件域里执行写操作并刷新快照。
    const std::string set_label = g_localizer.Text("button.set", "Set to this value");
    if (ui->button(ui->user, anomaly::sdk::StringView(set_label), -1.0f, 0.0f)) {
        g_pending_command = PendingCommand::SetMoney;
        g_pending_amount = g_amount;
    }
    const std::string add_label = g_localizer.Text("button.add", "Add this value");
    if (ui->button(ui->user, anomaly::sdk::StringView(add_label), -1.0f, 0.0f)) {
        g_pending_command = PendingCommand::AddMoney;
        g_pending_amount = g_amount;
    }
    const std::string quick_label = g_localizer.Text("button.quick", "Money +100000");
    if (ui->button(ui->user, anomaly::sdk::StringView(quick_label), -1.0f, 0.0f)) {
        g_pending_command = PendingCommand::AddMoney;
        g_pending_amount = 100000;
    }
    const std::string clear_label = g_localizer.Text("button.clear_debt", "Clear debt");
    if (ui->button(ui->user, anomaly::sdk::StringView(clear_label), -1.0f, 0.0f)) {
        g_pending_command = PendingCommand::ClearDebt;
        g_pending_amount = 0;
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView("mayday.money"),
        anomaly::sdk::StringView("Money Editor"),
        anomaly::sdk::StringView("Mayday Loader"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
