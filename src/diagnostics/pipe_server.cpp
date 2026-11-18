#include "pipe_server.hpp"

#include "anomaly/diagnostic_pipe_service.hpp"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

namespace anomaly {
namespace {

void LogPipeError(const Analyzer& analyzer, DWORD error) noexcept {
    try {
        const auto path = analyzer.Root() / L"anomaly-pipe-error.log";
        // 低频失败诊断日志：超过 1 MiB 时截断重开，避免无界追加。
        constexpr std::uintmax_t kMaximumLogBytes = 1024U * 1024U;
        std::error_code size_error;
        const auto size = std::filesystem::file_size(path, size_error);
        if (!size_error && size > kMaximumLogBytes) {
            std::ofstream truncate_log(path, std::ios::trunc);
        }
        std::ofstream error_log(path, std::ios::app);
        error_log << "pipe service error=" << error << '\n';
    } catch (...) {
    }
}

bool WaitForRetry(std::stop_token stop_token) noexcept {
    constexpr DWORD retry_slice_ms = 50;
    constexpr DWORD retry_slices = 20;
    for (DWORD index = 0; index < retry_slices; ++index) {
        if (stop_token.stop_requested()) return true;
        Sleep(retry_slice_ms);
    }
    return stop_token.stop_requested();
}

void RunPipeServerLoop(
    const Analyzer& analyzer,
    const std::wstring& pipe_name,
    std::stop_token stop_token) {
    const std::shared_ptr<const Analyzer> analyzer_reference(
        std::shared_ptr<const Analyzer>{}, &analyzer);
    while (!stop_token.stop_requested()) {
        anomaly::DiagnosticPipeService service({analyzer_reference, pipe_name});
        DWORD result = service.Prepare();
        if (result == ERROR_SUCCESS) result = service.Run(stop_token);
        if (result == ERROR_SUCCESS || stop_token.stop_requested()) return;
        LogPipeError(analyzer, result);
        if (WaitForRetry(stop_token)) return;
    }
}

}  // namespace

std::wstring BuildPipeName(std::wstring_view prefix, unsigned long process_id) {
    return L"\\\\.\\pipe\\LOCAL\\" + std::wstring(prefix) + L"-" +
        std::to_wstring(process_id);
}

void RunPipeServer(
    const Analyzer& analyzer,
    const std::wstring& pipe_name,
    std::stop_token stop_token) {
    RunPipeServerLoop(analyzer, pipe_name, stop_token);
}

}  // namespace anomaly
