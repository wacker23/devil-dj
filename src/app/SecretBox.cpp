// No JUCE here: windows.h on its own (DPAPI lives in crypt32).
#include "app/SecretBox.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dpapi.h>
#endif

namespace devildj::app
{

std::vector<std::uint8_t> sealForUser(const std::vector<std::uint8_t>& plain)
{
#ifdef _WIN32
    DATA_BLOB in{static_cast<DWORD>(plain.size()), const_cast<BYTE*>(plain.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Devil-DJ", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::vector<std::uint8_t> sealed(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return sealed;
#else
    return plain;
#endif
}

bool openForUser(const std::vector<std::uint8_t>& sealed, std::vector<std::uint8_t>& plain)
{
#ifdef _WIN32
    DATA_BLOB in{static_cast<DWORD>(sealed.size()), const_cast<BYTE*>(sealed.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    plain.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return true;
#else
    plain = sealed;
    return true;
#endif
}

} // namespace devildj::app
