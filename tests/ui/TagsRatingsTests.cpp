// P2-08: tags, ratings, colours. Done when: persisted and filterable.

#include "library/Connection.h"
#include "library/Database.h"
#include "library/TagRepository.h"
#include "library/TrackRepository.h"
#include "ui/LibraryBrowser.h"
#include "ui/TrackColours.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>

using namespace devildj;
namespace fs = std::filesystem;
using ui::TrackColumn;

namespace
{
juce::StringArray names(const juce::PopupMenu& m)
{
    juce::StringArray out;
    for (juce::PopupMenu::MenuItemIterator it(m); it.next();)
        if (!it.getItem().isSeparator)
            out.add(it.getItem().text);
    return out;
}

const juce::PopupMenu::Item* find(const juce::PopupMenu& m, const juce::String& text)
{
    for (juce::PopupMenu::MenuItemIterator it(m); it.next();)
        if (it.getItem().text == text)
            return &it.getItem();
    return nullptr;
}

struct Rig
{
    fs::path dir;
    std::unique_ptr<library::Database> db;
    std::unique_ptr<library::Connection> reader;
    std::vector<std::int64_t> ids;
    std::unique_ptr<ui::LibraryBrowser> browser;

    Rig()
    {
        dir = fs::temp_directory_path() /
              ("devildj-tags-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        open();
        db->writeAndWait(
            [&](library::Connection& c)
            {
                for (const char* title : {"Alpha", "Bravo", "Charlie"})
                {
                    library::Track t;
                    t.path = std::string("C:\\Music\\") + title + ".mp3";
                    t.contentHash = t.path;
                    t.title = title;
                    ids.push_back(library::TrackRepository(c).add(t).value());
                }
            });
        makeBrowser();
    }
    void open()
    {
        db = std::move(library::Database::open(dir / "library.db")).value();
        reader = std::move(db->openReader()).value();
    }
    void makeBrowser()
    {
        browser = std::make_unique<ui::LibraryBrowser>(db.get(), juce::File());
        browser->setBounds(0, 0, 1400, 400);
    }
    // Close everything and open the library file again (a restart).
    void reopen()
    {
        browser.reset();
        reader.reset();
        db.reset();
        open();
        makeBrowser();
    }
    ~Rig()
    {
        browser.reset();
        reader.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void settle()
    {
        db->flush();
        browser->handlePendingLibraryChanges();
    }
    void select(std::initializer_list<int> rows)
    {
        browser->table().deselectAllRows();
        for (const int r : rows)
            browser->table().selectRow(r, false, false);
    }
    library::Track track(int i) { return *library::TrackRepository(*reader).get(ids[static_cast<std::size_t>(i)]); }
    int matching(const ui::TrackFilter& f)
    {
        // The model's filter (the browser's filter row picks one tag at a time; the model takes several).
        browser->model()->setFilter(f);
        const int n = browser->model()->size();
        browser->model()->setFilter({});
        return n;
    }
};
} // namespace

TEST_CASE("Tracks: quick rating and colour edits", "[library][tags]")
{
    Rig rig;
    std::vector<library::Change> changes;
    rig.db->addChangeListener([&](const std::vector<library::Change>& c) { changes.insert(changes.end(), c.begin(), c.end()); });
    rig.db->writeAndWait(
        [&](library::Connection& c)
        {
            library::TrackRepository repo(c);
            CHECK(repo.setRating(rig.ids[0], 4));
            CHECK(repo.setColour(rig.ids[0], 0xE0483C));
            CHECK_FALSE(repo.setRating(rig.ids[0], 6));
            CHECK_FALSE(repo.setColour(rig.ids[0], 0x1000000));
            CHECK_FALSE(repo.setRating(999, 3));
        });
    CHECK(rig.track(0).rating == 4);
    CHECK(rig.track(0).color == 0xE0483C);
    CHECK(changes.size() == 2);
    rig.db->writeAndWait([&](library::Connection& c) { CHECK(library::TrackRepository(c).setColour(rig.ids[0], std::nullopt)); });
    CHECK_FALSE(rig.track(0).color.has_value());
    CHECK(rig.track(0).rating == 4); // the other fields stay
}

TEST_CASE("Ratings: keys, star clicks, menu; persisted and filterable", "[ui][tags]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Rig rig;
    auto& b = *rig.browser;

    // Ctrl+Shift+N rates every selected track (the key handler calls rateSelected).
    rig.select({0, 2});
    b.rateSelected(5);
    rig.settle();
    CHECK(rig.track(0).rating == 5);
    CHECK(rig.track(1).rating == 0);
    CHECK(rig.track(2).rating == 5);
    CHECK(b.status() == "Rated 5 stars");
    CHECK(b.selectedRows() == std::vector<int>{0, 2}); // regression: the refresh kept only one of several
    CHECK(b.model()->row(0)->rating == 5); // the table shows it
    rig.select({2});
    b.rateSelected(0); // Ctrl+Shift+0
    rig.settle();
    CHECK(rig.track(2).rating == 0);

    // The stars in the Rating cell: which one a click hits.
    CHECK(ui::LibraryBrowser::starAt(2) == 0);
    CHECK(ui::LibraryBrowser::starAt(7) == 1);
    CHECK(ui::LibraryBrowser::starAt(6 + 13 * 2 + 5) == 3);
    CHECK(ui::LibraryBrowser::starAt(500) == 5);

    // A click on the third star of row 1 (Bravo) rates it 3; a second click on the same star clears.
    auto& header = b.table().getHeader();
    const int col = static_cast<int>(TrackColumn::Rating);
    const int x = header.getColumnPosition(header.getIndexOfColumnId(col, true)).getX() + 6 + 13 * 2 + 5;
    const auto click = [&](int row)
    {
        const auto pos = juce::Point<float>(static_cast<float>(x), 40.0f);
        const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), pos, {}, 0.0f, 0.0f, 0.0f, 0.0f,
                                 0.0f, &b.table(), &b.table(), juce::Time::getCurrentTime(), pos,
                                 juce::Time::getCurrentTime(), 1, false);
        b.table().getTableListBoxModel()->cellClicked(row, col, e);
        rig.settle();
    };
    click(1);
    CHECK(rig.track(1).rating == 3);
    CHECK(b.selectedTrackIds() == std::vector<std::int64_t>{rig.ids[1]});
    click(1);
    CHECK(rig.track(1).rating == 0);

