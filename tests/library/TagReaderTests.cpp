#include "TestData.h"
#include "library/TagReader.h"

#include <catch2/catch_test_macros.hpp>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

using devildj::library::parseBpmTag;
using devildj::library::readBasicTags;

TEST_CASE("BPM tag parsing", "[library][tags]")
{
    CHECK(parseBpmTag("128") == 128.0);
    CHECK(parseBpmTag(" 127.98") == 127.98);
    CHECK(parseBpmTag("174 BPM") == 174.0);
    CHECK(parseBpmTag("") == 0.0);
    CHECK(parseBpmTag("fast") == 0.0);
    CHECK(parseBpmTag("0") == 0.0);
    CHECK(parseBpmTag("12000") == 0.0);
}

TEST_CASE("Tags: untagged, tagged and missing files", "[library][tags]")
{
    const auto source = devildj::test::generatedFile("formats/ref.wav");
    if (!source.existsAsFile())
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");

    const auto untagged = readBasicTags(source.getFullPathName().toWideCharPointer());
    CHECK(untagged.title.empty());
    CHECK(untagged.artist.empty());
    CHECK(untagged.bpm == 0.0);

    // Tag a copy (non-ASCII text and path, like a real library).
    const juce::TemporaryFile temp(juce::String::fromUTF8("Devil-DJ 테스트.wav"));
    REQUIRE(source.copyFileTo(temp.getFile()));
    {
        TagLib::FileRef ref(temp.getFile().getFullPathName().toWideCharPointer());
        REQUIRE_FALSE(ref.isNull());
        ref.tag()->setTitle(TagLib::String("Night Drive — Extended", TagLib::String::UTF8));
        ref.tag()->setArtist(TagLib::String("Kim Seo-yeon 김서연", TagLib::String::UTF8));
        auto properties = ref.properties();
        properties.replace("BPM", TagLib::StringList(TagLib::String("126")));
        ref.setProperties(properties);
        REQUIRE(ref.save());
    }
    const auto tagged = readBasicTags(temp.getFile().getFullPathName().toWideCharPointer());
    CHECK(tagged.title == "Night Drive — Extended");
    CHECK(tagged.artist == "Kim Seo-yeon 김서연");
    CHECK(tagged.bpm == 126.0);

    const auto missing = readBasicTags(L"C:\\definitely\\not\\here.mp3");
    CHECK(missing.title.empty());
}
