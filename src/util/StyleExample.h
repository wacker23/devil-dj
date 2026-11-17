#pragma once

#include <cstdint>

namespace devildj::util
{

// Example file used to check that .clang-format runs clean (P0-01). Safe to delete once real code exists.
constexpr std::int32_t kExampleConstant = 42;

class StyleExample
{
public:
    explicit StyleExample(int value)
        : value(value)
    {
    }

    int getValue() const { return value; }

    int clampTo(int low, int high) const
    {
        if (value < low)
            return low;

        return value > high ? high : value;
    }

private:
    int value = 0;
};

} // namespace devildj::util