    // The menu shows the current rating.
    rig.select({0});
    const auto menu = b.ratingMenu();
    CHECK(names(menu).size() == 6);
    const auto* five = find(menu, juce::String::fromUTF8("\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85\xE2\x98\x85") +
                                      "   (Ctrl+Shift+5)");
    REQUIRE(five != nullptr);
    CHECK(five->isTicked);

    // Filterable and persisted.
    ui::TrackFilter f;
    f.minRating = 4;
    CHECK(rig.matching(f) == 1);
    rig.reopen();
    CHECK(rig.track(0).rating == 5);
    CHECK(rig.matching(f) == 1);
}

TEST_CASE("Colours: palette, persisted and filterable", "[ui][tags]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Rig rig;
    auto& b = *rig.browser;
    CHECK(ui::kTrackColours.size() == 8);
    CHECK(juce::String(ui::paletteName(0x3C7CE0)) == "Blue");
    CHECK(ui::LibraryBrowser::colourName(0x3C7CE0) == "blue");

    rig.select({0, 1});
    b.colourSelected(0x3C7CE0);
    rig.select({2});
    b.colourSelected(0xE0483C);
    rig.settle();
    CHECK(rig.track(0).color == 0x3C7CE0);
    CHECK(rig.track(1).color == 0x3C7CE0);
    CHECK(rig.track(2).color == 0xE0483C);

    // The menu: eight colours (ticked: the selected track's) and No colour.
    rig.select({0});
    const auto menu = b.colourMenu();
    CHECK(names(menu).size() == 9);
    CHECK(find(menu, juce::String::fromUTF8("\xE2\x97\x8F  Blue"))->isTicked);
    CHECK_FALSE(find(menu, juce::String::fromUTF8("\xE2\x97\x8F  Red"))->isTicked);

    // The filter's colour list now has them, by name.
    b.setFiltersShown(true);
    ui::TrackFilter f;
    f.colours = {0x3C7CE0};
    CHECK(rig.matching(f) == 2);

    b.colourSelected(std::nullopt);
    rig.settle();
    CHECK_FALSE(rig.track(0).color.has_value());
    CHECK(rig.matching(f) == 1);

    rig.reopen();
    CHECK(rig.track(1).color == 0x3C7CE0);
    CHECK(rig.matching(f) == 1);
}

