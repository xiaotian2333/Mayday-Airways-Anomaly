// 音乐播放插件：读取本地 mp3/flac/wav，经宿主 mayday.voice 服务把 μ-law 编码
// 帧送入游戏语音管线（PlayerVoice.SendVoice → Mirror 广播给全房间）；「给自己
// 播放」走 miniaudio 本机回放（原始音质），两条链路互相独立、开关分离。
// 线程域：解码/扫描/文件 I/O 在 Worker（scheduler 任务），发送推进与 il2cpp
// 调用在 Game（on_update），UI 只读快照并记录意图（on_draw）。
#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/localization.hpp"
#include "anomaly/sdk/services/core.h"
#include "anomaly/sdk/services/json.h"
#include "anomaly/sdk/services/mayday.h"
#include "anomaly/sdk/services/platform.h"
#include "anomaly/sdk/services/ui.h"

#include "miniaudio.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kSchemaId = "music";
constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::string_view kSchema =
    R"json({"type":"object","additionalProperties":false,"required":["music_dir","send_volume","self_listen","self_volume","route","loop"],"properties":{"music_dir":{"type":"string","maxLength":600},"send_volume":{"type":"number","minimum":0,"maximum":2},"self_listen":{"type":"boolean"},"self_volume":{"type":"number","minimum":0,"maximum":2},"route":{"type":"integer","minimum":0,"maximum":2},"loop":{"type":"integer","minimum":0,"maximum":1}}})json";

// 单首曲目解码上限：约 42 分钟 16 kHz mono float（1 GiB 内存的保守边界）。
constexpr std::uint64_t kMaxSourceFileBytes = 512ull * 1024 * 1024;
constexpr std::size_t kMaxPcmSamples = 64u * 1024 * 1024;
// μ-law 帧长上界：游戏 MuLawPacketBytes 远小于该值，超界视为异常布局拒发。
constexpr std::int32_t kMaxFrameBytes = 4096;
// 一次 Update 最多补发的帧数：帧率骤降后防止瞬间堆积整秒数据。
constexpr int kSendFrameBudget = 4;

constexpr std::size_t kMaxDirBytes = 600;

// 播放状态机（Game 域为权威写入者，Render 只读）。
constexpr int kIdle = 0;
constexpr int kLoading = 1;
constexpr int kPlaying = 2;
constexpr int kPaused = 3;

// 解码状态（Worker 写 / Game 读）。kDecodeDone 借用值 2 由 Game 域消费后清零。
constexpr int kDecodeIdle = 0;
constexpr int kDecodeRunning = 1;
constexpr int kDecodeDone = 2;
constexpr int kDecodeFailed = 3;

// UI 意图（Render 记录 → Game 消费）。
constexpr int kActionNone = 0;
constexpr int kActionPlay = 1;
constexpr int kActionPause = 2;
constexpr int kActionStop = 3;
constexpr int kActionPrev = 4;
constexpr int kActionNext = 5;

// 状态码（跨域稳定 key，Draw 里本地化）。
constexpr int kStatusNone = 0;
constexpr int kStatusWaiting = 1;
constexpr int kStatusNotInFlight = 2;
constexpr int kStatusAttachFailed = 3;
constexpr int kStatusNoSymbols = 4;
constexpr int kStatusFailed = 5;
constexpr int kStatusDecoding = 6;
constexpr int kStatusDecodeFailed = 7;
constexpr int kStatusRateMissing = 8;
constexpr int kStatusNoTracks = 9;
constexpr int kStatusScanFailed = 10;
constexpr int kStatusStopped = 11;

const AnomalyHostApiV1* g_host{};
const AnomalyCoreServiceV1* g_core{};
const AnomalyConfigServiceV1* g_config{};
const AnomalyJsonServiceV1* g_json{};
const AnomalySchedulerServiceV1* g_scheduler{};
const AnomalyMaydayVoiceServiceV1* g_voice{};
anomaly::sdk::Localizer g_localizer;
AnomalyGenerationHandleV1 g_config_schema{};
bool g_schema_registered{};

// ---- 用户设置（Render 写 / Game 读，原子） ----
std::string g_music_dir;                    // UTF-8；Worker 域维护（mutex 保护读取）
std::mutex g_music_dir_mutex;
std::atomic<float> g_send_volume{1.0f};
std::atomic<float> g_self_volume{1.0f};
std::atomic<bool> g_self_listen{false};
std::atomic<std::int32_t> g_voice_route{1};  // 0 近讲 1 机内广播 2 电台
std::atomic<bool> g_loop{true};
std::atomic<bool> g_dirty{false};
std::atomic<bool> g_save_scheduled{false};

// ---- 播放状态 ----
std::atomic<int> g_play_state{kIdle};
std::atomic<int> g_current_track{-1};
std::atomic<int> g_pending_action{kActionNone};
std::atomic<int> g_pending_track{-1};
std::atomic<bool> g_refresh_pending{false};
std::atomic<int> g_status{kStatusNone};
// 解码状态：0 空闲 1 进行中 2 完成 3 失败（Worker 写，Game 读）。
std::atomic<int> g_decode_state{kIdle};

// 曲目列表（Worker 写 / Render+Game 读）。
std::mutex g_tracks_mutex;
std::vector<std::wstring> g_track_paths;
std::vector<std::string> g_track_names;
std::atomic<int> g_track_count{-1};   // -1 = 尚未扫描
std::string g_pending_scan_dir;       // UI 请求的扫描目录（UTF-8）

// 解码产出（Worker 写 / Game 读）。
struct TrackPcm {
    std::vector<float> samples;   // mono f32 @ 语音采样率
};
std::atomic<std::shared_ptr<const TrackPcm>> g_pcm;
std::mutex g_pcm_name_mutex;
std::string g_pcm_name;           // 已解码曲目名（与 g_pcm 配对）

// Game 域专属发送状态（无需原子）。
std::size_t g_send_pos{};
double g_send_accum{};
std::array<std::uint8_t, kMaxFrameBytes> g_frame{};

// 语音快照（Game 域 refresh → 多域读）。
std::atomic<std::int32_t> g_voice_rate{0};
std::atomic<std::int32_t> g_voice_frame_bytes{0};
std::atomic<std::int32_t> g_voice_interval_ms{0};
std::atomic<std::int32_t> g_voice_backend{-1};   // -1 未知 0 μ-law 1 Dissonance
std::atomic<bool> g_voice_in_flight{false};
std::atomic<bool> g_voice_host{false};
// Dissonance capture 注入快照。
std::atomic<bool> g_inject_available{false};
std::atomic<std::int32_t> g_inject_rate{-1};     // hook 捕获的 WaveFormat.SampleRate
std::atomic<std::int32_t> g_inject_pos{0};       // hook 消费位置（样本数）
// 注入注册状态（Game 域专属，无需原子）。
bool g_inject_registered{false};
const void* g_inject_ptr{nullptr};

