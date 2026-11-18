#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "util/Result.h"

namespace devildj::library
{

struct ContentHash
{
    std::string hex;        // 16 lower-case hex digits
    std::uint64_t size = 0; // file size in bytes
};

// The library's duplicate check (P2-02): XXH3-64 of the file size, the first 1 MB and the last 1 MB (the whole
// file when it is 2 MB or smaller). Reads at most 2 MB, so it is quick on big files; a retag that changes the
// first or last MB (most tag formats live there) changes the hash.
util::Result<ContentHash> hashFileContent(const std::filesystem::path& file);

} // namespace devildj::library
