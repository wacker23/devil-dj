// P5-13: the code deck's buffers persist (Documents\Devil-DJ\Code: one .js file per tab, tabs.json for the order).

#include "app/CodeBuffers.h"

#include <catch2/catch_test_macros.hpp>

using devildj::app::CodeBuffers;

namespace
{
// Text with an umlaut and Korean, from UTF-8 (a plain literal would not be read as UTF-8 by juce::String).
const juce::String kUnicode = juce::String::fromUTF8("note(\"c3 e3 g3\").s(\"saw\") // \xc3\xbcmlaut and \xed\x95\x9c\xea\xb5\xad\xec\x96\xb4\n");

struct TempFolder
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-code", "");
    ~TempFolder() { dir.deleteRecursively(); }
};
} // namespace

TEST_CASE("Code buffers: the first start gets a starter buffer, saved as a file", "[app][codebeat][buffers]")
{
    TempFolder t;
    CodeBuffers b(t.dir);
    b.load();
    REQUIRE(b.count() == 1);
    CHECK(b.at(0).name == "Code 1");
    CHECK(b.at(0).text == CodeBuffers::starter());
    CHECK(b.save().isEmpty());
    CHECK(t.dir.getChildFile("Code 1.js").loadFileAsString() == CodeBuffers::starter());
    CHECK(t.dir.getChildFile("tabs.json").existsAsFile());
}

TEST_CASE("Code buffers: text, order, names and the tab shown survive a restart", "[app][codebeat][buffers]")
{
    TempFolder t;
    {
        CodeBuffers b(t.dir);
        b.load();
        b.setText(0, "s(\"bd*4\")\n");
        const int drums = b.add("drums", "s(\"hh*8\")");
        CHECK(drums == 1);
        const int third = b.add();
        CHECK(b.at(third).name == "Code 2"); // "Code 1" is taken
        b.setText(third, kUnicode);
        CHECK(b.rename(0, "intro").isEmpty());
        b.setCurrent(1);
        CHECK(b.save().isEmpty());
    }
    CodeBuffers again(t.dir);
    again.load();
    REQUIRE(again.count() == 3);
    CHECK(again.at(0).name == "intro");
    CHECK(again.at(0).text == "s(\"bd*4\")\n");
    CHECK(again.at(1).name == "drums");
    CHECK(again.at(1).text == "s(\"hh*8\")");
    CHECK(again.at(2).text == kUnicode);
    CHECK(again.current() == 1);
    CHECK_FALSE(t.dir.getChildFile("Code 1.js").existsAsFile()); // renamed, not copied
    CHECK(t.dir.getChildFile("intro.js").existsAsFile());
}

TEST_CASE("Code buffers: names are legal and unique; files dropped in the folder show up", "[app][codebeat][buffers]")
{
    TempFolder t;
    CodeBuffers b(t.dir);
    b.load();
    CHECK(b.rename(0, "  ").isNotEmpty());
    CHECK(b.rename(0, "a/b:c?") .isEmpty());
    CHECK_FALSE(b.at(0).name.containsAnyOf("/:?"));
    b.add("bass");
    CHECK(b.rename(1, "BASS").isEmpty()); // only its own name differs in case
    b.add("bass");
    CHECK(b.at(2).name == "bass 2");
    CHECK(b.rename(2, "Bass").isNotEmpty()); // taken
    CHECK(b.rename(2, "lead.js").isEmpty());
    CHECK(b.at(2).name == "lead");
    CHECK(b.save().isEmpty());

    // A file copied into the folder: a tab after the known ones.
    t.dir.getChildFile("zz from outside.js").replaceWithText("s(\"cp\")");
    CodeBuffers again(t.dir);
    again.load();
    REQUIRE(again.count() == 4);
    CHECK(again.at(3).name == "zz from outside");
}

TEST_CASE("Code buffers: closing moves the file to Code\\Closed; the last one stays", "[app][codebeat][buffers]")
{
    TempFolder t;
    CodeBuffers b(t.dir);
    b.load();
    b.add("keep", "1");
    b.add("go", "s(\"bd\") // last words");
    b.setCurrent(2);
    CHECK(b.save().isEmpty());
    b.setText(2, "s(\"bd\") // last words, edited");
    b.remove(2);
    CHECK(b.count() == 2);
    CHECK(b.current() == 1);
    CHECK_FALSE(t.dir.getChildFile("go.js").existsAsFile());
    CHECK(t.dir.getChildFile("Closed").getChildFile("go.js").loadFileAsString() == "s(\"bd\") // last words, edited");
    b.remove(0);
    b.remove(0); // the last one is never closed
    CHECK(b.count() == 1);
    CHECK(b.save().isEmpty());
    CodeBuffers again(t.dir);
    again.load();
    CHECK(again.count() == 1);
    CHECK(again.at(0).name == "keep");
}
