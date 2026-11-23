@echo off
setlocal enabledelayedexpansion

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] vswhere.exe not found at !VSWHERE!
    exit /b 1
)

:: "call" first: cmd strips the outer quotes of a for /f command that starts with one, which breaks
:: the quoted "Program Files (x86)" path when this script is started from another shell or folder.
set "VS_DIR="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -property installationPath`) do (
    set "VS_DIR=%%i"
)

if not exist "!VS_DIR!" (
    echo [ERROR] Visual Studio installation not found
    exit /b 1
)

set "CMAKE_EXE=!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA_EXE=!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "VCVARS_BAT=!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat"

:: Visual Studio without its CMake component (one of the two machines): fall back to a CMake/Ninja on PATH,
:: the standalone CMake installer's folder, or the ninja that "py -m pip install --user ninja" puts in Scripts.
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

if /i "%~1"=="vs" (
    echo [IYI] Generating Visual Studio 2026 solution in build\vs...
    "!CMAKE_EXE!" -S "%~dp0." -B "%~dp0build\vs" -G "Visual Studio 18 2026" -A x64
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] CMake VS generation failed
        exit /b 1
    )
    echo [IYI] Solution generated in %~dp0build\vs
    exit /b 0
)

call "%~dp0check_invariants.bat"

:: Python 3.14 + the current raylib (6.2) build into build\ninja. "py311" picks the legacy Python 3.11, "rl55" the
:: legacy raylib 5.5 (in this order); each adds a suffix: build\ninja-py311, build\ninja-rl55, build\ninja-py311-rl55.
:: A raylib bump changes RL_VERSION and the legacy argument here, in build.sh and the places SPEC 0.1 lists.
set "PY_VERSION=3.14"
set "RL_VERSION=6.2"
set "BUILD_DIR=%~dp0build\ninja"
if /i "%~1"=="py311" (
    set "PY_VERSION=3.11"
    set "BUILD_DIR=!BUILD_DIR!-py311"
    shift /1
)
if /i "%~1"=="rl55" (
    set "RL_VERSION=5.5"
    set "BUILD_DIR=!BUILD_DIR!-rl55"
    shift /1
)

set "CONFIG=%~1"
if "!CONFIG!"=="" set "CONFIG=Release"

:: The raylib version is passed every time: a cache variable's default never reaches an existing build tree.
echo [IYI] Configuring Ninja build (!CONFIG!, Python !PY_VERSION!, raylib !RL_VERSION!)...
"!CMAKE_EXE!" -S "%~dp0." -B "!BUILD_DIR!" -G "Ninja" -DCMAKE_MAKE_PROGRAM="!NINJA_EXE!" -DCMAKE_BUILD_TYPE=!CONFIG! -DIYI_SHIPPING_BUILD=OFF -DIYI_PYTHON_VERSION=!PY_VERSION! -DIYI_RAYLIB_VERSION=!RL_VERSION!
if !ERRORLEVEL! neq 0 (
    echo [ERROR] CMake configuration failed
    exit /b 1
)

:: A failed link (e.g. LNK1104 while the exe is still running) can come back as a negative exit code,
:: which "if errorlevel 1" reads as success; every check here compares against 0 instead.
echo [IYI] Building IYIEngine (!CONFIG!)...
"!CMAKE_EXE!" --build "!BUILD_DIR!" --config !CONFIG!
if !ERRORLEVEL! neq 0 (
    echo [ERROR] Build failed
    exit /b 1
)

set "EXE_PATH=!BUILD_DIR!\IYIEngine.exe"
if exist "!EXE_PATH!" (
    echo [IYI] Build successful: !EXE_PATH!
    exit /b 0
) else (
    echo [ERROR] Output binary not found at !EXE_PATH!
    exit /b 1
)
