#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace anomaly::json {

std::string Quote(std::string_view value);
std::string Quote(std::wstring_view value);
std::string Hex(std::uintptr_t value);

}  // namespace anomaly::json
