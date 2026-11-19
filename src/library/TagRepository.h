#pragma once

#include "util/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace devildj::library
{

class Connection;

// The DJ's own labels (P2-08 builds the UI): categories ("Genre", "Mood", "Situation") holding tags
// ("Warm-up", "Peak time"); any number of tags per track.
struct TagCategory
{
    std::int64_t id = 0;
    std::string name;
    int sortOrder = 0;
    friend bool operator==(const TagCategory&, const TagCategory&) = default;
};

struct Tag
{
    std::int64_t id = 0;
    std::optional<std::int64_t> categoryId; // nullopt = not in a category
    std::string name;
    friend bool operator==(const Tag&, const Tag&) = default;
};

// Tag categories, tags and which tracks carry them (P2-03). Same rules as TrackRepository. Names are trimmed,
// must not be empty and are unique (case-insensitive) among categories / within a category.
class TagRepository
{
public:
    explicit TagRepository(Connection& connection) noexcept;

    util::Result<std::int64_t> createCategory(const std::string& name); // added after the others
    std::vector<TagCategory> categories();                               // by sortOrder
    std::optional<TagCategory> category(std::int64_t id);
    bool updateCategory(const TagCategory& category); // rename / reorder
    bool removeCategory(std::int64_t id);             // with its tags (and their track assignments)

    util::Result<std::int64_t> createTag(const std::string& name, std::optional<std::int64_t> categoryId);
    std::optional<Tag> tag(std::int64_t id);
    std::vector<Tag> tags();                                          // by name
    std::vector<Tag> tagsInCategory(std::optional<std::int64_t> categoryId); // by name
    bool updateTag(const Tag& tag); // rename / move to another category
    bool removeTag(std::int64_t id);

    bool assign(std::int64_t trackId, std::int64_t tagId); // no-op if already assigned
    bool unassign(std::int64_t trackId, std::int64_t tagId);
    std::vector<Tag> tagsForTrack(std::int64_t trackId);
    std::vector<std::int64_t> tracksWithTag(std::int64_t tagId);

    const std::string& error() const noexcept { return lastError; }

private:
    bool fail(std::string message);
    bool categoryNameTaken(const std::string& name, std::int64_t exceptId);
    bool tagNameTaken(const std::string& name, std::optional<std::int64_t> categoryId, std::int64_t exceptId);
    Connection& db;
    std::string lastError;
};

} // namespace devildj::library
