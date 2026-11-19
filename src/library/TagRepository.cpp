#include "TagRepository.h"

#include "library/Connection.h"
#include "library/RepositoryUtil.h"

namespace devildj::library
{

namespace
{
Tag readTag(const Statement& s)
{
    return Tag{s.getInt(0), s.getOptionalInt(1), s.getText(2)};
}

TagCategory readCategory(const Statement& s)
{
    return TagCategory{s.getInt(0), s.getText(1), static_cast<int>(s.getInt(2))};
}
} // namespace

TagRepository::TagRepository(Connection& connection) noexcept
    : db(connection)
{
}

bool TagRepository::fail(std::string message)
{
    lastError = std::move(message);
    return false;
}

bool TagRepository::categoryNameTaken(const std::string& name, std::int64_t exceptId)
{
    auto* s = db.prepare("SELECT 1 FROM tag_categories WHERE name = ? COLLATE NOCASE AND id != ?");
    const bool taken = s != nullptr && s->bind(1, name).bind(2, exceptId).step();
    if (s != nullptr)
        s->reset();
    return taken;
}

bool TagRepository::tagNameTaken(const std::string& name, std::optional<std::int64_t> categoryId,
                                 std::int64_t exceptId)
{
    auto* s = db.prepare("SELECT 1 FROM tags WHERE name = ? COLLATE NOCASE AND category_id IS ? AND id != ?");
    const bool taken = s != nullptr && s->bind(1, name).bind(2, categoryId).bind(3, exceptId).step();
    if (s != nullptr)
        s->reset();
    return taken;
}

// --- categories -------------------------------------------------------------------------------------------------

util::Result<std::int64_t> TagRepository::createCategory(const std::string& rawName)
{
    using R = util::Result<std::int64_t>;
    const auto name = trimmed(rawName);
    if (name.empty())
        return fail("a tag category needs a name"), R::fail(lastError);
    if (categoryNameTaken(name, 0))
        return fail("there is already a category \"" + name + "\""), R::fail(lastError);
    auto* s = db.prepare("INSERT INTO tag_categories (name, sort_order) "
                         "VALUES (?, (SELECT coalesce(max(sort_order), 0) + 1 FROM tag_categories))");
    if (s == nullptr)
        return fail(db.lastError()), R::fail(lastError);
    if (!s->bind(1, name).run())
        return fail(s->error()), R::fail(lastError);
    const auto id = db.lastInsertId();
    db.noteChange(Table::TagCategories, id);
    lastError.clear();
    return R::ok(id);
}

std::vector<TagCategory> TagRepository::categories()
{
    std::vector<TagCategory> out;
    auto* s = db.prepare("SELECT id, name, coalesce(sort_order, 0) FROM tag_categories ORDER BY sort_order, id");
    if (s == nullptr)
        return out;
    while (s->step())
        out.push_back(readCategory(*s));
    return out;
}

std::optional<TagCategory> TagRepository::category(std::int64_t id)
{
    auto* s = db.prepare("SELECT id, name, coalesce(sort_order, 0) FROM tag_categories WHERE id = ?");
    if (s == nullptr || !s->bind(1, id).step())
        return std::nullopt;
    auto c = readCategory(*s);
    s->reset();
    return c;
}

bool TagRepository::updateCategory(const TagCategory& c)
{
    const auto name = trimmed(c.name);
    if (name.empty())
        return fail("a tag category needs a name");
    if (!category(c.id))
        return fail("no tag category " + std::to_string(c.id));
    if (categoryNameTaken(name, c.id))
        return fail("there is already a category \"" + name + "\"");
    auto* s = db.prepare("UPDATE tag_categories SET name = ?, sort_order = ? WHERE id = ?");
    if (s == nullptr)
        return fail(db.lastError());
    if (!s->bind(1, name).bind(2, c.sortOrder).bind(3, c.id).run())
        return fail(s->error());
    db.noteChange(Table::TagCategories, c.id);
    lastError.clear();
    return true;
}

bool TagRepository::removeCategory(std::int64_t id)
{
    if (!category(id))
        return fail("no tag category " + std::to_string(id));
    const auto inside = tagsInCategory(id);
    const bool ok = db.savepoint(
        [&]
        {
            for (const auto& t : inside)
                if (!removeTag(t.id))
                    return false;
            auto* s = db.prepare("DELETE FROM tag_categories WHERE id = ?");
            if (s == nullptr)
                return fail(db.lastError());
            return s->bind(1, id).run() || fail(s->error());
        });
    if (!ok)
        return false;
    db.noteChange(Table::TagCategories, id, true);
    lastError.clear();
    return true;
}

// --- tags -------------------------------------------------------------------------------------------------------

util::Result<std::int64_t> TagRepository::createTag(const std::string& rawName, std::optional<std::int64_t> categoryId)
{
    using R = util::Result<std::int64_t>;
    const auto name = trimmed(rawName);
    if (name.empty())
        return fail("a tag needs a name"), R::fail(lastError);
    if (categoryId && !category(*categoryId))
        return fail("no tag category " + std::to_string(*categoryId)), R::fail(lastError);
    if (tagNameTaken(name, categoryId, 0))
        return fail("there is already a tag \"" + name + "\" there"), R::fail(lastError);
    auto* s = db.prepare("INSERT INTO tags (category_id, name) VALUES (?, ?)");
    if (s == nullptr)
        return fail(db.lastError()), R::fail(lastError);
    if (!s->bind(1, categoryId).bind(2, name).run())
        return fail(s->error()), R::fail(lastError);
    const auto id = db.lastInsertId();
    db.noteChange(Table::Tags, id);
    lastError.clear();
    return R::ok(id);
}

std::optional<Tag> TagRepository::tag(std::int64_t id)
{
    auto* s = db.prepare("SELECT id, category_id, name FROM tags WHERE id = ?");
    if (s == nullptr || !s->bind(1, id).step())
        return std::nullopt;
    auto t = readTag(*s);
    s->reset();
    return t;
}

std::vector<Tag> TagRepository::tags()
{
    std::vector<Tag> out;
    auto* s = db.prepare("SELECT id, category_id, name FROM tags ORDER BY name COLLATE NOCASE, id");
    if (s == nullptr)
        return out;
    while (s->step())
        out.push_back(readTag(*s));
    return out;
}

std::vector<Tag> TagRepository::tagsInCategory(std::optional<std::int64_t> categoryId)
{
    std::vector<Tag> out;
    auto* s =
        db.prepare("SELECT id, category_id, name FROM tags WHERE category_id IS ? ORDER BY name COLLATE NOCASE, id");
    if (s == nullptr)
        return out;
    s->bind(1, categoryId);
    while (s->step())
        out.push_back(readTag(*s));
    return out;
}

bool TagRepository::updateTag(const Tag& t)
{
    const auto name = trimmed(t.name);
    if (name.empty())
        return fail("a tag needs a name");
    if (!tag(t.id))
        return fail("no tag " + std::to_string(t.id));
    if (t.categoryId && !category(*t.categoryId))
        return fail("no tag category " + std::to_string(*t.categoryId));
    if (tagNameTaken(name, t.categoryId, t.id))
        return fail("there is already a tag \"" + name + "\" there");
    auto* s = db.prepare("UPDATE tags SET category_id = ?, name = ? WHERE id = ?");
    if (s == nullptr)
        return fail(db.lastError());
    if (!s->bind(1, t.categoryId).bind(2, name).bind(3, t.id).run())
        return fail(s->error());
    db.noteChange(Table::Tags, t.id);
    lastError.clear();
    return true;
}

bool TagRepository::removeTag(std::int64_t id)
{
    if (!tag(id))
        return fail("no tag " + std::to_string(id));
    const auto tracks = tracksWithTag(id);
    auto* s = db.prepare("DELETE FROM tags WHERE id = ?"); // track_tags rows go with it (ON DELETE CASCADE)
    if (s == nullptr)
        return fail(db.lastError());
    if (!s->bind(1, id).run())
        return fail(s->error());
    db.noteChange(Table::Tags, id, true);
    for (const auto t : tracks)
        db.noteChange(Table::TrackTags, t);
    lastError.clear();
    return true;
}

// --- assignments ------------------------------------------------------------------------------------------------

bool TagRepository::assign(std::int64_t trackId, std::int64_t tagId)
{
    auto* s = db.prepare("INSERT OR IGNORE INTO track_tags (track_id, tag_id) VALUES (?, ?)");
    if (s == nullptr)
        return fail(db.lastError());
    if (!s->bind(1, trackId).bind(2, tagId).run())
        return fail(s->error()); // e.g. FOREIGN KEY: no such track or tag
    if (db.changes() > 0)
        db.noteChange(Table::TrackTags, trackId);
    lastError.clear();
    return true;
}

bool TagRepository::unassign(std::int64_t trackId, std::int64_t tagId)
{
    auto* s = db.prepare("DELETE FROM track_tags WHERE track_id = ? AND tag_id = ?");
    if (s == nullptr)
        return fail(db.lastError());
    if (!s->bind(1, trackId).bind(2, tagId).run())
        return fail(s->error());
    if (db.changes() > 0)
        db.noteChange(Table::TrackTags, trackId);
    lastError.clear();
    return true;
}

std::vector<Tag> TagRepository::tagsForTrack(std::int64_t trackId)
{
    std::vector<Tag> out;
    auto* s = db.prepare("SELECT t.id, t.category_id, t.name FROM tags t JOIN track_tags tt ON tt.tag_id = t.id "
                         "WHERE tt.track_id = ? ORDER BY t.name COLLATE NOCASE, t.id");
    if (s == nullptr)
        return out;
    s->bind(1, trackId);
    while (s->step())
        out.push_back(readTag(*s));
    return out;
}

std::vector<std::int64_t> TagRepository::tracksWithTag(std::int64_t tagId)
{
    std::vector<std::int64_t> out;
    auto* s = db.prepare("SELECT track_id FROM track_tags WHERE tag_id = ? ORDER BY track_id");
    if (s == nullptr)
        return out;
    s->bind(1, tagId);
    while (s->step())
        out.push_back(s->getInt(0));
    return out;
}

} // namespace devildj::library
