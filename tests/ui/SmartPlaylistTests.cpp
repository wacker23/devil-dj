// P2-07: smart playlists in the browser - the rule editor and live updating.

#include "library/Connection.h"
#include "library/Database.h"
#include "library/PlaylistRepository.h"
#include "library/SmartRules.h"
#include "library/TrackRepository.h"
#include "ui/LibraryBrowser.h"
#include "ui/SmartPlaylistEditor.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>

using namespace devildj;
namespace fs = std::filesystem;
using library::RuleField;
using library::RuleOp;
using Editor = ui::SmartPlaylistEditor;

TEST_CASE("Smart playlist editor: values as typed", "[ui][smart]")
{
    CHECK(Editor::parseValue(RuleField::Time, "5:30") == 330.0);
    CHECK(Editor::parseValue(RuleField::Time, "200") == 200.0);
    CHECK_FALSE(Editor::parseValue(RuleField::Time, "5:").has_value());
    CHECK(Editor::parseValue(RuleField::Colour, "#E03030") == static_cast<double>(0xE03030));
    CHECK(Editor::parseValue(RuleField::Colour, "e03030") == static_cast<double>(0xE03030));
    CHECK_FALSE(Editor::parseValue(RuleField::Colour, "#E030").has_value());
    CHECK(Editor::parseValue(RuleField::Bpm, "127,5") == 127.5);
    CHECK_FALSE(Editor::parseValue(RuleField::Bpm, "fast").has_value());
    CHECK_FALSE(Editor::parseValue(RuleField::Bpm, " ").has_value());
    CHECK(Editor::valueText(RuleField::Time, 330.0) == "5:30");
    CHECK(Editor::valueText(RuleField::Colour, 0xE03030) == "#E03030");
    CHECK(Editor::valueText(RuleField::Bpm, 128.0) == "128");
    CHECK(Editor::valueText(RuleField::Bpm, 127.5) == "127.5");
}

TEST_CASE("Smart playlist editor: rows, rules and the live count", "[ui][smart]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    library::SmartRules seen;
    int calls = 0;
    library::SmartRules start;
    start.rules = {{RuleField::Bpm, RuleOp::Between, {}, 120.0, 128.0}, {RuleField::Genre, RuleOp::Is, "House"}};
    Editor editor("Warm-up", start,
                  [&](const library::SmartRules& r)
                  {
                      seen = r;
                      ++calls;
                      return static_cast<int>(r.rules.size()) * 10; // a stand-in count
                  });
    editor.setSize(720, 420);
    CHECK(editor.numRules() == 2);
    CHECK(editor.rules() == start); // what came in is what goes out
    CHECK(editor.countText() == "20 tracks match");
    CHECK(editor.canSave());

    editor.setMatchAll(false);
    CHECK_FALSE(editor.rules().matchAll);
    CHECK_FALSE(seen.matchAll); // the count follows every edit

    // An empty new row: shown, not counted, and Save waits for it.
    editor.addRule();
    CHECK(editor.numRules() == 3);
    CHECK(editor.rules().rules.size() == 2);
    CHECK(editor.countText().contains("fill in or remove"));
    CHECK_FALSE(editor.canSave());
    editor.removeRule(2);
    CHECK(editor.canSave());
    editor.removeRule(0);
    CHECK(editor.rules().rules.size() == 1);
    CHECK(editor.rules().rules[0].field == RuleField::Genre);

    // Saving hands over the name and the rules.
    juce::String savedName;
    library::SmartRules saved;
    editor.onSave = [&](const juce::String& n, const library::SmartRules& r)
    {
        savedName = n;
        saved = r;
    };
    editor.save();
    CHECK(savedName == "Warm-up");
    CHECK(saved == editor.rules());

    // Every field / operator pair can be set up and read back.
    Editor all("All", {}, nullptr);
    all.setSize(720, 420);
    all.removeRule(0);
    int n = 0;
    for (const auto f : {RuleField::Title, RuleField::Tag, RuleField::Bpm, RuleField::Key, RuleField::Rating,
                         RuleField::Colour, RuleField::Time, RuleField::DateAdded, RuleField::LastPlayed})
        for (const auto o : library::operatorsFor(f))
        {
            library::Rule r{f, o, "x", 3.0, 5.0};
            if (o == RuleOp::IsEmpty || o == RuleOp::IsNotEmpty)
                r = {f, o};
            else if (!library::isTextField(f))
                r.text.clear();
            if (o != RuleOp::Between)
                r.to = 0.0;
            if (library::isTextField(f))
                r.value = 0.0;
            if (o == RuleOp::IsEmpty || o == RuleOp::IsNotEmpty)
                r.value = 0.0;
            all.addRule(r);
            ++n;
            const auto back = all.rules();
            REQUIRE(static_cast<int>(back.rules.size()) == n);
            INFO(library::fieldName(f) << " " << library::opName(o));
            CHECK(back.rules.back().field == r.field);
            CHECK(back.rules.back().op == r.op);
            CHECK(back.rules.back().text == r.text);
            CHECK(back.rules.back().value == r.value);
            CHECK(back.rules.back().to == r.to);
        }
    CHECK(all.countText() == "These rules can't be used"); // no counter given
}

