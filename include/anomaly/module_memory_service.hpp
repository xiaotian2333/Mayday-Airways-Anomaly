#pragma once

#include "memory.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anomaly {

class ModuleMemoryService final {
public:
    [[nodiscard]] std::vector<anomaly::ModuleInfo> EnumerateModules() const;
    [[nodiscard]] std::optional<anomaly::ModuleInfo> FindModule(
        std::wstring_view name) const;
    [[nodiscard]] std::vector<anomaly::SectionInfo> EnumerateSections(
        const anomaly::ModuleInfo& module) const;
    [[nodiscard]] std::vector<anomaly::RegionInfo> EnumerateRegions(
        const anomaly::ModuleInfo& module) const;
    [[nodiscard]] std::vector<std::uintptr_t> ScanSection(
        const anomaly::ModuleInfo& module,
        std::string_view section_name,
        const anomaly::Pattern& pattern,
        std::size_t limit) const;
    [[nodiscard]] std::vector<std::uintptr_t> ScanSection(
        const anomaly::SectionInfo& section,
        const anomaly::Pattern& pattern,
        std::size_t limit) const;
    [[nodiscard]] std::vector<std::vector<std::uintptr_t>> ScanSectionBatch(
        const anomaly::SectionInfo& section,
        std::span<const anomaly::Pattern> patterns,
        std::size_t limit) const;

    [[nodiscard]] std::optional<std::uintptr_t> ResolveRipRelative(
        std::uintptr_t instruction,
        std::size_t displacement_offset,
        std::size_t instruction_size,
        std::ptrdiff_t addend) const;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadMemory(
        std::uintptr_t address, std::size_t size) const;
    [[nodiscard]] bool ReadMemoryInto(
        std::uintptr_t address, void* destination, std::size_t size) const;
    [[nodiscard]] bool WriteMemory(
        std::uintptr_t address, const void* source, std::size_t size) const;
    [[nodiscard]] bool PatchMemory(
        std::uintptr_t address, const void* source, std::size_t size) const;
    [[nodiscard]] bool ProtectMemory(
        std::uintptr_t address,
        std::size_t size,
        DWORD protection,
        DWORD& previous) const;
    [[nodiscard]] void* AllocateMemory(
        std::size_t size, DWORD protection = PAGE_READWRITE) const;
    [[nodiscard]] bool FreeMemory(void* address) const;
    [[nodiscard]] std::optional<std::uintptr_t> ResolvePointerChain(
        std::uintptr_t base,
        const std::ptrdiff_t* offsets,
        std::size_t count) const;

    [[nodiscard]] std::string ProtectionName(DWORD protection) const;
    [[nodiscard]] std::string StateName(DWORD state) const;
    [[nodiscard]] std::string TypeName(DWORD type) const;
};

}  // namespace anomaly
