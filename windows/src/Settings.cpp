// Settings.cpp — persisted user settings backed by the Windows registry.
//
// Port of the macOS UserDefaults usage in AppDelegate.swift. Stores under
//   HKCU\Software\io.vbar.screenflip
//     flippedDisplays : REG_MULTI_SZ  — stable DisplayIds the user wants flipped
//     flipCursor      : REG_DWORD     — mirror the proxy cursor image to match glass
//
// Required link libraries (keep build.bat in sync):
//   advapi32.lib   (RegCreateKeyExW / RegOpenKeyExW / RegSetValueExW / RegQueryValueExW / RegCloseKey)

#include "Settings.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sf {
namespace Settings {

namespace {

// HKCU\Software\io.vbar.screenflip
constexpr const wchar_t* kSubKey       = L"Software\\io.vbar.screenflip";
constexpr const wchar_t* kValDisplays  = L"flippedDisplays";
constexpr const wchar_t* kValFlipCursor = L"flipCursor";

// Open the settings key for reading. Returns nullptr (no logging) if it does
// not exist yet — a missing key is a normal "no settings saved" state.
HKEY OpenForRead()
{
    HKEY key = nullptr;
    LSTATUS rc = RegOpenKeyExW(HKEY_CURRENT_USER, kSubKey, 0, KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS)
    {
        // ERROR_FILE_NOT_FOUND is expected on first run; only log the unexpected.
        if (rc != ERROR_FILE_NOT_FOUND)
            sf::Log::Linef(L"Settings: RegOpenKeyExW(read) failed: %ld", (long)rc);
        return nullptr;
    }
    return key;
}

// Open (creating if needed) the settings key for writing. Returns nullptr on
// failure (logged).
HKEY OpenForWrite()
{
    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS rc = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kSubKey,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        &disposition);
    if (rc != ERROR_SUCCESS)
    {
        sf::Log::Linef(L"Settings: RegCreateKeyExW(write) failed: %ld", (long)rc);
        return nullptr;
    }
    return key;
}

} // namespace

// ---------------------------------------------------------------------------
// SelectedDisplays — read REG_MULTI_SZ "flippedDisplays".
//
// A REG_MULTI_SZ is a sequence of null-terminated wide strings followed by an
// extra terminating null, e.g.  "abc\0def\0\0". We query the byte size first,
// read into a buffer, then split on the embedded nulls (skipping empty strings,
// including the leading-empty case of an "empty" multi-sz which is a single \0).
// ---------------------------------------------------------------------------
std::vector<std::wstring> SelectedDisplays()
{
    std::vector<std::wstring> result;

    HKEY key = OpenForRead();
    if (!key)
        return result; // default: empty selection

    // Query size + type first.
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS rc = RegQueryValueExW(key, kValDisplays, nullptr, &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS)
    {
        if (rc != ERROR_FILE_NOT_FOUND)
            sf::Log::Linef(L"Settings: RegQueryValueExW(size) failed: %ld", (long)rc);
        RegCloseKey(key);
        return result; // default: empty selection
    }

    if (type != REG_MULTI_SZ || bytes < sizeof(wchar_t))
    {
        // Wrong type or degenerate size — treat as empty.
        RegCloseKey(key);
        return result;
    }

    // Allocate enough wchar_t to hold the buffer, rounding up, plus a couple of
    // guard nulls so our parsing never reads past a malformed (non-double-null-
    // terminated) buffer.
    const size_t count = (static_cast<size_t>(bytes) + sizeof(wchar_t) - 1) / sizeof(wchar_t);
    std::vector<wchar_t> buf(count + 2, L'\0');

    DWORD readBytes = static_cast<DWORD>(count * sizeof(wchar_t));
    rc = RegQueryValueExW(key, kValDisplays, nullptr, &type,
                          reinterpret_cast<LPBYTE>(buf.data()), &readBytes);
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS)
    {
        sf::Log::Linef(L"Settings: RegQueryValueExW(read) failed: %ld", (long)rc);
        return result;
    }

    // Walk the buffer; each run up to a null is one entry. Stop at the double
    // null (an empty entry following another, i.e. *p == L'\0' at a run start).
    const wchar_t* p = buf.data();
    const wchar_t* end = buf.data() + buf.size();
    while (p < end && *p != L'\0')
    {
        std::wstring entry(p);
        p += entry.size() + 1; // advance past this string and its null
        if (!entry.empty())    // skip empty strings defensively
            result.push_back(std::move(entry));
    }

    return result;
}

// ---------------------------------------------------------------------------
// SetSelectedDisplays — write REG_MULTI_SZ "flippedDisplays".
//
// Build the double-null-terminated buffer:
//   - each id contributes its characters + one L'\0'
//   - the whole thing gets a final extra L'\0'
//   - an empty vector => a single L'\0' (an empty REG_MULTI_SZ)
// Empty strings in the input are skipped so we never embed a stray "\0\0" that
// would be read back as a premature terminator.
// ---------------------------------------------------------------------------
void SetSelectedDisplays(const std::vector<std::wstring>& ids)
{
    // Compose the multi-sz buffer.
    std::vector<wchar_t> buf;
    for (const std::wstring& id : ids)
    {
        if (id.empty())
            continue; // never write empty entries
        buf.insert(buf.end(), id.begin(), id.end());
        buf.push_back(L'\0');
    }
    // Final extra terminator. For an empty list this leaves exactly one L'\0',
    // which is a valid (empty) REG_MULTI_SZ.
    buf.push_back(L'\0');

    HKEY key = OpenForWrite();
    if (!key)
        return;

    const DWORD bytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    LSTATUS rc = RegSetValueExW(
        key,
        kValDisplays,
        0,
        REG_MULTI_SZ,
        reinterpret_cast<const BYTE*>(buf.data()),
        bytes);
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS)
        sf::Log::Linef(L"Settings: RegSetValueExW(flippedDisplays) failed: %ld", (long)rc);
}

// ---------------------------------------------------------------------------
// FlipCursor — read REG_DWORD "flipCursor" (default false).
// ---------------------------------------------------------------------------
bool FlipCursor()
{
    HKEY key = OpenForRead();
    if (!key)
        return false; // default

    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    LSTATUS rc = RegQueryValueExW(key, kValFlipCursor, nullptr, &type,
                                  reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS)
    {
        if (rc != ERROR_FILE_NOT_FOUND)
            sf::Log::Linef(L"Settings: RegQueryValueExW(flipCursor) failed: %ld", (long)rc);
        return false; // default
    }

    if (type != REG_DWORD || size != sizeof(DWORD))
        return false; // wrong type — default

    return value != 0;
}

// ---------------------------------------------------------------------------
// SetFlipCursor — write REG_DWORD "flipCursor" (0/1).
// ---------------------------------------------------------------------------
void SetFlipCursor(bool on)
{
    HKEY key = OpenForWrite();
    if (!key)
        return;

    DWORD value = on ? 1u : 0u;
    LSTATUS rc = RegSetValueExW(
        key,
        kValFlipCursor,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS)
        sf::Log::Linef(L"Settings: RegSetValueExW(flipCursor) failed: %ld", (long)rc);
}

} // namespace Settings
} // namespace sf