// ---- 自听链路（miniaudio）----
// decoder 只在 Worker/Lifecycle 域 init/uninit；device start/stop 任意域。
ma_decoder g_self_decoder{};
bool g_self_decoder_valid{false};
ma_device g_self_device{};
bool g_self_device_init{false};
std::atomic<bool> g_self_device_running{false};
std::atomic<bool> g_self_available{true};

AnomalyByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

bool Succeeded(const AnomalyStatusV1& status) noexcept {
    return anomaly::sdk::Succeeded(status);
}

void PluginLog(const AnomalyCoreLogLevelV1 level, const std::string& message) {
    if (g_core == nullptr || g_core->log == nullptr) return;
    g_core->log(g_core->user, static_cast<std::uint32_t>(level),
                anomaly::sdk::StringView(message));
}

// ---- UTF-8 / UTF-16 转换（Win32 路径用宽字符，UI/配置用 UTF-8） ----

std::wstring Utf8ToWide(const std::string_view utf8) {
    if (utf8.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    static_cast<void>(MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        wide.data(), count));
    return wide;
}

std::string WideToUtf8(const std::wstring_view wide) {
    if (wide.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string utf8(static_cast<std::size_t>(count), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        utf8.data(), count, nullptr, nullptr));
    return utf8;
}

bool HasAudioExtension(const std::wstring_view name) noexcept {
    const std::array<std::wstring_view, 3> suffixes{L".mp3", L".flac", L".wav"};
    for (const std::wstring_view suffix : suffixes) {
        if (name.size() > suffix.size() &&
            std::equal(suffix.rbegin(), suffix.rend(), name.rbegin(),
                       [](const wchar_t a, const wchar_t b) {
                           return std::towlower(a) == std::towlower(b);
                       })) {
            return true;
        }
    }
    return false;
}

// ---- 自听：miniaudio 设备回调（设备私有线程） ----

void SelfListenDataCallback(ma_device* device, void* output,
                            const void* /*input*/, ma_uint32 frames) {
    // decoder 流式读原始音质数据；读不足（曲尾）补零静音，发送侧负责切曲。
    if (!g_self_decoder_valid || !g_self_device_running.load(std::memory_order_acquire)) {
        return;
    }
    const ma_uint64 channels = g_self_decoder.outputChannels;
    const std::size_t want = static_cast<std::size_t>(frames) *
        static_cast<std::size_t>(channels);
    ma_uint64 read = 0;
    static_cast<void>(ma_decoder_read_pcm_frames(
        &g_self_decoder, output, static_cast<ma_uint64>(frames), &read));
    auto* const samples = static_cast<float*>(output);
    const std::size_t got = static_cast<std::size_t>(read) *
        static_cast<std::size_t>(channels);
    for (std::size_t i = got; i < want; ++i) samples[i] = 0.0f;
    const float volume = g_self_volume.load(std::memory_order_relaxed);
    if (volume != 1.0f) {
        for (std::size_t i = 0; i < got; ++i) samples[i] *= volume;
    }
}

// ---- 自听设备控制（Game/Lifecycle 域调用；miniaudio API 线程安全） ----

void StopSelfDevice() {
    if (g_self_device_init && g_self_device_running.exchange(false, std::memory_order_acq_rel)) {
        static_cast<void>(ma_device_stop(&g_self_device));
    }
}

bool StartSelfDevice() {
    if (!g_self_device_init || !g_self_decoder_valid) return false;
    if (g_self_device_running.load(std::memory_order_acquire)) return true;
    if (ma_device_start(&g_self_device) != MA_SUCCESS) {
        g_self_available.store(false, std::memory_order_release);
        return false;
    }
    g_self_device_running.store(true, std::memory_order_release);
    return true;
}

// Worker/Lifecycle 域：关闭旧自听解码器（device 需先停）。
void CloseSelfDecoder() {
    if (g_self_decoder_valid) {
        ma_decoder_uninit(&g_self_decoder);
        g_self_decoder_valid = false;
    }
}

// Worker 域：打开自听解码器（原始采样率/声道，f32 输出）。
bool OpenSelfDecoder(const std::wstring& path) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    // 0 通道/0 采样率 = 保持文件原始声道数与采样率。
    if (ma_decoder_init_file_w(path.c_str(), &config, &g_self_decoder) != MA_SUCCESS) {
        g_self_decoder_valid = false;
        return false;
    }
    g_self_decoder_valid = true;
    return true;
}

// 任意域：确保自听设备按当前 decoder 输出格式就绪并启动。
bool EnsureSelfDeviceStarted() {
    if (!g_self_decoder_valid) return false;
    if (g_self_device_init) return StartSelfDevice();
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = g_self_decoder.outputFormat;
    config.playback.channels = g_self_decoder.outputChannels;
    config.sampleRate = g_self_decoder.outputSampleRate;
    config.dataCallback = SelfListenDataCallback;
    if (ma_device_init(nullptr, &config, &g_self_device) != MA_SUCCESS) {
        g_self_available.store(false, std::memory_order_release);
        return false;
    }
    g_self_device_init = true;
    return StartSelfDevice();
}

// ---- 配置持久化（autobat 防抖模式） ----

void SaveSettingsTask(void*, AnomalyGenerationHandleV1) {
    g_save_scheduled.store(false, std::memory_order_release);
    if (!g_dirty.exchange(false, std::memory_order_acq_rel)) return;
    if (g_config == nullptr) {
        g_dirty.store(true, std::memory_order_release);
        return;
    }
    std::string escaped;
    {
        std::scoped_lock lock(g_music_dir_mutex);
        escaped.reserve(g_music_dir.size() + 8);
        for (const char c : g_music_dir) {
            switch (c) {
                case '"': escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped += c;
            }
        }
    }
    char volume_send[8]{};
    std::snprintf(volume_send, sizeof(volume_send), "%.2f",
                  static_cast<double>(g_send_volume.load(std::memory_order_relaxed)));
    char volume_self[8]{};
    std::snprintf(volume_self, sizeof(volume_self), "%.2f",
                  static_cast<double>(g_self_volume.load(std::memory_order_relaxed)));
    std::string document;
    document.reserve(escaped.size() + 160);
    document += "{\"music_dir\":\"";
    document += escaped;
    document += "\",\"send_volume\":";
    document += volume_send;
    document += ",\"self_listen\":";
    document += g_self_listen.load(std::memory_order_relaxed) ? "true" : "false";
    document += ",\"self_volume\":";
    document += volume_self;
    document += ",\"route\":";
    document += std::to_string(g_voice_route.load(std::memory_order_relaxed));
    document += ",\"loop\":";
    document += g_loop.load(std::memory_order_relaxed) ? "1" : "0";
    document += "}";
    const AnomalyStatusV1 status = g_config->write_atomic(
        g_config->user, anomaly::sdk::StringView(kSchemaId), kSchemaVersion,
        Bytes(document));
    if (!Succeeded(status)) g_dirty.store(true, std::memory_order_release);
}

