#include "memory.hpp"

#include "anomaly/thread_local_value.hpp"

#include <Psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>

namespace anomaly {
namespace {

// HasRange 的 VirtualQuery 是每次读取一次内核调用。全量实体/actor 扫描每帧要做上万次
// 读取，光这一项就吃掉几百毫秒帧时间。这里把"已验证可读（可写）"的区域按线程缓存：
// 命中即放行，未命中仍走原来的 VirtualQuery 校验并记录结果。条目带 TTL，过期后重建，
// 因此最坏情况只是某次读取越过校验、由 ReadProcessMemory 自己失败返回，
// 与该地址校验失败的结果一致，安全性不变。
struct CachedRange {
    std::uintptr_t base{};
    std::uintptr_t end{};
    bool writable{};
};

constexpr auto kRangeCacheTtl = std::chrono::seconds(1);

struct RangeCacheState {
    std::vector<CachedRange> ranges;
    std::chrono::steady_clock::time_point stamp{};
};

// The core is manually mapped and cannot carry loader-managed static TLS.
anomaly::ThreadLocalObject<RangeCacheState> g_range_cache;

void RangeCacheRemember(
    std::vector<CachedRange>& cache, std::uintptr_t base, std::uintptr_t end,
    bool writable) {
    const auto position = std::lower_bound(
        cache.begin(), cache.end(), base,
        [](const CachedRange& range, std::uintptr_t value) { return range.base < value; });
    if (position != cache.end() && position->base == base) {
        position->writable = position->writable || writable;
        position->end = (std::max)(position->end, end);
        return;
    }
    cache.insert(position, CachedRange{base, end, writable});
}

bool HasRange(std::uintptr_t address, std::size_t size, bool write) {
    if (address == 0 || size == 0 || address > std::numeric_limits<std::uintptr_t>::max() - size) {
        return false;
    }
    const auto end = address + size;
    auto& state = g_range_cache.Get();
    auto& cache = state.ranges;
    const auto now = std::chrono::steady_clock::now();
    if (state.stamp.time_since_epoch().count() == 0 ||
        now - state.stamp > kRangeCacheTtl) {
        cache.clear();
        state.stamp = now;
    }
    const auto covering = std::upper_bound(
        cache.begin(), cache.end(), address,
        [](std::uintptr_t value, const CachedRange& range) { return value < range.base; });
    if (covering != cache.begin()) {
        const auto& range = *(covering - 1);
        if (address >= range.base && end <= range.end && (!write || range.writable)) {
            return true;
        }
    }
    auto cursor = address;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0 ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }
        const DWORD base = info.Protect & 0xff;
        const bool readable = base == PAGE_READONLY || base == PAGE_READWRITE ||
                              base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
                              base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
        const bool writable = base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
                              base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
        if (!readable || (write && !writable)) return false;
        const auto region_base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto region_end = region_base + info.RegionSize;
        if (region_end <= cursor) return false;
        if (readable) RangeCacheRemember(cache, region_base, region_end, writable);
        cursor = std::min(region_end, end);
    }
    return true;
}

