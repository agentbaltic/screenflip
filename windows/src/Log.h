// Lightweight file logger for a windowless tray app (port of macOS Log.swift).
// Writes to %TEMP%\screenflip.log and mirrors to OutputDebugStringW so the app
// can be diagnosed without a console — exactly the role of /tmp/screenflip.log
// on macOS.
#pragma once

#include <string>

namespace sf {
namespace Log {

// Truncate the log file (called once at launch).
void Reset();

// Append one timestamped line.
void Line(const std::wstring& msg);

// printf-style convenience overload (wide).
void Linef(const wchar_t* fmt, ...);

// Absolute path of the log file (for the README / menu hint).
std::wstring Path();

} // namespace Log
} // namespace sf
