// Log.cpp — file logger for the windowless ScreenFlip tray app.
//
// Faithful port of macOS Log.swift. Writes timestamped lines to
// %TEMP%\screenflip.log and mirrors every line to OutputDebugStringW so the
// app can be diagnosed without a console (the role /tmp/screenflip.log plays
// on macOS). All writes are serialized through a static mutex so concurrent
// worker threads (capture, mirror-input, etc.) never interleave a line.
//
// Link libraries: none beyond the implicitly-linked kernel32.lib (GetTempPathW,
// GetLocalTime, CreateFileW, WriteFile, SetFilePointerEx, OutputDebugStringW,
// CloseHandle, GetLastError, WideCharToMultiByte). No extra .lib entries are
// required in build.bat for this translation unit.

#include "Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace sf {
namespace Log {

namespace {

// Serializes all file/debug output so lines from different threads never
// interleave.
std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}

// Builds and caches the absolute log path: %TEMP%\screenflip.log.
const std::wstring& CachedPath() {
    static const std::wstring path = [] {
        // GetTempPathW returns a path that always ends with a backslash.
        wchar_t buf[MAX_PATH + 1] = {};
        DWORD n = GetTempPathW(MAX_PATH + 1, buf);
        std::wstring dir;
        if (n == 0 || n > MAX_PATH) {
            // Fall back to the current directory if %TEMP% is unavailable.
            dir = L".\\";
        } else {
            dir.assign(buf, n);
            if (dir.empty() || dir.back() != L'\\') {
                dir.push_back(L'\\');
            }
        }
        return dir + L"screenflip.log";
    }();
    return path;
}

// Converts a wide string to UTF-8 bytes for writing to the on-disk log.
// The log file is stored as UTF-8 (no BOM) so it is readable by any editor.
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                     static_cast<int>(w.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(needed), '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()),
                                      &out[0], needed, nullptr, nullptr);
    if (written <= 0) {
        return std::string();
    }
    out.resize(static_cast<size_t>(written));
    return out;
}

} // namespace

std::wstring Path() {
    return CachedPath();
}

void Reset() {
    std::lock_guard<std::mutex> lock(LogMutex());

    // CREATE_ALWAYS truncates an existing file (or creates a new one), giving
    // us an empty log at launch.
    HANDLE h = CreateFileW(CachedPath().c_str(),
                           GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // Can't use Log::Line here (it would re-enter the mutex / fail too);
        // surface the failure to the debugger only.
        wchar_t dbg[256];
        _snwprintf_s(dbg, _countof(dbg), _TRUNCATE,
                     L"[screenflip] Log::Reset CreateFileW failed (err=%lu)\n",
                     GetLastError());
        OutputDebugStringW(dbg);
        return;
    }
    CloseHandle(h);
}

void Line(const std::wstring& msg) {
    // Timestamp "[HH:mm:ss.mmm] " in local time, matching the macOS format.
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t stamp[32];
    _snwprintf_s(stamp, _countof(stamp), _TRUNCATE,
                 L"[%02u:%02u:%02u.%03u] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::wstring line;
    line.reserve(msg.size() + 24);
    line.append(stamp);
    line.append(msg);
    line.push_back(L'\n');

    std::lock_guard<std::mutex> lock(LogMutex());

    // Mirror to the debugger first; this is the diagnostic path that always
    // works even if the file can't be opened.
    OutputDebugStringW(line.c_str());

    // Append to the on-disk log. OPEN_ALWAYS opens an existing file or creates
    // a new one; we then seek to the end and write. (FILE_APPEND_DATA would
    // also work, but an explicit seek keeps behavior identical regardless of
    // whether the file was just created.)
    HANDLE h = CreateFileW(CachedPath().c_str(),
                           GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr,
                           OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    SetFilePointerEx(h, zero, nullptr, FILE_END);

    std::string utf8 = ToUtf8(line);
    if (!utf8.empty()) {
        DWORD wrote = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &wrote,
                  nullptr);
    }

    CloseHandle(h);
}

void Linef(const wchar_t* fmt, ...) {
    if (fmt == nullptr) {
        return;
    }

    wchar_t buf[2048];

    va_list args;
    va_start(args, fmt);
    // _vsnwprintf_s with _TRUNCATE always NUL-terminates and never overruns;
    // it returns -1 on truncation, which is harmless — we still log what fits.
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    Line(std::wstring(buf));
}

} // namespace Log
} // namespace sf