template <typename T>
bool ReadLocal(std::uintptr_t address, T& output) {
    if (address == 0) return false;
    __try {
        std::memcpy(&output, reinterpret_cast<const void*>(address), sizeof(output));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::wstring BaseName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool EqualInsensitive(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() &&
           _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

struct ImageLayout final {
    std::size_t image_size{};
    std::vector<IMAGE_SECTION_HEADER> sections;
};

std::optional<ImageLayout> ReadMemoryImageLayout(const ModuleInfo& module) {
    IMAGE_DOS_HEADER dos{};
    if (!ReadLocal(module.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 ||
        module.base > std::numeric_limits<std::uintptr_t>::max() -
            static_cast<std::uintptr_t>(dos.e_lfanew)) {
        return std::nullopt;
    }
    const auto nt_address = module.base + static_cast<std::uintptr_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadLocal(nt_address, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections > 96) {
        return std::nullopt;
    }

    const auto section_address = nt_address + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        nt.FileHeader.SizeOfOptionalHeader;
    ImageLayout layout;
    layout.image_size = nt.OptionalHeader.SizeOfImage;
    layout.sections.reserve(nt.FileHeader.NumberOfSections);
    for (unsigned index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER header{};
        if (!ReadLocal(section_address + index * sizeof(header), header)) {
            return std::nullopt;
        }
        layout.sections.push_back(header);
    }
    return layout;
}

std::optional<ImageLayout> ReadFileImageLayout(const std::wstring& path) {
    if (path.empty()) return std::nullopt;
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) return std::nullopt;

    IMAGE_DOS_HEADER dos{};
    stream.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!stream || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return std::nullopt;
    }
    stream.seekg(dos.e_lfanew, std::ios::beg);
    IMAGE_NT_HEADERS64 nt{};
    stream.read(reinterpret_cast<char*>(&nt), sizeof(nt));
    if (!stream || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections > 96) {
        return std::nullopt;
    }

    const std::streamoff section_offset = static_cast<std::streamoff>(dos.e_lfanew) +
        sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    stream.seekg(section_offset, std::ios::beg);
    if (!stream) return std::nullopt;
    ImageLayout layout;
    layout.image_size = nt.OptionalHeader.SizeOfImage;
    layout.sections.resize(nt.FileHeader.NumberOfSections);
    stream.read(
        reinterpret_cast<char*>(layout.sections.data()),
        static_cast<std::streamsize>(layout.sections.size() * sizeof(IMAGE_SECTION_HEADER)));
    return stream ? std::optional<ImageLayout>(std::move(layout)) : std::nullopt;
}

}  // namespace

std::vector<ModuleInfo> EnumerateModules() {
    std::vector<HMODULE> handles(256);
    DWORD needed{};
    while (true) {
        if (!K32EnumProcessModules(
                GetCurrentProcess(), handles.data(),
                static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed)) {
            return {};
        }
        if (needed <= handles.size() * sizeof(HMODULE)) {
            handles.resize(needed / sizeof(HMODULE));
            break;
        }
        handles.resize(needed / sizeof(HMODULE) + 16);
    }

    std::vector<ModuleInfo> modules;
    modules.reserve(handles.size());
    for (const auto handle : handles) {
        MODULEINFO native{};
        std::array<wchar_t, 32768> path{};
        if (!K32GetModuleInformation(GetCurrentProcess(), handle, &native, sizeof(native))) {
            continue;
        }
        const DWORD length = K32GetModuleFileNameExW(
            GetCurrentProcess(), handle, path.data(), static_cast<DWORD>(path.size()));
        ModuleInfo module;
        module.path.assign(path.data(), length);
        module.name = BaseName(module.path);
        module.base = reinterpret_cast<std::uintptr_t>(native.lpBaseOfDll);
        module.size = native.SizeOfImage;
        modules.push_back(std::move(module));
    }
    std::sort(modules.begin(), modules.end(), [](const auto& left, const auto& right) {
        return left.base < right.base;
    });
    return modules;
}

std::optional<ModuleInfo> FindModule(std::wstring_view name) {
    const auto modules = EnumerateModules();
    if (name.empty()) {
        const auto main_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto main_module = std::find_if(modules.begin(), modules.end(), [&](const auto& module) {
            return module.base == main_base;
        });
        return main_module == modules.end() ? std::nullopt : std::optional<ModuleInfo>(*main_module);
    }
    for (const auto& module : modules) {
        if (EqualInsensitive(module.name, name) || EqualInsensitive(module.path, name)) {
            return module;
        }
    }
    return std::nullopt;
}

std::vector<SectionInfo> EnumerateSections(const ModuleInfo& module) {
    auto layout = ReadMemoryImageLayout(module);
    if (!layout) layout = ReadFileImageLayout(module.path);
    if (!layout) return {};
    const std::size_t image_size = module.size != 0 ? module.size : layout->image_size;
    std::vector<SectionInfo> sections;
    sections.reserve(layout->sections.size());
    for (const IMAGE_SECTION_HEADER& header : layout->sections) {
        std::array<char, IMAGE_SIZEOF_SHORT_NAME + 1> name{};
        std::memcpy(name.data(), header.Name, IMAGE_SIZEOF_SHORT_NAME);
        const auto offset = static_cast<std::size_t>(header.VirtualAddress);
        const auto size = static_cast<std::size_t>(header.Misc.VirtualSize);
        if (offset >= image_size) continue;
        sections.push_back({
            name.data(), module.base + offset, std::min(size, image_size - offset),
            header.Characteristics});
    }
    return sections;
}

std::vector<RegionInfo> EnumerateRegions(const ModuleInfo& module) {
    std::vector<RegionInfo> regions;
    const auto end = module.base + module.size;
    auto cursor = module.base;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0 ||
            info.RegionSize == 0) {
            break;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const auto region_end = base + info.RegionSize;
        const auto clipped_end = std::min(region_end, end);
        regions.push_back({base, clipped_end - base, info.State, info.Protect, info.Type});
        if (region_end <= cursor) {
            break;
        }
        cursor = region_end;
    }
    return regions;
}

