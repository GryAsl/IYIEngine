@echo off
setlocal enabledelayedexpansion

:: Build the fixture module into native\bin. CMake and Ninja are not on PATH; they ship inside
:: Visual Studio, so locate them the way the engine's own build.bat does (vswhere.exe).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] vswhere.exe not found at !VSWHERE!
    exit /b 1
)

set "VS_DIR="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -property installationPath`) do (
    set "VS_DIR=%%i"
)

if "!VS_DIR!"=="" (
    echo [ERROR] Visual Studio installation not found
    exit /b 1
)

set "CMAKE_EXE=!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA_EXE=!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "VCVARS_BAT=!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat"

:: Same fallback as the engine's build.bat: Visual Studio without its CMake component uses a CMake/Ninja
:: on PATH, the standalone CMake installer's folder, or the ninja "py -m pip install --user ninja" installs.
if not exist "!CMAKE_EXE!" (
    set "CMAKE_EXE="
    for /f "delims=" %%i in ('where cmake.exe 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%i"
    if not defined CMAKE_EXE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
)
if not exist "!NINJA_EXE!" (
    set "NINJA_EXE="
    for /f "delims=" %%i in ('where ninja.exe 2^>nul') do if not defined NINJA_EXE set "NINJA_EXE=%%i"
    if not defined NINJA_EXE for /d %%d in ("%APPDATA%\Python\Python3*") do if exist "%%d\Scripts\ninja.exe" set "NINJA_EXE=%%d\Scripts\ninja.exe"
)
if not defined CMAKE_EXE (
    echo [ERROR] cmake.exe not found: add the "C++ CMake tools" Visual Studio component or run "winget install Kitware.CMake"
    exit /b 1
)
if not defined NINJA_EXE (
    echo [ERROR] ninja.exe not found: add the "C++ CMake tools" Visual Studio component or run "py -m pip install --user ninja"
    exit /b 1
)
if not exist "!VCVARS_BAT!" (
    echo [ERROR] vcvars64.bat not found at !VCVARS_BAT!
    exit /b 1
)

call "!VCVARS_BAT!" >nul
if !ERRORLEVEL! neq 0 (
    echo [ERROR] Failed to run vcvars64.bat
    exit /b 1
)

echo [IYI] Building native_example (Release)...
"!CMAKE_EXE!" -S "%~dp0." -B "%~dp0build" -G "Ninja" -DCMAKE_MAKE_PROGRAM="!NINJA_EXE!" -DCMAKE_BUILD_TYPE=Release
if !ERRORLEVEL! neq 0 (
    echo [ERROR] CMake configuration failed
    exit /b 1
)

"!CMAKE_EXE!" --build "%~dp0build"
if !ERRORLEVEL! neq 0 (
    echo [ERROR] Build failed
    exit /b 1
)

echo [IYI] Module built in %~dp0bin
exit /b 0
