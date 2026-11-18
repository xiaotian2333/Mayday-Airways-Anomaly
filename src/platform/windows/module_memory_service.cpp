#include "anomaly/module_memory_service.hpp"

namespace anomaly {

std::vector<anomaly::ModuleInfo> ModuleMemoryService::EnumerateModules() const {
    return anomaly::EnumerateModules();
}

std::optional<anomaly::ModuleInfo> ModuleMemoryService::FindModule(
    std::wstring_view name) const {
    return anomaly::FindModule(name);
}

std::vector<anomaly::SectionInfo> ModuleMemoryService::EnumerateSections(
    const anomaly::ModuleInfo& module) const {
    return anomaly::EnumerateSections(module);
}

std::vector<anomaly::RegionInfo> ModuleMemoryService::EnumerateRegions(
    const anomaly::ModuleInfo& module) const {
    return anomaly::EnumerateRegions(module);
}

std::vector<std::uintptr_t> ModuleMemoryService::ScanSection(
    const anomaly::ModuleInfo& module,
    std::string_view section_name,
    const anomaly::Pattern& pattern,
    std::size_t limit) const {
    return anomaly::ScanSection(module, section_name, pattern, limit);
}

std::vector<std::uintptr_t> ModuleMemoryService::ScanSection(
    const anomaly::SectionInfo& section,
    const anomaly::Pattern& pattern,
    std::size_t limit) const {
    return anomaly::ScanSection(section, pattern, limit);
}

std::vector<std::vector<std::uintptr_t>> ModuleMemoryService::ScanSectionBatch(
    const anomaly::SectionInfo& section,
    std::span<const anomaly::Pattern> patterns,
    std::size_t limit) const {
    return anomaly::ScanSectionBatch(section, patterns, limit);
}

std::optional<std::uintptr_t> ModuleMemoryService::ResolveRipRelative(
    std::uintptr_t instruction,
    std::size_t displacement_offset,
    std::size_t instruction_size,
    std::ptrdiff_t addend) const {
    return anomaly::ResolveRipRelative(
        instruction, displacement_offset, instruction_size, addend);
}

std::optional<std::vector<std::uint8_t>> ModuleMemoryService::ReadMemory(
    std::uintptr_t address, std::size_t size) const {
    return anomaly::ReadMemory(address, size);
}

bool ModuleMemoryService::ReadMemoryInto(
    std::uintptr_t address, void* destination, std::size_t size) const {
    return anomaly::ReadMemoryInto(address, destination, size);
}

bool ModuleMemoryService::WriteMemory(
    std::uintptr_t address, const void* source, std::size_t size) const {
    return anomaly::WriteMemory(address, source, size);
}

bool ModuleMemoryService::PatchMemory(
    std::uintptr_t address, const void* source, std::size_t size) const {
    return anomaly::PatchMemory(address, source, size);
}

bool ModuleMemoryService::ProtectMemory(
    std::uintptr_t address,
    std::size_t size,
    DWORD protection,
    DWORD& previous) const {
    return anomaly::ProtectMemory(address, size, protection, previous);
}

void* ModuleMemoryService::AllocateMemory(std::size_t size, DWORD protection) const {
    return anomaly::AllocateMemory(size, protection);
}

bool ModuleMemoryService::FreeMemory(void* address) const {
    return anomaly::FreeMemory(address);
}

std::optional<std::uintptr_t> ModuleMemoryService::ResolvePointerChain(
    std::uintptr_t base,
    const std::ptrdiff_t* offsets,
    std::size_t count) const {
    return anomaly::ResolvePointerChain(base, offsets, count);
}

std::string ModuleMemoryService::ProtectionName(DWORD protection) const {
    return anomaly::ProtectionName(protection);
}

std::string ModuleMemoryService::StateName(DWORD state) const {
    return anomaly::StateName(state);
}

std::string ModuleMemoryService::TypeName(DWORD type) const {
    return anomaly::TypeName(type);
}

}  // namespace anomaly
