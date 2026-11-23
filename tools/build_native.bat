@echo off
setlocal enabledelayedexpansion

:: Build a game's native module (docs/spec/project.md section 2b, cmake/iyi_native.cmake): build_native.bat [native dir]
:: (default: the current folder). Output goes to <native dir>\bin; restart the game after, since a loaded
:: .pyd is locked. CMake and Ninja ship inside Visual Studio, found the way build.bat finds them.
set "NATIVE_DIR=%~1"
if "!NATIVE_DIR!"=="" set "NATIVE_DIR=%CD%"
if not exist "!NATIVE_DIR!\CMakeLists.txt" (
    echo [ERROR] No CMakeLists.txt in !NATIVE_DIR!
    exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_DIR="
if exist "!VSWHERE!" for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -property installationPath`) do set "VS_DIR=%%i"
if "!VS_DIR!"=="" (
    echo [ERROR] Visual Studio installation not found
    exit /b 1
)
set "CMAKE_EXE=!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA_EXE=!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
call "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat" >nul
if !ERRORLEVEL! neq 0 (
    echo [ERROR] Failed to run vcvars64.bat
    exit /b 1
)

echo [IYI] Building the native module in !NATIVE_DIR! (Release)...
"!CMAKE_EXE!" -S "!NATIVE_DIR!" -B "!NATIVE_DIR!\build" -G "Ninja" -DCMAKE_MAKE_PROGRAM="!NINJA_EXE!"
if !ERRORLEVEL! neq 0 (
    echo [ERROR] CMake configuration failed
    exit /b 1
)
"!CMAKE_EXE!" --build "!NATIVE_DIR!\build"
if !ERRORLEVEL! neq 0 (
    echo [ERROR] Build failed
    exit /b 1
)
echo [IYI] Module built in !NATIVE_DIR!\bin
exit /b 0
