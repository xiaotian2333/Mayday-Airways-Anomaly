#include "anomaly/bootstrap_client.hpp"
#include "anomaly/runtime_launch.hpp"
#include "proxy_exports.hpp"

#include <Windows.h>

#include <filesystem>
#include <string>

namespace {

HMODULE g_original_dwmapi{};
HMODULE g_proxy_module{};
HMODULE g_anomaly_core{};

std::filesystem::path ModuleDirectory(HMODULE module) {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool LoadOriginalDwmapi() {
    std::wstring system_directory(32768, L'\0');
    const UINT length = GetSystemDirectoryW(
        system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) return false;
    system_directory.resize(length);
    g_original_dwmapi = LoadLibraryW(
        (std::filesystem::path(system_directory) / L"dwmapi.dll").c_str());
    if (g_original_dwmapi == nullptr) return false;
    static_cast<void>(ResolveProxyExports(g_original_dwmapi));
    return true;
}

// UTF-8 诊断文本按 MultiByteToWideChar 正确转宽字符；按字节 widen 会让
// 包含非 ASCII（例如中文路径）的消息变成乱码。
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

DWORD WINAPI BootstrapThread(void*) {
    const auto runtime_root = ModuleDirectory(g_proxy_module) / L"Anomaly";
    const auto log_directory = runtime_root / L"logs";
    const auto selected = anomaly::ResolveRuntimeLaunch({
        .installation_root = runtime_root,
    });
    if (!selected.Ok()) {
        OutputDebugStringW(
            (L"Anomaly: Runtime launch failed: " + Utf8ToWide(selected.message) + L"\n")
                .c_str());
        return ERROR_INVALID_STATE;
    }
    const auto& core_path = selected.core_path;
    const AnomalyStartInfo start_info{
        .struct_size = ANOMALY_START_INFO_V1_SIZE,
        .bootstrap_abi_version = ANOMALY_BOOTSTRAP_ABI_VERSION,
        .bootstrap_type = ANOMALY_BOOTSTRAP_TYPE_DWMAPI_PROXY,
        .flags = 0,
        .bootstrap_module = g_proxy_module,
        .game_module = GetModuleHandleW(nullptr),
        .runtime_root = runtime_root.c_str(),
        .log_directory = log_directory.c_str(),
        .external_stop_event = nullptr,
    };
    const anomaly::BootstrapRuntimeResult result =
        anomaly::StartRuntimeCore(core_path, start_info);
    if (!result) {
        OutputDebugStringW(
            (L"Anomaly: core start failed: " + core_path.wstring() + L" error=" +
             std::to_wstring(result.error) + L"\n")
                .c_str());
        return result.error;
    }
    g_anomaly_core = result.module;
    return ERROR_SUCCESS;
}

}  // namespace

extern "C" std::uintptr_t WINAPI ProxyMissingExport() {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return static_cast<std::uintptr_t>(E_NOTIMPL);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_proxy_module = module;
        DisableThreadLibraryCalls(module);
        if (!LoadOriginalDwmapi()) return FALSE;
        const HANDLE thread = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        } else {
            // 线程创建失败时 Core 永远不会启动，至少留下一条诊断痕迹。
            OutputDebugStringW(
                (L"Anomaly: bootstrap thread creation failed error=" +
                 std::to_wstring(GetLastError()) + L"\n")
                    .c_str());
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr && g_original_dwmapi != nullptr) {
        FreeLibrary(g_original_dwmapi);
        g_original_dwmapi = nullptr;
    }
    return TRUE;
}