void MarkSettingsDirty() {
    g_dirty.store(true, std::memory_order_release);
    if (g_scheduler == nullptr) return;
    if (g_save_scheduled.exchange(true, std::memory_order_acq_rel)) return;
    AnomalyGenerationHandleV1 handle{};
    if (!Succeeded(g_scheduler->schedule(
            g_scheduler->user, 250, SaveSettingsTask, nullptr, &handle))) {
        g_save_scheduled.store(false, std::memory_order_release);
    }
}

void LoadSettings() {
    if (g_config == nullptr || g_json == nullptr) return;
    std::uint32_t version{};
    std::size_t size{};
    AnomalyStatusV1 status = g_config->read(
        g_config->user, anomaly::sdk::StringView(kSchemaId), &version,
        {nullptr, 0}, &size);
    if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) return;
    if (!Succeeded(status) || size == 0 || size > 4096) return;
    std::string document(size, '\0');
    std::size_t copied = document.size();
    status = g_config->read(
        g_config->user, anomaly::sdk::StringView(kSchemaId), &version,
        {reinterpret_cast<std::uint8_t*>(document.data()), document.size()}, &copied);
    if (!Succeeded(status) || copied == 0 || copied > document.size()) return;
    if (version != kSchemaVersion) return;
    AnomalyGenerationHandleV1 root{};
    if (!Succeeded(g_json->parse(
            g_json->user, {document.data(), copied}, &root))) {
        return;
    }
    // music_dir：两段调用协议读字符串。宿主 string_value 返回的 size 含 null
    // 终止符（required = source.size() + 1），本体在前 taken-1 字节；不剔除的
    // 话 embedded null 会让 FindFirstFileW 的路径被截断。
    AnomalyGenerationHandleV1 dir{};
    if (Succeeded(g_json->object_find(
            g_json->user, root, anomaly::sdk::StringView("music_dir"), &dir))) {
        std::size_t dir_size = 0;
        if (Succeeded(g_json->string_value(
                g_json->user, dir, nullptr, &dir_size)) && dir_size > 0 &&
            dir_size <= kMaxDirBytes) {
            std::string value(dir_size, '\0');
            std::size_t taken = dir_size;
            if (Succeeded(g_json->string_value(
                    g_json->user, dir, value.data(), &taken)) && taken > 0) {
                value.resize(taken - 1);
                std::scoped_lock lock(g_music_dir_mutex);
                g_music_dir = std::move(value);
            }
        }
    }
    if (dir.id != 0) static_cast<void>(g_json->release(g_json->user, dir));
    AnomalyGenerationHandleV1 node{};
    double number = 0.0;
    if (Succeeded(g_json->object_find(
            g_json->user, root, anomaly::sdk::StringView("send_volume"), &node)) &&
        Succeeded(g_json->number_value(g_json->user, node, &number)) &&
        number >= 0.0 && number <= 2.0) {
        g_send_volume.store(static_cast<float>(number), std::memory_order_release);
    }
    if (node.id != 0) static_cast<void>(g_json->release(g_json->user, node));
    if (Succeeded(g_json->object_find(
            g_json->user, root, anomaly::sdk::StringView("self_volume"), &node)) &&
        Succeeded(g_json->number_value(g_json->user, node, &number)) &&
        number >= 0.0 && number <= 2.0) {
        g_self_volume.store(static_cast<float>(number), std::memory_order_release);
    }
    if (node.id != 0) static_cast<void>(g_json->release(g_json->user, node));
    std::int32_t flag = 0;
    if (Succeeded(g_json->object_find(
            g_json->user, root, anomaly::sdk::StringView("self_listen"), &node)) &&
        Succeeded(g_json->boolean_value(g_json->user, node, &flag))) {
        g_self_listen.store(flag != 0, std::memory_order_release);
    }
    if (node.id != 0) static_cast<void>(g_json->release(g_json->user, node));
    if (Succeeded(g_json->object_find(
            g_json->user, root, anomaly::sdk::StringView("route"), &node)) &&
        Succeeded(g_json->number_value(g_json->user, node, &number)) &&
        number >= 0.0 && number <= 2.0) {
        g_voice_route.store(static_cast<std::int32_t>(number + 0.5),
                            std::memory_order_release);
    }
    if (node.id != 0) static_cast<void>(g_json->release(g_json->user, node));
    if (Succeeded(g_json->object_find(
            g_json->user, root, anomaly::sdk::StringView("loop"), &node)) &&
        Succeeded(g_json->number_value(g_json->user, node, &number)) &&
        number >= 0.0 && number <= 1.0) {
        g_loop.store(number >= 0.5, std::memory_order_release);
    }
    if (node.id != 0) static_cast<void>(g_json->release(g_json->user, node));
    static_cast<void>(g_json->release(g_json->user, root));
}

// ---- Worker 任务：目录扫描 ----

void ScanDirectoryTask(void*, AnomalyGenerationHandleV1) {
    try {
        std::string dir_utf8;
        {
            std::scoped_lock lock(g_music_dir_mutex);
            dir_utf8 = g_music_dir;
        }
        std::wstring dir = Utf8ToWide(dir_utf8);
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        if (dir.empty()) {
            g_status.store(kStatusScanFailed, std::memory_order_release);
            return;
        }
        std::vector<std::wstring> paths;
        std::vector<std::string> names;
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) {
            g_status.store(kStatusScanFailed, std::memory_order_release);
            return;
        }
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            const std::wstring_view name(data.cFileName);
            if (!HasAudioExtension(name)) continue;
            paths.push_back(dir + L"\\" + std::wstring(name));
            names.push_back(WideToUtf8(name));
        } while (FindNextFileW(find, &data) != 0);
        static_cast<void>(FindClose(find));
        std::scoped_lock lock(g_tracks_mutex);
        g_track_paths = std::move(paths);
        g_track_names = std::move(names);
        g_track_count.store(
            static_cast<std::int32_t>(g_track_paths.size()), std::memory_order_release);
        g_status.store(g_track_paths.empty() ? kStatusNoTracks : kStatusStopped,
                       std::memory_order_release);
    } catch (const std::exception&) {
        g_status.store(kStatusScanFailed, std::memory_order_release);
    }
}

// ---- Worker 任务：解码（文件 I/O + miniaudio 重采样） ----

std::vector<std::uint8_t> ReadFileBytes(const std::wstring& path) {
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    std::vector<std::uint8_t> bytes;
    if (GetFileSizeEx(handle, &size) && size.QuadPart > 0 &&
        static_cast<std::uint64_t>(size.QuadPart) <= kMaxSourceFileBytes) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read_bytes = 0;
        if (!ReadFile(handle, bytes.data(),
                      static_cast<DWORD>(bytes.size()), &read_bytes, nullptr) ||
            read_bytes != bytes.size()) {
            bytes.clear();
        }
    }
    static_cast<void>(CloseHandle(handle));
    return bytes;
}

