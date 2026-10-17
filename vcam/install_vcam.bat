@echo off
rem Keep this file ASCII-only. Builds iphonecam.dll if needed, then installs it (asks for admin rights).
cd /d "%~dp0"
if /i "%~1"=="-Uninstall" goto :install
if not exist build\Release\iphonecam.dll (
  echo Building iphonecam.dll ...
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64 || goto :builderror
  cmake --build build --config Release || goto :builderror
)

:install
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_vcam.ps1" %*
echo.
type "%TEMP%\iphonecam-install.log" 2>nul | findstr /C:"iPhoneCam" /C:"ERROR"
pause
exit /b

:builderror
echo Build failed. Install "Visual Studio 2022 Build Tools" (Desktop development with C++) and CMake 3.24+.
pause
exit /b 1
