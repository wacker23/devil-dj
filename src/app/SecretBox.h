#pragma once

// Bytes sealed for the current Windows user (DPAPI, P4-20): only this user on this computer can open them again.
// Used for the Spotify tokens. On other systems the bytes pass through unchanged (Devil-DJ targets Windows).

#include <cstdint>
#include <vector>

namespace devildj::app
{

std::vector<std::uint8_t> sealForUser(const std::vector<std::uint8_t>& plain); // empty on failure
bool openForUser(const std::vector<std::uint8_t>& sealed, std::vector<std::uint8_t>& plain);

} // namespace devildj::app