void DecodeTask(void*, AnomalyGenerationHandleV1) {
    try {
        const int index = g_current_track.load(std::memory_order_acquire);
        std::wstring path;
        {
            std::scoped_lock lock(g_tracks_mutex);
            if (index < 0 || index >= static_cast<int>(g_track_paths.size())) {
                g_status.store(kStatusDecodeFailed, std::memory_order_release);
                g_decode_state.store(kDecodeIdle, std::memory_order_release);
                return;
            }
            path = g_track_paths[static_cast<std::size_t>(index)];
        }
        // 解码采样率：Dissonance 注入模式用 hook 捕获的 capture 采样率
        // （一般 48000），Legacy μ-law 模式用游戏 MuLawRate。
        std::int32_t rate = g_inject_rate.load(std::memory_order_acquire);
        if (rate <= 0) {
            rate = g_voice_rate.load(std::memory_order_acquire);
        }
        if (rate <= 0) {
            g_status.store(kStatusRateMissing, std::memory_order_release);
            g_decode_state.store(kDecodeIdle, std::memory_order_release);
            return;
        }
        const std::vector<std::uint8_t> bytes = ReadFileBytes(path);
        if (bytes.empty()) {
            g_status.store(kStatusDecodeFailed, std::memory_order_release);
            g_decode_state.store(kDecodeIdle, std::memory_order_release);
            return;
        }
        // miniaudio 解码：f32 mono，直接重采样到语音采样率。
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, rate);
        ma_decoder decoder{};
        if (ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder) != MA_SUCCESS) {
            g_status.store(kStatusDecodeFailed, std::memory_order_release);
            g_decode_state.store(kDecodeIdle, std::memory_order_release);
            return;
        }
        auto pcm = std::make_shared<TrackPcm>();
        bool ok = true;
        for (;;) {
            std::array<float, 8192> chunk{};
            ma_uint64 read = 0;
            static_cast<void>(ma_decoder_read_pcm_frames(
                &decoder, chunk.data(), static_cast<ma_uint64>(chunk.size()), &read));
            if (read == 0) break;
            pcm->samples.insert(pcm->samples.end(), chunk.begin(),
                                chunk.begin() + static_cast<std::ptrdiff_t>(read));
            if (pcm->samples.size() > kMaxPcmSamples) {
                ok = false;
                break;
            }
        }
        ma_decoder_uninit(&decoder);
        if (!ok || pcm->samples.empty()) {
            g_status.store(kStatusDecodeFailed, std::memory_order_release);
            g_decode_state.store(kDecodeIdle, std::memory_order_release);
            return;
        }
        // 自听链路：换曲时重建解码器（原始音质流式回放）。device 先停，解码
        // 器关闭后再开新文件，避免音频线程读到已拆毁的 decoder。
        StopSelfDevice();
        CloseSelfDecoder();
        const bool decoder_ok = OpenSelfDecoder(path);
        if (!decoder_ok) {
            PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_WARNING,
                      "music: self-listen decoder unavailable");
        }
        if (g_self_listen.load(std::memory_order_acquire)) {
            static_cast<void>(EnsureSelfDeviceStarted());
        }
        g_pcm.store(std::move(pcm), std::memory_order_release);
        {
            std::scoped_lock lock(g_pcm_name_mutex);
            g_pcm_name = WideToUtf8(path);
        }
        // 位置重置与播放开始由 Game 域消费 kDecodeDone 时执行。
        g_decode_state.store(kDecodeDone, std::memory_order_release);
    } catch (const std::exception&) {
        g_status.store(kStatusDecodeFailed, std::memory_order_release);
        g_decode_state.store(kDecodeIdle, std::memory_order_release);
    }
}

// ---- 服务查询 ----

void QueryServices() {
    if (g_host == nullptr) return;
    const anomaly::sdk::Host host(g_host);
    if (g_core == nullptr) {
        g_core = host.Query<AnomalyCoreServiceV1>(
            ANOMALY_CORE_SERVICE_V1_ID, ANOMALY_CORE_SERVICE_V1_VERSION).get();
    }
    if (g_scheduler == nullptr) {
        g_scheduler = host.Query<AnomalySchedulerServiceV1>(
            ANOMALY_SCHEDULER_SERVICE_V1_ID,
            ANOMALY_SCHEDULER_SERVICE_V1_VERSION).get();
    }
    if (g_voice == nullptr) {
        g_voice = host.Query<AnomalyMaydayVoiceServiceV1>(
            ANOMALY_MAYDAY_VOICE_SERVICE_V1_ID,
            ANOMALY_MAYDAY_VOICE_SERVICE_V1_VERSION).get();
    }
}

// 调度一次 Worker 任务；失败返回 false（调用方决定状态）。
bool ScheduleTask(AnomalyTaskCallbackV1 task) {
    if (g_scheduler == nullptr) return false;
    AnomalyGenerationHandleV1 handle{};
    return Succeeded(g_scheduler->schedule(
        g_scheduler->user, 0, task, nullptr, &handle));
}

// 注册/注销 Dissonance capture 注入（Game 域执行，定义在 Update 前）。
void RegisterInjection(const float* samples, std::int32_t count, std::int32_t rate);

// 按索引播放（Game 域）：记录曲目并投递解码任务。
void StartTrack(const int index) {
    g_current_track.store(index, std::memory_order_release);
    // 换曲前先注销注入（旧缓冲随后可能被释放，hook 不得再读）。
    if (g_inject_registered) RegisterInjection(nullptr, 0, 0);
    g_status.store(kStatusDecoding, std::memory_order_release);
    g_play_state.store(kLoading, std::memory_order_release);
    if (!ScheduleTask(DecodeTask)) {
        g_status.store(kStatusFailed, std::memory_order_release);
        g_play_state.store(kIdle, std::memory_order_release);
    }
}

// 切换到相邻曲目（Game 域）；列表循环开启时环绕。
void AdvanceTrack(const int direction) {
    const int count = g_track_count.load(std::memory_order_acquire);
    if (count <= 0) return;
    int index = g_current_track.load(std::memory_order_acquire) + direction;
    if (index >= count) {
        if (!g_loop.load(std::memory_order_acquire)) {
            g_play_state.store(kIdle, std::memory_order_release);
            StopSelfDevice();
            g_status.store(kStatusStopped, std::memory_order_release);
            return;
        }
        index = 0;
    }
    if (index < 0) index = count - 1;
    StartTrack(index);
}

// 曲目发送完毕后的接续（Game 域）。
void HandleTrackFinished() {
    AdvanceTrack(1);
}

// ---- 标准 G.711 μ-law 编码 ----
// 16-bit 线性 PCM → 8-bit μ-law。接收端由游戏 MuLaw.Decode 解码；若游戏实现
// 偏离标准 G.711（偏置/段界不同）需实机校准此函数。
std::uint8_t EncodeMuLaw(const std::int16_t sample) noexcept {
    int value = sample;
    const int sign = (value >> 8) & 0x80;
    if (sign != 0) value = -value;
    if (value > 32635) value = 32635;
    value += 0x84;   // G.711 偏置 132
    int exponent = 7;
    for (int mask = 0x4000; (value & mask) == 0 && exponent > 0; mask >>= 1) {
        --exponent;
    }
    const int mantissa = (value >> (exponent + 3)) & 0x0F;
    return static_cast<std::uint8_t>(~(sign | (exponent << 4) | mantissa));
}