std::vector<std::uintptr_t> ScanSection(
    const ModuleInfo& module,
    std::string_view section_name,
    const Pattern& pattern,
    std::size_t limit) {
    const auto sections = EnumerateSections(module);
    const auto found = std::find_if(sections.begin(), sections.end(), [&](const auto& section) {
        return section.name == section_name;
    });
    if (found == sections.end() || found->virtual_size == 0) {
        return {};
    }
    return ScanSection(*found, pattern, limit);
}

// 分块扫描的单次读取量。块间保留 overlap，使跨块边界的模式匹配不丢失。
constexpr std::size_t kScanChunkBytes = 64 * 1024;

std::size_t ScanChunkOverlap(std::span<const Pattern> patterns) {
    std::size_t largest = 0;
    for (const Pattern& pattern : patterns) {
        largest = (std::max)(largest, pattern.Size());
    }
    return largest > 1 ? largest - 1 : 0;
}

std::vector<std::uintptr_t> ScanSection(
    const SectionInfo& section,
    const Pattern& pattern,
    std::size_t limit) {
    std::vector<std::uintptr_t> addresses;
    if (section.virtual_size == 0 || !HasRange(section.base, section.virtual_size, false)) {
        return addresses;
    }
    // 模块可能在 HasRange 的 TTL 缓存窗口内被卸载，直接把映像内存当裸指针
    // 整段遍历无法拦截这种 AV。改为分块经 ReadMemoryInto（自带 SEH）读入本地
    // 缓冲后再匹配；每块把上一块尾部 overlap 拼进窗口，保证跨块匹配不漏。
    const std::size_t overlap = ScanChunkOverlap(std::span<const Pattern>(&pattern, 1));
    const std::size_t pattern_size = pattern.Size();
    std::vector<std::uint8_t> buffer(kScanChunkBytes + overlap);
    const auto section_end = section.base + section.virtual_size;
    auto cursor = section.base;
    std::size_t remaining = limit;
    // carry 是缓冲开头保留的上一块尾部字节数，保证跨块边界的匹配可见。
    std::size_t carry = 0;
    while (cursor < section_end && remaining > 0) {
        const std::size_t read_size = static_cast<std::size_t>(
            (std::min<std::uintptr_t>)(section_end - cursor, kScanChunkBytes));
        if (!ReadMemoryInto(cursor, buffer.data() + carry, read_size)) break;
        const std::size_t window = carry + read_size;
        const auto offsets = pattern.FindAll(
            std::span<const std::uint8_t>(buffer.data(), window), remaining);
        for (const auto offset : offsets) {
            // 完全落在 carry 区内的匹配上一块已经报告过，跳过重复项。
            if (offset + pattern_size <= carry) continue;
            addresses.push_back(cursor - carry + offset);
        }
        remaining -= offsets.size();
        const std::size_t tail = (std::min)(overlap, window);
        if (tail > 0) {
            std::memcpy(buffer.data(), buffer.data() + window - tail, tail);
        }
        carry = tail;
        cursor += read_size;
    }
    return addresses;
}