TEST_CASE("Smart playlists: created, shown and always up to date", "[ui][smart]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto dir = fs::temp_directory_path() /
                     ("devildj-smartui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto db = std::move(library::Database::open(dir / "library.db")).value();
    auto reader = std::move(db->openReader()).value();
    const auto add = [&](const char* title, double bpm, const char* genre)
    {
        std::int64_t id = 0;
        db->writeAndWait(
            [&](library::Connection& c)
            {
                library::Track t;
                t.path = std::string("C:\\Music\\") + title + ".mp3";
                t.contentHash = t.path;
                t.title = title;
                t.bpm = bpm;
                t.genre = genre;
                id = library::TrackRepository(c).add(t).value();
            });
        return id;
    };
    const auto a = add("Alpha", 122.0, "House");
    add("Bravo", 140.0, "Techno");
    const auto c = add("Charlie", 126.0, "House");
    {
        ui::LibraryBrowser browser(db.get(), juce::File());
        browser.setBounds(0, 0, 1200, 400);
        const auto settle = [&]
        {
            db->flush();
            browser.handlePendingLibraryChanges();
        };

        library::SmartRules rules;
        rules.rules = {{RuleField::Bpm, RuleOp::Between, {}, 120.0, 128.0}};
        browser.createSmartPlaylist("Warm-up BPM", rules);
        settle();
        const auto all = library::PlaylistRepository(*reader).all();
        REQUIRE(all.size() == 1);
        const auto id = all[0].id;
        CHECK(all[0].kind == library::PlaylistKind::Smart);
        CHECK(library::parseRules(all[0].ruleJson).value() == rules);
        CHECK(browser.treeRowNames().joinIntoString("|") ==
              "Collection|Playlists|Smart playlists|Warm-up BPM|History|Folders");

        browser.showPlaylist(id);
        CHECK(browser.view() == ui::LibraryBrowser::View::SmartPlaylist);
        CHECK(browser.model()->size() == 2);
        CHECK(browser.model()->rowOfTrack(a) >= 0);
        CHECK(browser.model()->rowOfTrack(c) >= 0);
        CHECK_FALSE(browser.canReorder()); // its order isn't stored
        CHECK(browser.menuForTree(id, ui::LibraryBrowser::View::SmartPlaylist).getNumItems() == 2);
        CHECK(browser.menuForTree(0, ui::LibraryBrowser::View::SmartPlaylist).getNumItems() == 1);

        // Live: a new matching track appears, a changed one leaves.
        const auto d = add("Delta", 124.0, "Disco");
        settle();
        CHECK(browser.model()->size() == 3);
        CHECK(browser.model()->rowOfTrack(d) >= 0);
        db->writeAndWait(
            [&](library::Connection& conn)
            {
                library::TrackRepository repo(conn);
                auto t = *repo.get(a);
                t.bpm = 150.0;
                CHECK(repo.update(t));
            });
        settle();
        CHECK(browser.model()->size() == 2);
        CHECK(browser.model()->rowOfTrack(a) < 0);

        // Edited rules apply at once (and the search filter still works on top).
        rules.rules.push_back({RuleField::Genre, RuleOp::Is, "House"});
        browser.updateSmartPlaylist(id, "Warm-up house", rules);
        settle();
        CHECK(browser.treeRowNames().contains("Warm-up house"));
        CHECK(browser.view() == ui::LibraryBrowser::View::SmartPlaylist);
        CHECK(browser.model()->size() == 1);
        CHECK(browser.model()->trackIdAt(0) == c);
        ui::TrackFilter f;
        f.text = "zzz";
        browser.setFilter(f);
        CHECK(browser.model()->size() == 0);
        CHECK(browser.countText() == "0 of 1 tracks");
        browser.clearFilters();

        // The editor's live count uses the same query.
        CHECK(browser.model()->countMatching(rules) == 1);
        library::SmartRules any = rules;
        any.matchAll = false;
        CHECK(browser.model()->countMatching(any) == 3); // Charlie, Delta (BPM) and Alpha (House)
        library::SmartRules broken;
        broken.rules = {{RuleField::Bpm, RuleOp::Contains}};
        CHECK(browser.model()->countMatching(broken) == -1);

        // Broken rules stored in the database (another program, a hand edit) show nothing and say why.
        db->writeAndWait([&](library::Connection& conn)
                         { CHECK(conn.exec("UPDATE playlists SET rule_json = '{\"rules\":5}'")); });
        browser.refreshFromLibrary(true, true);
        CHECK(browser.model()->size() == 0);
        CHECK(browser.model()->error().isNotEmpty());
    }
    reader.reset();
    db.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Hidden: writes docs/images/p2-07-smart.png. Run by hand: DevilDJTests.exe "Smart playlist editor screenshot"
TEST_CASE("Smart playlist editor screenshot", "[.screenshot]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    library::SmartRules rules;
    rules.rules = {{RuleField::Bpm, RuleOp::Between, {}, 120.0, 126.0},
                   {RuleField::Genre, RuleOp::Contains, "House"},
                   {RuleField::Rating, RuleOp::GreaterThan, {}, 2.0},
                   {RuleField::DateAdded, RuleOp::InLast, {}, 90.0}};
    Editor editor("Warm-up house", rules, [](const library::SmartRules&) { return 37; });
    editor.setSize(720, 300);
    const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
    const auto out =
        juce::File(juce::String::fromUTF8(DEVILDJ_SOURCE_DIR)).getChildFile("docs/images/p2-07-smart.png");
    out.deleteFile();
    juce::FileOutputStream stream(out);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(image, stream));
}