// 发送推进（Game 域）：按 SendInterval 把 PCM 分帧编码后交给宿主。
void PumpVoiceFrames(const double delta_seconds) {
    const auto pcm = g_pcm.load(std::memory_order_acquire);
    if (pcm == nullptr || pcm->samples.empty()) return;
    const std::int32_t frame_bytes = g_voice_frame_bytes.load(std::memory_order_acquire);
    const std::int32_t rate = g_voice_rate.load(std::memory_order_acquire);
    if (frame_bytes <= 0 || frame_bytes > kMaxFrameBytes || rate <= 0) return;
    const double frame_seconds = static_cast<double>(frame_bytes) /
        static_cast<double>(rate);
    g_send_accum += delta_seconds;
    if (g_send_accum > frame_seconds * 4.0) g_send_accum = frame_seconds * 4.0;
    const float volume = g_send_volume.load(std::memory_order_relaxed);
    int budget = kSendFrameBudget;
    while (budget-- > 0 && g_send_accum >= frame_seconds) {
        g_send_accum -= frame_seconds;
        if (g_send_pos + static_cast<std::size_t>(frame_bytes) > pcm->samples.size()) {
            HandleTrackFinished();
            return;
        }
        const float* const source = pcm->samples.data() + g_send_pos;
        for (std::int32_t i = 0; i < frame_bytes; ++i) {
            float value = source[i] * volume;
            value = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
            g_frame[static_cast<std::size_t>(i)] = EncodeMuLaw(
                static_cast<std::int16_t>(value * 32767.0f));
        }
        const AnomalyStatusV1 status = g_voice->send_frame(
            g_voice->user, g_voice_route.load(std::memory_order_acquire),
            g_frame.data(), frame_bytes);
        if (!Succeeded(status)) {
            // 发送失败（换局/符号缺失等）：停止并展示稳定诊断。
            g_play_state.store(kIdle, std::memory_order_release);
            StopSelfDevice();
            std::string_view key;
            if (status.message.data != nullptr) {
                key = std::string_view(status.message.data, status.message.size);
            }
            if (key == "status.not_in_flight") {
                g_status.store(kStatusNotInFlight, std::memory_order_release);
            } else if (key == "status.attach_failed") {
                g_status.store(kStatusAttachFailed, std::memory_order_release);
            } else if (key == "status.voice_no_symbols") {
                g_status.store(kStatusNoSymbols, std::memory_order_release);
            } else {
                g_status.store(kStatusFailed, std::memory_order_release);
            }
            return;
        }
        g_send_pos += static_cast<std::size_t>(frame_bytes);
    }
}

// ---- 插件回调 ----

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** context) {
    g_host = host;
    g_localizer = anomaly::sdk::Localizer(host);
    const anomaly::sdk::Host view(host);
    const auto config = view.Query<AnomalyConfigServiceV1>(
        ANOMALY_CONFIG_SERVICE_V1_ID, ANOMALY_CONFIG_SERVICE_V1_VERSION);
    const auto json = view.Query<AnomalyJsonServiceV1>(
        ANOMALY_JSON_SERVICE_V1_ID, ANOMALY_JSON_SERVICE_V1_VERSION);
    const auto scheduler = view.Query<AnomalySchedulerServiceV1>(
        ANOMALY_SCHEDULER_SERVICE_V1_ID, ANOMALY_SCHEDULER_SERVICE_V1_VERSION);
    if (!config || !json || !scheduler) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    g_config = config.get();
    g_json = json.get();
    g_scheduler = scheduler.get();
    if (g_config->register_schema == nullptr || g_config->read == nullptr ||
        g_config->write_atomic == nullptr || g_json->parse == nullptr ||
        g_json->object_find == nullptr || g_json->boolean_value == nullptr ||
        g_json->number_value == nullptr || g_json->string_value == nullptr ||
        g_json->release == nullptr) {
        return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, {}};
    }
    const AnomalyStatusV1 schema_status = g_config->register_schema(
        g_config->user, anomaly::sdk::StringView(kSchemaId), kSchemaVersion,
        Bytes(kSchema), &g_config_schema);
    if (!Succeeded(schema_status)) return schema_status;
    g_schema_registered = true;
    LoadSettings();
    PluginLog(ANOMALY_CORE_LOG_LEVEL_V1_INFO, "music: plugin loaded (1.0.0)");
    *context = &g_play_state;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void*) {
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void*, std::uint32_t) {
    // 停止播放与自听；decoder/device 拆卸属于 Lifecycle 域，允许文件句柄操作。
    g_play_state.store(kIdle, std::memory_order_release);
    StopSelfDevice();
    // 先注销注入再释放 PCM 缓冲（hook 保证此后不再读指针）。
    if (g_voice != nullptr && g_voice->set_injection != nullptr) {
        static_cast<void>(g_voice->set_injection(g_voice->user, 0, nullptr, 0, 0));
    }
    g_inject_registered = false;
    g_inject_ptr = nullptr;
    CloseSelfDecoder();
    if (g_self_device_init) {
        ma_device_uninit(&g_self_device);
        g_self_device_init = false;
    }
    if (g_dirty.load(std::memory_order_acquire)) {
        SaveSettingsTask(nullptr, {});
    }
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void*) {
    g_host = nullptr;
    g_core = nullptr;
    g_config = nullptr;
    g_json = nullptr;
    g_scheduler = nullptr;
    g_voice = nullptr;
    g_config_schema = {};
    g_schema_registered = false;
    g_pcm.store(nullptr, std::memory_order_release);
    {
        std::scoped_lock lock(g_tracks_mutex);
        g_track_paths.clear();
        g_track_names.clear();
    }
    g_track_count.store(-1, std::memory_order_release);
    g_current_track.store(-1, std::memory_order_release);
    g_play_state.store(kIdle, std::memory_order_release);
    g_decode_state.store(kDecodeIdle, std::memory_order_release);
    g_pending_action.store(kActionNone, std::memory_order_release);
    g_pending_track.store(-1, std::memory_order_release);
    g_refresh_pending.store(false, std::memory_order_release);
    g_inject_registered = false;
    g_inject_ptr = nullptr;
    g_inject_available.store(false, std::memory_order_release);
    g_inject_rate.store(-1, std::memory_order_release);
    g_inject_pos.store(0, std::memory_order_release);
    {
        std::scoped_lock lock(g_music_dir_mutex);
        g_music_dir.clear();
    }
    {
        std::scoped_lock lock(g_pcm_name_mutex);
        g_pcm_name.clear();
    }
}

