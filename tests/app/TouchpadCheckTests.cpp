// The touchpad tip: Windows mutes a precision touchpad while keys are pressed unless it is set to Most sensitive.

#include "app/TouchpadCheck.h"

#include <catch2/catch_test_macros.hpp>

using namespace devildj::app;

TEST_CASE("Touchpad tip: shown only when Windows mutes the touchpad while typing, until dismissed", "[app][touchpad]")
{
    // 0 Most sensitive (never muted), 1 High, 2 Medium (the Windows default), 3 Low; none: no precision touchpad.
    CHECK_FALSE(touchpad::mutesWhileTyping(std::nullopt));
    CHECK_FALSE(touchpad::mutesWhileTyping(0));
    CHECK(touchpad::mutesWhileTyping(1));
    CHECK(touchpad::mutesWhileTyping(2));
    CHECK(touchpad::mutesWhileTyping(3));

    CHECK(touchpad::showAtStart(2, false));
    CHECK_FALSE(touchpad::showAtStart(2, true)); // "Don't show again"
    CHECK_FALSE(touchpad::showAtStart(0, false));
    CHECK_FALSE(touchpad::showAtStart(std::nullopt, false));
}

TEST_CASE("Touchpad tip: the text names the setting and how to change it", "[app][touchpad]")
{
    const auto muted = touchpad::tipText(2);
    CHECK(muted.contains("Medium sensitivity"));
    CHECK(muted.contains("Touchpad sensitivity: Most sensitive"));
    CHECK(muted.contains("USB mouse"));
    CHECK(touchpad::tipText(0).contains("doesn't mute it"));
    CHECK(touchpad::tipText(std::nullopt).contains("doesn't report a precision touchpad"));
    CHECK(touchpad::sensitivityName(3) == "Low sensitivity");
    CHECK(touchpad::sensitivityName(7) == "unknown");
}

TEST_CASE("Touchpad tip: this computer's setting can be read (or is absent) without failing", "[app][touchpad]")
{
    const auto s = touchpad::readSensitivity();
    if (s.has_value())
        CHECK(*s >= 0);
    SUCCEED("sensitivity: " << (s ? touchpad::sensitivityName(*s).toStdString() : std::string("no precision touchpad")));
}
