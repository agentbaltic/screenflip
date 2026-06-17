@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ScreenFlip (Windows) build — MSVC command-line tools only, no Visual Studio IDE
rem (the analogue of the macOS build.sh, which uses swiftc + Command Line Tools).
rem
rem   build.bat          Build the app -> build\ScreenFlip.exe
rem   build.bat driver   Build the bundled IddCx driver (needs the WDK + msbuild)
rem
rem Run from an "x64 Native Tools Command Prompt for VS" (so cl/link/rc/fxc/mt and
rem the INCLUDE/LIB env are set), with a Windows 10/11 SDK installed.

if /I "%~1"=="driver" goto :driver

where cl >nul 2>nul || (
  echo [!] cl.exe not found. Open an "x64 Native Tools Command Prompt for VS" and re-run,
  echo     or install the "Desktop development with C++" workload + a Windows 10/11 SDK.
  exit /b 1
)

set OUT=build
if not exist "%OUT%" mkdir "%OUT%"

echo ^>^> Compiling HLSL shaders (fxc)
fxc /nologo /T vs_5_0 /E main /Fh "%OUT%\flip_vs.h"   /Vn g_flip_vs   src\d3d\flip_vs.hlsl   || goto :fail
fxc /nologo /T ps_5_0 /E main /Fh "%OUT%\flip_ps.h"   /Vn g_flip_ps   src\d3d\flip_ps.hlsl   || goto :fail
fxc /nologo /T vs_5_0 /E main /Fh "%OUT%\cursor_vs.h" /Vn g_cursor_vs src\d3d\cursor_vs.hlsl || goto :fail
fxc /nologo /T ps_5_0 /E main /Fh "%OUT%\cursor_ps.h" /Vn g_cursor_ps src\d3d\cursor_ps.hlsl || goto :fail

echo ^>^> Compiling resources (rc)
rc /nologo /fo "%OUT%\app.res" app.rc || goto :fail

echo ^>^> Compiling + linking ScreenFlip.exe (cl/link)
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /bigobj ^
   /DUNICODE /D_UNICODE /DNOMINMAX ^
   /I "%OUT%" /I src ^
   src\*.cpp "%OUT%\app.res" ^
   /Fo"%OUT%\\" /Fe"%OUT%\ScreenFlip.exe" ^
   /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib ^
   d3d11.lib dxgi.lib dcomp.lib windowsapp.lib || goto :fail

echo ^>^> Embedding manifest (PerMonitorV2 DPI, asInvoker)
mt -nologo -manifest src\app.manifest -outputresource:"%OUT%\ScreenFlip.exe";1 || goto :fail

echo.
echo ^>^> Built %OUT%\ScreenFlip.exe
echo    Run it; it lives in the system tray (look for the flip icon). Log: %%TEMP%%\screenflip.log
exit /b 0

:driver
echo ^>^> Building the IddCx virtual-display driver (windows\driver)
where msbuild >nul 2>nul || (
  echo [!] msbuild not found. Open an EWDK prompt, or VS with the Windows Driver Kit ^(WDK^) installed.
  exit /b 1
)
msbuild driver\ScreenFlipIdd.vcxproj /p:Configuration=Release /p:Platform=x64 || goto :fail
echo ^>^> Driver built. See windows\driver\README.md to test-sign and install it.
exit /b 0

:fail
echo [!] Build failed.
exit /b 1