// 注册/注销 Dissonance capture 注入（Game 域）。samples 为空即注销；换曲
// 传入新缓冲指针，宿主对指针变化重置消费位置，同指针重注册恢复播放。
void RegisterInjection(const float* samples, std::int32_t count, std::int32_t rate) {
    if (g_voice == nullptr || g_voice->set_injection == nullptr) return;
    const AnomalyStatusV1 status = g_voice->set_injection(
        g_voice->user, samples != nullptr ? 1 : 0, samples, count, rate);
    if (!Succeeded(status)) {
        g_inject_registered = false;
        g_inject_ptr = nullptr;
        g_status.store(kStatusFailed, std::memory_order_release);
        return;
    }
    g_inject_registered = samples != nullptr;
    g_inject_ptr = samples;
    if (samples != nullptr && g_voice->set_injection_volume != nullptr) {
        static_cast<void>(g_voice->set_injection_volume(
            g_voice->user, g_send_volume.load(std::memory_order_relaxed)));
    }
}

// 注入可用判定：capture hook 就绪且已捕获采样率。
bool InjectionUsable() {
    return g_inject_available.load(std::memory_order_acquire) &&
           g_inject_rate.load(std::memory_order_acquire) > 0;
}

void ANOMALY_CALL Update(void*, double delta_seconds) {
    QueryServices();
    if (g_voice == nullptr) {
        if (g_status.load(std::memory_order_acquire) == kStatusNone) {
            g_status.store(kStatusWaiting, std::memory_order_release);
        }
        return;
    }
    // 每帧刷新语音快照（Game 域专用入口）。
    const AnomalyStatusV1 refreshed = g_voice->refresh_state(g_voice->user);
    if (Succeeded(refreshed)) {
        AnomalyMaydayVoiceStateV1 state{};
        state.struct_size = sizeof(state);
        if (Succeeded(g_voice->get_state(g_voice->user, &state))) {
            g_voice_rate.store(state.sample_rate, std::memory_order_release);
            g_voice_frame_bytes.store(state.frame_bytes, std::memory_order_release);
            g_voice_interval_ms.store(state.send_interval_ms, std::memory_order_release);
            g_voice_backend.store(state.backend_dissonance, std::memory_order_release);
            g_voice_in_flight.store(state.in_flight != 0, std::memory_order_release);
            g_voice_host.store(state.host_mode != 0, std::memory_order_release);
            g_inject_available.store(state.injection_available != 0, std::memory_order_release);
            g_inject_rate.store(state.injection_sample_rate, std::memory_order_release);
            g_inject_pos.store(state.injection_position, std::memory_order_release);
        }
    } else {
        g_voice_in_flight.store(false, std::memory_order_release);
        const std::string_view key(refreshed.message.data != nullptr
            ? std::string_view(refreshed.message.data, refreshed.message.size)
            : std::string_view{});
        if (key == "status.voice_no_symbols") {
            g_status.store(kStatusNoSymbols, std::memory_order_release);
        } else if (key == "status.attach_failed") {
            g_status.store(kStatusAttachFailed, std::memory_order_release);
        }
    }

    // 消费 UI 意图。
    const int action = g_pending_action.exchange(kActionNone, std::memory_order_acq_rel);
    const int requested = g_pending_track.exchange(-1, std::memory_order_acq_rel);
    if (action == kActionPlay && requested >= 0) {
        StartTrack(requested);
    } else if (action == kActionPrev) {
        AdvanceTrack(-1);
    } else if (action == kActionNext) {
        AdvanceTrack(1);
    } else if (action == kActionPause) {
        const int state = g_play_state.load(std::memory_order_acquire);
        if (state == kPlaying) {
            g_play_state.store(kPaused, std::memory_order_release);
            StopSelfDevice();
            if (g_inject_registered) RegisterInjection(nullptr, 0, 0);
        } else if (state == kPaused) {
            g_play_state.store(kPlaying, std::memory_order_release);
            if (g_self_listen.load(std::memory_order_acquire)) {
                static_cast<void>(EnsureSelfDeviceStarted());
            }
        }
    } else if (action == kActionStop) {
        g_play_state.store(kIdle, std::memory_order_release);
        StopSelfDevice();
        if (g_inject_registered) RegisterInjection(nullptr, 0, 0);
        g_status.store(kStatusStopped, std::memory_order_release);
    }

    // 目录刷新意图：覆盖当前目录后交 Worker 扫描。
    if (g_refresh_pending.exchange(false, std::memory_order_acq_rel)) {
        g_current_track.store(-1, std::memory_order_release);
        g_play_state.store(kIdle, std::memory_order_release);
        StopSelfDevice();
        if (g_inject_registered) RegisterInjection(nullptr, 0, 0);
        if (!ScheduleTask(ScanDirectoryTask)) {
            g_status.store(kStatusFailed, std::memory_order_release);
        }
    }

    // 解码完成 → 进入播放；解码进行中不动自听设备（Worker 正在换曲）。
    const int decode = g_decode_state.exchange(kDecodeIdle, std::memory_order_acq_rel);
    const bool decoding = decode == kDecodeRunning;
    if (decode == kDecodeDone) {
        g_send_pos = 0;
        g_send_accum = 0.0;
        g_play_state.store(kPlaying, std::memory_order_release);
        g_status.store(kStatusNone, std::memory_order_release);
        // 注入模式：把解码流注册给 capture hook（新缓冲指针 → 宿主位置归零）。
        if (InjectionUsable()) {
            const auto pcm = g_pcm.load(std::memory_order_acquire);
            if (pcm != nullptr && !pcm->samples.empty()) {
                RegisterInjection(
                    pcm->samples.data(),
                    static_cast<std::int32_t>(pcm->samples.size()),
                    g_inject_rate.load(std::memory_order_acquire));
            }
        } else if (g_inject_registered) {
            RegisterInjection(nullptr, 0, 0);
        }
    }

    // 播放推进：注入模式由 capture hook 自动消费（这里只判结束与音量）；
    // Legacy 模式按 SendInterval 分帧 μ-law 发送。
    const int play_state_now = g_play_state.load(std::memory_order_acquire);
    if (play_state_now == kPlaying) {
        if (g_inject_registered) {
            const auto pcm = g_pcm.load(std::memory_order_acquire);
            const std::int32_t total = pcm != nullptr
                ? static_cast<std::int32_t>(pcm->samples.size()) : 0;
            if (total > 0 && g_inject_pos.load(std::memory_order_acquire) >= total) {
                HandleTrackFinished();
                return;
            }
            if (g_voice->set_injection_volume != nullptr) {
                static_cast<void>(g_voice->set_injection_volume(
                    g_voice->user, g_send_volume.load(std::memory_order_relaxed)));
            }
        } else {
            PumpVoiceFrames(delta_seconds);
        }
        if (!decoding) {
            if (g_self_listen.load(std::memory_order_acquire) &&
                g_self_available.load(std::memory_order_acquire)) {
                static_cast<void>(EnsureSelfDeviceStarted());
            } else {
                StopSelfDevice();
            }
        }
    } else if (play_state_now == kIdle) {
        StopSelfDevice();
    }
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) {
    if (ui == nullptr) return;
    int open = 1;
    const std::string title = g_localizer.Text("window.title", "Music Player");
    anomaly::sdk::UiWindow window(ui, title, &open);
    if (!window) return;

    QueryServices();
    const int play_state = g_play_state.load(std::memory_order_acquire);

    // 状态行。
    const int status = g_status.load(std::memory_order_acquire);
    std::string status_line;
    switch (status) {
        case kStatusWaiting:
            status_line = g_localizer.Text("status.waiting",
                "Waiting for the voice service. Enter a flight first.");
            break;
        case kStatusNotInFlight:
            status_line = g_localizer.Text("status.not_in_flight",
                "Enter a flight before playing: the voice pipeline is not ready.");
            break;
        case kStatusAttachFailed:
            status_line = g_localizer.Text("status.attach_failed",
                "Could not attach to the game runtime.");
            break;
        case kStatusNoSymbols:
            status_line = g_localizer.Text("status.voice_no_symbols",
                "This game build is missing the symbols needed for voice injection.");
            break;
        case kStatusFailed:
            status_line = g_localizer.Text("status.failed", "Operation failed.");
            break;
        case kStatusDecoding:
            status_line = g_localizer.Text("status.decoding", "Decoding...");
            break;
        case kStatusDecodeFailed:
            status_line = g_localizer.Text("status.decode_failed",
                "Decoding failed: the file could not be read or is unsupported.");
            break;
        case kStatusRateMissing:
            status_line = g_localizer.Text("status.rate_missing",
                "Voice sample rate unknown: enter a flight before playing.");
            break;
        case kStatusNoTracks:
            status_line = g_localizer.Text("status.no_tracks",
                "No mp3/flac/wav files found in the music directory.");
            break;
        case kStatusScanFailed:
            status_line = g_localizer.Text("status.scan_failed",
                "Scanning the directory failed.");
            break;
        case kStatusStopped:
            status_line = g_localizer.Text("status.stopped", "Stopped.");
            break;
        default:
            break;
    }
    if (!status_line.empty()) {
        ui->text(ui->user, anomaly::sdk::StringView(status_line));
    }
    if (play_state == kPlaying || play_state == kPaused) {
        const int current = g_current_track.load(std::memory_order_acquire);
        std::string name;
        {
            std::scoped_lock lock(g_tracks_mutex);
            if (current >= 0 && current < static_cast<int>(g_track_names.size())) {
                name = g_track_names[static_cast<std::size_t>(current)];
            }
        }
        if (!name.empty()) {
            const std::string_view arguments[]{name};
            const std::string line = g_localizer.Format(
                play_state == kPlaying ? "status.playing" : "status.paused",
                play_state == kPlaying ? "Playing: {0}" : "Paused: {0}",
                std::span<const std::string_view>{arguments});
            ui->text(ui->user, anomaly::sdk::StringView(line));
        }
    }

    // 语音能力信息。
    const std::int32_t rate = g_voice_rate.load(std::memory_order_acquire);
    const std::int32_t frame_bytes = g_voice_frame_bytes.load(std::memory_order_acquire);
    const std::int32_t interval = g_voice_interval_ms.load(std::memory_order_acquire);
    const bool injection_mode = g_inject_available.load(std::memory_order_acquire);
    const std::string backend_line = injection_mode
        ? g_localizer.Text("ui.mode_injection",
            "Injection: Dissonance microphone pipeline (mic muted while playing)")
        : g_voice_backend.load(std::memory_order_acquire) == 1
        ? g_localizer.Text("ui.backend_dissonance",
            "Voice backend: Dissonance/Opus (injection unavailable)")
        : g_voice_backend.load(std::memory_order_acquire) == 0
        ? g_localizer.Text("ui.backend_legacy", "Voice backend: mu-law")
        : g_localizer.Text("ui.backend_unknown", "Voice backend: unknown");
    ui->text(ui->user, anomaly::sdk::StringView(backend_line));
    if (injection_mode) {
        const std::string inject_rate = std::to_string(
            g_inject_rate.load(std::memory_order_acquire));
        const std::string_view inject_args[]{inject_rate};
        ui->text(ui->user, anomaly::sdk::StringView(g_localizer.Format(
            "ui.injection_info", "Capture sample rate: {0} Hz (48 kHz standard)",
            std::span<const std::string_view>{inject_args})));
    }
    if (!injection_mode && rate > 0 && frame_bytes > 0) {
        const std::string rate_text = std::to_string(rate);
        const std::string frame_text = std::to_string(frame_bytes);
        const std::string interval_text = std::to_string(interval);
        const std::string_view arguments[]{rate_text, frame_text, interval_text};
        ui->text(ui->user, anomaly::sdk::StringView(g_localizer.Format(
            "ui.voice_info",
            "Sample rate {0} Hz, {1} bytes per frame, {2} ms interval.",
            std::span<const std::string_view>{arguments})));
    }
    ui->text(ui->user, anomaly::sdk::StringView(
        g_voice_host.load(std::memory_order_acquire)
            ? g_localizer.Text("ui.host",
                "Hosting: the broadcast bypasses the server-side filter.")
            : g_localizer.Text("ui.remote",
                "Joined: PA/radio routes may be blocked by the host's permissions.")));

    ui->separator(ui->user);

    // 音乐目录 + 刷新。
    static char dir_buffer[kMaxDirBytes + 1]{};
    {
        std::scoped_lock lock(g_music_dir_mutex);
        std::snprintf(dir_buffer, sizeof(dir_buffer), "%s", g_music_dir.c_str());
    }
    const std::string dir_label = g_localizer.Text("ui.directory", "Music directory");
    if (ui->input_text(ui->user, anomaly::sdk::StringView(dir_label),
                       dir_buffer, sizeof(dir_buffer), 0) != 0) {
        std::scoped_lock lock(g_music_dir_mutex);
        g_music_dir = dir_buffer;
        MarkSettingsDirty();
    }
    const std::string refresh_label = g_localizer.Text("ui.refresh", "Refresh list");
    if (ui->button(ui->user, anomaly::sdk::StringView(refresh_label), 0.0f, 0.0f) != 0) {
        g_refresh_pending.store(true, std::memory_order_release);
    }
    if (!g_self_available.load(std::memory_order_acquire)) {
        ui->text(ui->user, anomaly::sdk::StringView(
            g_localizer.Text("ui.self_unavailable",
                "No local audio output device available; self-listen is disabled.")));
    }

    // 曲目列表（简单筛选 + 滚动子区）。
    static char filter_buffer[64]{};
    const std::string filter_label = g_localizer.Text("ui.filter", "Filter");
    if (ui->input_text(ui->user, anomaly::sdk::StringView(filter_label),
                       filter_buffer, sizeof(filter_buffer), 0) != 0) {
        // 缓冲本身是筛选状态，无需额外动作。
    }
    const std::string tracks_label = g_localizer.Text("ui.tracks", "Tracks");
    if (ui->begin_child(ui->user, anomaly::sdk::StringView(tracks_label),
                        0.0f, 260.0f, 0) != 0) {
        const int count = g_track_count.load(std::memory_order_acquire);
        const int current = g_current_track.load(std::memory_order_acquire);
        std::vector<std::string> names;
        {
            std::scoped_lock lock(g_tracks_mutex);
            names = g_track_names;
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (filter_buffer[0] != '\0' && ui->filter_match != nullptr) {
                const AnomalyStringViewV1 filter{filter_buffer,
                    std::strlen(filter_buffer)};
                if (ui->filter_match(ui->user, filter,
                                     anomaly::sdk::StringView(names[i])) == 0) {
                    continue;
                }
            }
            const bool active = static_cast<int>(i) == current;
            std::string row = active ? "[*] " : "";
            row += names[i];
            if (ui->button(ui->user, anomaly::sdk::StringView(row), 0.0f, 0.0f) != 0) {
                g_pending_action.store(kActionPlay, std::memory_order_release);
                g_pending_track.store(static_cast<int>(i), std::memory_order_release);
            }
        }
        ui->end_child(ui->user);
    }

    // 控制行：上一首 / 播放暂停 / 停止 / 下一首。
    const std::string prev_label = g_localizer.Text("ui.prev", "Prev");
    const std::string toggle_label = play_state == kPlaying
        ? g_localizer.Text("ui.pause", "Pause")
        : g_localizer.Text("ui.play", "Play");
    const std::string stop_label = g_localizer.Text("ui.stop", "Stop");
    const std::string next_label = g_localizer.Text("ui.next", "Next");
    if (ui->button(ui->user, anomaly::sdk::StringView(prev_label), 0.0f, 0.0f) != 0) {
        g_pending_action.store(kActionPrev, std::memory_order_release);
    }
    ui->same_line(ui->user, 0.0f, 0.0f);
    const bool toggle_enabled = play_state == kPlaying || play_state == kPaused;
    if (ui->button_enabled != nullptr) {
        if (ui->button_enabled(ui->user, anomaly::sdk::StringView(toggle_label),
                               0.0f, 0.0f, toggle_enabled ? 1 : 0) != 0) {
            g_pending_action.store(kActionPause, std::memory_order_release);
        }
    } else if (toggle_enabled &&
               ui->button(ui->user, anomaly::sdk::StringView(toggle_label),
                          0.0f, 0.0f) != 0) {
        g_pending_action.store(kActionPause, std::memory_order_release);
    }
    ui->same_line(ui->user, 0.0f, 0.0f);
    if (ui->button(ui->user, anomaly::sdk::StringView(stop_label), 0.0f, 0.0f) != 0) {
        g_pending_action.store(kActionStop, std::memory_order_release);
    }
    ui->same_line(ui->user, 0.0f, 0.0f);
    if (ui->button(ui->user, anomaly::sdk::StringView(next_label), 0.0f, 0.0f) != 0) {
        g_pending_action.store(kActionNext, std::memory_order_release);
    }

    // 设置区：自听开关、双音量、路由、循环。
    int self_listen = g_self_listen.load(std::memory_order_acquire) ? 1 : 0;
    const std::string self_label = g_localizer.Text("ui.self_listen", "Self-listen");
    if (ui->checkbox(ui->user, anomaly::sdk::StringView(self_label), &self_listen) != 0 &&
        self_listen != (g_self_listen.load(std::memory_order_acquire) ? 1 : 0)) {
        g_self_listen.store(self_listen != 0, std::memory_order_release);
        MarkSettingsDirty();
    }
    int loop = g_loop.load(std::memory_order_acquire) ? 1 : 0;
    const std::string loop_label = g_localizer.Text("ui.loop", "Loop playlist");
    if (ui->checkbox(ui->user, anomaly::sdk::StringView(loop_label), &loop) != 0 &&
        loop != (g_loop.load(std::memory_order_acquire) ? 1 : 0)) {
        g_loop.store(loop != 0, std::memory_order_release);
        MarkSettingsDirty();
    }

    float send_volume = g_send_volume.load(std::memory_order_relaxed);
    const std::string send_label = g_localizer.Text("ui.send_volume", "Send volume");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView(send_label),
                         &send_volume, 0.0f, 2.0f) != 0 &&
        send_volume != g_send_volume.load(std::memory_order_relaxed)) {
        g_send_volume.store(send_volume, std::memory_order_relaxed);
        MarkSettingsDirty();
    }
    float self_volume = g_self_volume.load(std::memory_order_relaxed);
    const std::string self_volume_label =
        g_localizer.Text("ui.self_volume", "Self volume");
    if (ui->slider_float(ui->user, anomaly::sdk::StringView(self_volume_label),
                         &self_volume, 0.0f, 2.0f) != 0 &&
        self_volume != g_self_volume.load(std::memory_order_relaxed)) {
        g_self_volume.store(self_volume, std::memory_order_relaxed);
        MarkSettingsDirty();
    }

    // 路由选择（三选一互斥）。
    const int route = g_voice_route.load(std::memory_order_acquire);
    const std::string route_label = g_localizer.Text("ui.route", "Route");
    ui->text(ui->user, anomaly::sdk::StringView(route_label));
    const std::array<std::string, 3> route_names{
        g_localizer.Text("ui.route_proximity", "Proximity (nearby players only)"),
        g_localizer.Text("ui.route_pa", "PA broadcast (whole plane, needs permission)"),
        g_localizer.Text("ui.route_radio", "Radio (needs permission and quota)")};
    for (int i = 0; i < 3; ++i) {
        int selected = route == i ? 1 : 0;
        if (ui->checkbox(ui->user, anomaly::sdk::StringView(route_names[i]),
                         &selected) != 0 && selected != 0 && route != i) {
            g_voice_route.store(i, std::memory_order_release);
            MarkSettingsDirty();
        }
    }

    ui->separator(ui->user);
    const std::string help = g_localizer.Text(
        "ui.help",
        "Pick a directory, refresh, then click a track. Music is mu-law encoded "
        "and pushed through the game's voice pipeline, so everyone in the "
        "session hears it through game voice (phone quality comes from the "
        "legacy voice link). Self-listen plays the original file through your "
        "own audio device and does not affect the send path. PA and radio "
        "routes are subject to the game's own permission and loudness quota; "
        "switch to proximity when blocked.");
    if (ui->text_wrapped != nullptr) {
        ui->text_wrapped(ui->user, anomaly::sdk::StringView(help));
    } else {
        ui->text(ui->user, anomaly::sdk::StringView(help));
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
        anomaly::sdk::StringView("mayday.music"),
        anomaly::sdk::StringView("Music Player"),
        anomaly::sdk::StringView("Mayday Loader"), anomaly::sdk::StringView("1.0.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return anomaly::sdk::Ok();
}
