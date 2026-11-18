#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace anomaly {

// Mayday Airways (Unity IL2CPP) 游戏适配器。
// 上游 Ue5NteAdapter 的 Mayday 对应物：tick 回调生命周期与渲染桥的消费面对齐；
// 引擎符号层走 il2cpp 导出函数（GameAssembly.dll），不做 UE5 特征码 Profile。
// tick 的帧源是渲染桥的 Present 回调（OnFrame），不再 hook 引擎游戏线程。
class MaydayAdapter final {
public:
    using TickCallback = std::function<void(double)>;

    // 发布给插件的 il2cpp 服务表（C ABI，v1）。
    struct Il2CppTableV1 {
        std::uint32_t struct_size;
        void* domain_get;                 // il2cpp_domain_get
        void* class_from_name;            // il2cpp_class_from_name
        void* class_get_method_from_name; // il2cpp_class_get_method_from_name
        void* runtime_invoke;             // il2cpp_runtime_invoke
        void* thread_attach;              // il2cpp_thread_attach
        void* thread_detach;              // il2cpp_thread_detach
        void* string_new;                 // il2cpp_string_new
        void* image_get_class;            // il2cpp_image_get_class
        void* assembly_get_image;         // il2cpp_assembly_get_image
    };

    MaydayAdapter();
    ~MaydayAdapter();
    MaydayAdapter(const MaydayAdapter&) = delete;
    MaydayAdapter& operator=(const MaydayAdapter&) = delete;

    // 启动后台探测线程：等待 GameAssembly.dll 出现并解析 il2cpp 导出表；
    // 解析完成后把 Il2CppTableV1 发布到 AdapterServiceRegistry（id="mayday.il2cpp"）。
    // 即使游戏模块暂未加载也返回 true（降级运行，后续探测成功再发布）。
    [[nodiscard]] bool Start(std::stop_token stop_token);
    bool Stop(std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;

    void SetTickCallback(TickCallback callback);
    // 立即摘除回调并等待已进入的回调返回；超时返回 false 但回调必然已摘除。
    bool ClearTickCallback(std::chrono::seconds timeout) noexcept;

    // 渲染桥每帧调用（Present 域）：驱动 tick 回调（插件 GameUpdate 的帧源）。
    void OnFrame(double delta_seconds);

    // 账号遥测键：Mayday 暂无对应概念，恒空。
    [[nodiscard]] std::optional<std::string> AccountKey() const;

    [[nodiscard]] std::string DiagnosticsJson() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anomaly
