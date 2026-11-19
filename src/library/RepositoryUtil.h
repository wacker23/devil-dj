#pragma once

// Small helpers shared by the repositories (not part of the library's public interface).

#include "library/Connection.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace devildj::library
{

inline std::optional<std::string_view> textOrNull(const std::string& text)
{
    if (text.empty())
        return std::nullopt;
    return std::string_view(text);
}

inline std::optional<int> optionalInt(const Statement& s, int column)
{
    if (const auto v = s.getOptionalInt(column))
        return static_cast<int>(*v);
    return std::nullopt;
}

inline std::int64_t nowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::string trimmed(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

} // namespace devildj::library
