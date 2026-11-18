#pragma once

#include <string>
#include <utility>
#include <variant>

namespace devildj::util
{

// Value-or-error return type for worker functions (ARCHITECTURE §12: no exceptions cross threads).
// Error is a human-readable message (shown in logs / toasts).
template <typename T>
class Result
{
public:
    static Result ok(T value) { return Result(std::in_place_index<0>, std::move(value)); }
    static Result fail(std::string message) { return Result(std::in_place_index<1>, std::move(message)); }

    bool isOk() const noexcept { return data.index() == 0; }
    explicit operator bool() const noexcept { return isOk(); }

    T& value() & { return std::get<0>(data); }
    const T& value() const& { return std::get<0>(data); }
    T&& value() && { return std::get<0>(std::move(data)); }

    // Empty when isOk().
    const std::string& error() const noexcept
    {
        static const std::string none;
        return isOk() ? none : std::get<1>(data);
    }

private:
    template <std::size_t I, typename U>
    Result(std::in_place_index_t<I> tag, U&& v)
        : data(tag, std::forward<U>(v))
    {
    }

    std::variant<T, std::string> data;
};

} // namespace devildj::util
