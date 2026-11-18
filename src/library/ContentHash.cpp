#include "ContentHash.h"

#include <xxhash.h>

#include <cstdio>
#include <memory>
#include <vector>

namespace devildj::library
{

namespace
{
constexpr std::uint64_t kChunk = 1u << 20;

struct FileCloser
{
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};

bool readAt(std::FILE* f, std::uint64_t offset, std::size_t bytes, std::vector<char>& buffer)
{
    buffer.resize(bytes);
    if (_fseeki64(f, static_cast<long long>(offset), SEEK_SET) != 0)
        return false;
    return std::fread(buffer.data(), 1, bytes, f) == bytes;
}
} // namespace

util::Result<ContentHash> hashFileContent(const std::filesystem::path& file)
{
    using R = util::Result<ContentHash>;
    std::FILE* raw = nullptr;
    if (_wfopen_s(&raw, file.c_str(), L"rbS") != 0 || raw == nullptr) // S: sequential-access hint
        return R::fail("Can't open the file");
    const std::unique_ptr<std::FILE, FileCloser> f(raw);

    if (_fseeki64(f.get(), 0, SEEK_END) != 0)
        return R::fail("Can't read the file size");
    const auto end = _ftelli64(f.get());
    if (end < 0)
        return R::fail("Can't read the file size");
    const auto size = static_cast<std::uint64_t>(end);

    XXH3_state_t* state = XXH3_createState();
    if (state == nullptr)
        return R::fail("Out of memory");
    const std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)> owner(state, &XXH3_freeState);
    XXH3_64bits_reset(state);
    XXH3_64bits_update(state, &size, sizeof(size));

    std::vector<char> buffer;
    if (size <= 2 * kChunk)
    {
        if (!readAt(f.get(), 0, static_cast<std::size_t>(size), buffer))
            return R::fail("Can't read the file");
        XXH3_64bits_update(state, buffer.data(), buffer.size());
    }
    else
    {
        if (!readAt(f.get(), 0, kChunk, buffer))
            return R::fail("Can't read the file");
        XXH3_64bits_update(state, buffer.data(), buffer.size());
        if (!readAt(f.get(), size - kChunk, kChunk, buffer))
            return R::fail("Can't read the file");
        XXH3_64bits_update(state, buffer.data(), buffer.size());
    }

    char hex[17] = {};
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(XXH3_64bits_digest(state)));
    return R::ok(ContentHash{hex, size});
}

} // namespace devildj::library
