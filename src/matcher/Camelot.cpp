#include "matcher/Camelot.h"

#include "util/MusicalKey.h"

#include <cctype>
#include <cstdlib>
#include <string_view>

namespace devildj::matcher
{

int camelotNumber(int key) noexcept
{
    return util::camelotNumber(key);
}

char camelotLetter(int key) noexcept
{
    return util::isValidKey(key) ? (key >= 12 ? 'A' : 'B') : '\0';
}

std::string camelotCode(int key)
{
    return util::camelotCode(key);
}

int camelotDelta(int keyA, int keyB) noexcept
{
    int d = (camelotNumber(keyB) - camelotNumber(keyA)) % 12;
    if (d > 6)
        d -= 12;
    if (d < -6)
        d += 12;
    return d;
}

KeyRelation relation(int a, int b) noexcept
{
    if (!util::isValidKey(a) || !util::isValidKey(b))
        return KeyRelation::Other;
    const int d = camelotDelta(a, b);
    const bool same = camelotLetter(a) == camelotLetter(b);
    if (d == 0)
        return same ? KeyRelation::Same : KeyRelation::Relative;
    if (d == 1 || d == -1)
        return same ? KeyRelation::Adjacent : KeyRelation::Diagonal;
    if (!same)
        return KeyRelation::Other;
    if (d == 2)
        return KeyRelation::EnergyBoost;
    if (d == -5)
        return KeyRelation::SemitoneUp;
    if (d == -2)
        return KeyRelation::TwoDown;
    if (d == 5)
        return KeyRelation::SemitoneDown;
    return KeyRelation::Other;
}

const char* HarmonicTable::name(KeyRelation r) noexcept
{
    switch (r)
    {
        case KeyRelation::Same:
            return "same";
        case KeyRelation::Adjacent:
            return "adjacent";
        case KeyRelation::Relative:
            return "relative";
        case KeyRelation::Diagonal:
            return "diagonal";
        case KeyRelation::EnergyBoost:
            return "energyBoost";
        case KeyRelation::SemitoneUp:
            return "semitoneUp";
        case KeyRelation::TwoDown:
            return "twoDown";
        case KeyRelation::SemitoneDown:
            return "semitoneDown";
        case KeyRelation::Other:
            return "other";
        case KeyRelation::Count:
            break;
    }
    return "";
}

bool HarmonicTable::load(const std::string& json)
{
    // A flat object of "name": number - small enough to read without a JSON library.
    const auto open = json.find('{');
    if (open == std::string::npos || json.find('}', open) == std::string::npos)
        return false;
    for (int i = 0; i < static_cast<int>(KeyRelation::Count); ++i)
    {
        const auto r = static_cast<KeyRelation>(i);
        const std::string key = std::string("\"") + name(r) + "\"";
        auto at = json.find(key);
        if (at == std::string::npos)
            continue;
        at = json.find(':', at + key.size());
        if (at == std::string::npos)
            continue;
        const char* begin = json.c_str() + at + 1;
        char* end = nullptr;
        const double v = std::strtod(begin, &end);
        if (end != begin && v >= 0.0 && v <= 1.0)
            score[static_cast<std::size_t>(i)] = v;
    }
    return true;
}

} // namespace devildj::matcher
