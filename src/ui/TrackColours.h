#pragma once

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <cstdint>
#include <optional>

namespace devildj::ui
{

// Track colours (P2-08): Devil-DJ's own palette of eight, stored as 0xRRGGBB in tracks.color. Colours other
// programs wrote (imports) are kept and shown as they are; the menus offer these.
struct TrackColour
{
    const char* name;
    std::uint32_t rgb;
};

inline constexpr std::array<TrackColour, 8> kTrackColours{{
    {"Red", 0xE0483C},
    {"Orange", 0xF08A24},
    {"Yellow", 0xE8C832},
    {"Green", 0x4CBF5A},
    {"Aqua", 0x38C6C0},
    {"Blue", 0x3C7CE0},
    {"Purple", 0x9A5CE0},
    {"Pink", 0xE05CAE},
}};

inline juce::Colour toColour(std::uint32_t rgb)
{
    return juce::Colour(0xff000000u | rgb);
}

// The palette's name for a colour, or empty when it isn't one of ours.
inline const char* paletteName(std::uint32_t rgb)
{
    for (const auto& c : kTrackColours)
        if (c.rgb == rgb)
            return c.name;
    return "";
}

} // namespace devildj::ui