TEST_CASE("Tags: categories, assign from the menu, the Tags column; persisted and filterable", "[ui][tags]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Rig rig;
    auto& b = *rig.browser;

    b.createTagCategory("Situation");
    rig.settle();
    const auto situation = library::TagRepository(*rig.reader).categories().at(0).id;
    rig.select({0, 1});
    b.createTag("Warm-up", situation, true); // "New tag in Situation..." also tags the selection
    b.createTag("Vinyl", std::nullopt, false);
    rig.settle();
    library::TagRepository tags(*rig.reader);
    const auto all = tags.tags();
    REQUIRE(all.size() == 2);
    const auto warm = tags.tagsInCategory(situation).at(0).id;
    const auto vinyl = tags.tagsInCategory(std::nullopt).at(0).id;
    CHECK(tags.tracksWithTag(warm) == std::vector<std::int64_t>{rig.ids[0], rig.ids[1]});

    // The menu: the category as a submenu with its tags ticked when every selected track has them.
    rig.select({0, 1});
    auto menu = b.tagMenu();
    CHECK(names(menu).contains("Situation"));
    CHECK(names(menu).contains("Vinyl"));
    CHECK(names(menu).contains("New category..."));
    const auto* sub = find(menu, "Situation");
    REQUIRE(sub != nullptr);
    REQUIRE(sub->subMenu != nullptr);
    CHECK(find(*sub->subMenu, "Warm-up")->isTicked);
    rig.select({1, 2});
    menu = b.tagMenu();
    CHECK_FALSE(find(*find(menu, "Situation")->subMenu, "Warm-up")->isTicked); // only one of the two has it

    // Toggle: on for both, then off again.
    b.setTagOnSelected(vinyl, true);
    rig.settle();
    CHECK(tags.tracksWithTag(vinyl) == std::vector<std::int64_t>{rig.ids[1], rig.ids[2]});
    b.setTagOnSelected(warm, false);
    rig.settle();
    CHECK(tags.tracksWithTag(warm) == std::vector<std::int64_t>{rig.ids[0]});

    // The Tags column (names, sorted) and sorting by it.
    b.setTagOnSelected(warm, true);
    rig.settle();
    CHECK(b.model()->row(1)->tags == "Vinyl, Warm-up");
    CHECK(b.model()->row(0)->tags == "Warm-up");
    CHECK(b.table().getHeader().isColumnVisible(static_cast<int>(TrackColumn::Tags)));
    b.table().getHeader().setSortColumnId(static_cast<int>(TrackColumn::Tags), true);
    b.syncWithHeader();
    CHECK(b.model()->row(0)->tags == "Vinyl, Warm-up");
    CHECK(b.model()->row(2)->tags == "Warm-up");

    // Rename and delete (a tag, then its category with its tags).
    b.renameTag(vinyl, "Vinyl rip");
    b.renameTagCategory(situation, "When");
    rig.settle();
    CHECK(tags.tag(vinyl)->name == "Vinyl rip");
    CHECK(tags.category(situation)->name == "When");
    CHECK((b.tagFilter().getItemText(1) == "When: Warm-up" || b.tagFilter().getItemText(2) == "When: Warm-up"));

    // Filterable and persisted.
    ui::TrackFilter f;
    f.tagIds = {warm};
    CHECK(rig.matching(f) == 3);
    f.tagIds = {warm, vinyl};
    CHECK(rig.matching(f) == 2);
    rig.reopen();
    CHECK(rig.matching(f) == 2);
    CHECK(rig.browser->model()->row(1)->tags == "Vinyl rip, Warm-up");

    rig.browser->deleteTag(vinyl);
    rig.browser->deleteTagCategory(situation);
    rig.settle();
    CHECK(library::TagRepository(*rig.reader).tags().empty());
    CHECK(rig.browser->model()->row(0)->tags.isEmpty());
}