std::vector<std::vector<std::uintptr_t>> ScanSectionBatch(
    const SectionInfo& section,
    std::span<const Pattern> patterns,
    std::size_t limit) {
    std::vector<std::vector<std::uintptr_t>> addresses(patterns.size());
    if (section.virtual_size == 0 || !HasRange(section.base, section.virtual_size, false)) {
        return addresses;
    }
    const std::size_t overlap = ScanChunkOverlap(patterns);
    std::vector<std::uint8_t> buffer(kScanChunkBytes + overlap);
    const auto section_end = section.base + section.virtual_size;
    auto cursor = section.base;
    // carry 是缓冲开头保留的上一块尾部字节数，保证跨块边界的匹配可见。
    std::size_t carry = 0;
    while (cursor < section_end) {
        const std::size_t read_size = static_cast<std::size_t>(
            (std::min<std::uintptr_t>)(section_end - cursor, kScanChunkBytes));
        if (!ReadMemoryInto(cursor, buffer.data() + carry, read_size)) break;
        const std::size_t window = carry + read_size;
        const auto offsets = Pattern::FindAllBatch(
            std::span<const std::uint8_t>(buffer.data(), window), patterns, limit);
        for (std::size_t index = 0; index < offsets.size(); ++index) {
            const std::size_t pattern_size = index < patterns.size()
                ? patterns[index].Size() : 0;
            for (const auto offset : offsets[index]) {
                // 完全落在 carry 区内的匹配上一块已经报告过，跳过重复项。
                if (offset + pattern_size <= carry) continue;
                addresses[index].push_back(cursor - carry + offset);
            }
        }
        const std::size_t tail = (std::min)(overlap, window);
        if (tail > 0) {
            std::memcpy(buffer.data(), buffer.data() + window - tail, tail);
        }
        carry = tail;
        cursor += read_size;
    }
    return addresses;
}

std::optional<std::uintptr_t> ResolveRipRelative(
    std::uintptr_t instruction,
    std::size_t displacement_offset,
    std::size_t instruction_size,
    std::ptrdiff_t addend) {
    std::int32_t displacement{};
    if (!ReadLocal(instruction + displacement_offset, displacement)) {
        return std::nullopt;
    }
    return instruction + instruction_size + displacement + addend;
}

std::optional<std::vector<std::uint8_t>> ReadMemory(std::uintptr_t address, std::size_t size) {
    if (size == 0 || size > 4096) return std::nullopt;
    std::vector<std::uint8_t> bytes(size);
    if (!ReadMemoryInto(address, bytes.data(), size)) return std::nullopt;
    return bytes;
}

