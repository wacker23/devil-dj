// P1-17: keymap overlay. Done when: overlay reflects user rebinding.

#include "ui/KeyboardController.h"
#include "ui/KeymapOverlay.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace devildj::ui;

TEST_CASE("Key names and chords round-trip for the whole default map", "[ui][keymap]")
{
    for (const auto& b : defaultKeymap())
    {
        const auto text = describeChord(b.chord);
        const auto back = parseChord(text);
        INFO(text);
        REQUIRE(back);
        CHECK(*back == b.chord);
    }
    CHECK(parseChord("ctrl+shift+n") == KeyChord{scan::N, mods::Ctrl | mods::Shift});
    CHECK(parseChord("Shift+,") == KeyChord{scan::Comma, mods::Shift});
    CHECK(parseChord("pageup") == KeyChord{scan::PageUp, mods::None});
    CHECK_FALSE(parseChord("Hyper+Z"));
    CHECK_FALSE(parseChord("Ctrl+"));
    CHECK_FALSE(parseChord("NoSuchKey"));
}

TEST_CASE("User keymap overrides move, unbind and report problems", "[ui][keymap]")
{
    std::vector<std::string> errors;
    const auto map = applyKeymapOverrides(defaultKeymap(), R"({
        "version": 1,
        "bindings": [
            { "action": "deckA.Cue", "key": "Ctrl+Shift+Z" },
            { "action": "deckB.Play", "key": "Z" },
            { "action": "global.Quantize", "key": "" },
            { "action": "deckA.Nope", "key": "Q" },
            { "action": "deckA.Play", "key": "Ctrl+Hyper" },
            { "action": "deckA.Play", "key": "X", "context": "kitchen" }
        ]})",
                                          errors);
    CHECK(errors.size() == 3);
    ActionDispatcher d;
    KeyboardController k(d, map);
    const auto at = [&](std::uint16_t code, std::uint8_t m) { return k.lookup({code, m}, KeyContext::Main); };
    CHECK(at(scan::Z, mods::Ctrl | mods::Shift) == deckCommand(0, DeckAction::Cue)); // moved here
    CHECK(at(scan::Z, mods::None) == deckCommand(1, DeckAction::Play));              // took Z from deck A cue
    CHECK_FALSE(at(scan::Comma, mods::None));                                        // deck B play left ","
    CHECK_FALSE(at(scan::Backquote, mods::None));                                    // quantize unbound
    CHECK(at(scan::Z, mods::Shift) == deckCommand(0, DeckAction::JumpToStart));      // untouched
    CHECK(findConflicts(map).empty());

    std::vector<std::string> bad;
    applyKeymapOverrides(defaultKeymap(), "{ not json", bad);
    CHECK(bad.size() == 1);
    std::vector<std::string> none;
    applyKeymapOverrides(defaultKeymap(), exampleKeymapJson(), none);
    CHECK(none.empty()); // the example file we write is valid
}

TEST_CASE("Overlay labels: what each key does alone, with Shift and with Ctrl", "[ui][keymap]")
{
    const auto labels = KeymapOverlay::labelsFor(defaultKeymap());
    const auto& z = labels.at(scan::Z);
    CHECK(z.plain == "Cue");
    CHECK(z.shift == "Start");
    CHECK(z.deck == 0);
    const auto& a = labels.at(scan::A);
    CHECK(a.plain == "Pad 5");
    CHECK(a.ctrl == "Kill LO");
    CHECK(labels.at(scan::M).deck == 1);
    CHECK(labels.at(scan::G).plain == "XF <");
    CHECK(labels.at(scan::G).deck == -1);
    CHECK(labels.at(scan::Space).plain.empty());     // Space alone is deliberately unbound in the main view
    CHECK(labels.at(scan::Space).ctrl == "Preview"); // Ctrl+Space previews the selected track
}

TEST_CASE("Every bound key appears on the drawn keyboard", "[ui][keymap]")
{
    std::set<std::uint16_t> drawn;
    for (const auto& key : KeymapOverlay::layout())
        if (key.scanCode != 0)
            drawn.insert(key.scanCode);
    for (const auto& b : defaultKeymap())
    {
        INFO(describeChord(b.chord));
        CHECK(drawn.count(b.chord.scanCode) == 1);
    }
}

TEST_CASE("Overlay reflects a user rebinding (Done when)", "[ui][keymap]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ActionDispatcher d;
    KeyboardController k(d);
    KeymapOverlay overlay;
    overlay.setSize(1280, 520);
    overlay.setBindings(k.currentBindings());
    CHECK(overlay.labels().at(scan::Z).plain == "Cue");

    // What Options > Reload keymap does with a keymap.json that moves deck A's cue to C.
    std::vector<std::string> errors;
    k.setBindings(applyKeymapOverrides(defaultKeymap(), R"({"bindings":[{"action":"deckA.Cue","key":"C"}]})", errors));
    overlay.setBindings(k.currentBindings());
    REQUIRE(errors.empty());
    CHECK(overlay.labels().at(scan::C).plain == "Cue");
    CHECK(overlay.labels().at(scan::Z).plain.empty());
    CHECK(overlay.labels().at(scan::Z).shift == "Start"); // the rest of Z is unchanged
    CHECK(overlay.createComponentSnapshot(overlay.getLocalBounds()).getWidth() == 1280);
}

TEST_CASE("Keymap overlay screenshot", "[.screenshot]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    KeymapOverlay overlay;
    overlay.setSize(1280, 560);
    overlay.setBindings(defaultKeymap());
    overlay.setStatus("default keys");
    juce::Image image(juce::Image::ARGB, 1280, 560, true);
    {
        juce::Graphics g(image);
        g.fillAll(juce::Colour(0xff16161b));
        overlay.paintEntireComponent(g, false);
    }
    const auto out =
        juce::File(juce::String::fromUTF8(DEVILDJ_SOURCE_DIR)).getChildFile("docs/images/p1-17-keymap.png");
    out.deleteFile();
    juce::FileOutputStream stream(out);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(image, stream));
}