bool ReadMemoryInto(std::uintptr_t address, void* destination, std::size_t size) {
    if (destination == nullptr || !HasRange(address, size, false)) return false;
    __try {
        std::memcpy(destination, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteMemory(std::uintptr_t address, const void* source, std::size_t size) {
    if (source == nullptr || !HasRange(address, size, true)) return false;
    SIZE_T written{};
    const bool result = WriteProcessMemory(
                            GetCurrentProcess(), reinterpret_cast<void*>(address), source, size,
                            &written) != FALSE &&
                        written == size;
    if (result) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    return result;
}

bool PatchMemory(std::uintptr_t address, const void* source, std::size_t size) {
    if (source == nullptr || !HasRange(address, size, false)) return false;
    struct ProtectedSpan {
        void* address{};
        std::size_t size{};
        DWORD previous{};
    };
    std::vector<ProtectedSpan> spans;
    const auto end = address + size;
    auto cursor = address;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) break;
        const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        ProtectedSpan span{
            reinterpret_cast<void*>(cursor), std::min(region_end, end) - cursor, 0};
        if (!VirtualProtect(span.address, span.size, PAGE_EXECUTE_READWRITE, &span.previous)) break;
        spans.push_back(span);
        cursor += span.size;
    }
    if (cursor != end) {
        for (auto iterator = spans.rbegin(); iterator != spans.rend(); ++iterator) {
            DWORD ignored{};
            VirtualProtect(iterator->address, iterator->size, iterator->previous, &ignored);
        }
        return false;
    }
    SIZE_T written{};
    const bool wrote = WriteProcessMemory(
                           GetCurrentProcess(), reinterpret_cast<void*>(address), source, size,
                           &written) != FALSE &&
                       written == size;
    if (wrote) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    bool restored = true;
    for (auto iterator = spans.rbegin(); iterator != spans.rend(); ++iterator) {
        DWORD ignored{};
        restored = VirtualProtect(iterator->address, iterator->size, iterator->previous, &ignored) != FALSE && restored;
    }
    return wrote && restored;
}

bool ProtectMemory(std::uintptr_t address, std::size_t size, DWORD protection, DWORD& previous) {
    if (address == 0 || size == 0) return false;
    return VirtualProtect(reinterpret_cast<void*>(address), size, protection, &previous) != FALSE;
}

void* AllocateMemory(std::size_t size, DWORD protection) {
    if (size == 0) return nullptr;
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, protection);
}

bool FreeMemory(void* address) {
    return address != nullptr && VirtualFree(address, 0, MEM_RELEASE) != FALSE;
}

std::optional<std::uintptr_t> ResolvePointerChain(
    std::uintptr_t base,
    const std::ptrdiff_t* offsets,
    std::size_t count) {
    if (base == 0 || (count != 0 && offsets == nullptr) || count > 64) return std::nullopt;
    auto current = base;
    for (std::size_t index = 0; index < count; ++index) {
        std::uintptr_t next{};
        if (!ReadMemoryInto(current, &next, sizeof(next)) || next == 0) return std::nullopt;
        const auto offset = offsets[index];
        if (offset >= 0) {
            const auto magnitude = static_cast<std::uintptr_t>(offset);
            if (next > std::numeric_limits<std::uintptr_t>::max() - magnitude) return std::nullopt;
            current = next + magnitude;
        } else {
            const auto magnitude = std::uintptr_t{0} - static_cast<std::uintptr_t>(offset);
            if (next < magnitude) return std::nullopt;
            current = next - magnitude;
        }
    }
    return current;
}

std::string ProtectionName(DWORD protection) {
    if (protection == 0) return "none";
    const DWORD base = protection & 0xff;
    std::string result;
    switch (base) {
    case PAGE_NOACCESS: result = "noaccess"; break;
    case PAGE_READONLY: result = "r"; break;
    case PAGE_READWRITE: result = "rw"; break;
    case PAGE_WRITECOPY: result = "wc"; break;
    case PAGE_EXECUTE: result = "x"; break;
    case PAGE_EXECUTE_READ: result = "rx"; break;
    case PAGE_EXECUTE_READWRITE: result = "rwx"; break;
    case PAGE_EXECUTE_WRITECOPY: result = "xwc"; break;
    default: result = "unknown"; break;
    }
    if ((protection & PAGE_GUARD) != 0) result += "|guard";
    if ((protection & PAGE_NOCACHE) != 0) result += "|nocache";
    if ((protection & PAGE_WRITECOMBINE) != 0) result += "|writecombine";
    return result;
}

std::string StateName(DWORD state) {
    switch (state) {
    case MEM_COMMIT: return "commit";
    case MEM_RESERVE: return "reserve";
    case MEM_FREE: return "free";
    default: return "unknown";
    }
}

std::string TypeName(DWORD type) {
    switch (type) {
    case MEM_IMAGE: return "image";
    case MEM_MAPPED: return "mapped";
    case MEM_PRIVATE: return "private";
    default: return "unknown";
    }
}

}  // namespace anomaly
