@echo off
setlocal enabledelayedexpansion

:: Build a native game's exe (docs/spec/project.md section 2c, cmake/iyi_game.cmake):
::   build_game.bat <game dir> ["-DIYI_WITH_PYTHON=OFF"] (quote -D args: cmd splits them at "=")
:: <game dir> holds the game's CMakeLists.txt (the project folder or its src\). Output: <project>\bin.
:: The build tree is <game dir>\build\<Release|nopy>; the first build compiles the engine too.
set "GAME_DIR=%~1"
if "!GAME_DIR!"=="" set "GAME_DIR=%CD%"
if not exist "!GAME_DIR!\CMakeLists.txt" (
    echo [ERROR] No CMakeLists.txt in !GAME_DIR!
    exit /b 1
)
set "TREE=Release"
echo %* | findstr /i /c:"IYI_WITH_PYTHON=OFF" >nul && set "TREE=nopy"

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

echo [IYI] Building the native game in !GAME_DIR! (!TREE!)...
"!CMAKE_EXE!" -S "!GAME_DIR!" -B "!GAME_DIR!\build\!TREE!" -G "Ninja" -DCMAKE_MAKE_PROGRAM="!NINJA_EXE!" -DCMAKE_BUILD_TYPE=Release %2 %3 %4 %5
if !ERRORLEVEL! neq 0 (
    echo [ERROR] CMake configuration failed
    exit /b 1
)
"!CMAKE_EXE!" --build "!GAME_DIR!\build\!TREE!"
if !ERRORLEVEL! neq 0 (
    echo [ERROR] Build failed
    exit /b 1
)
echo [IYI] Game built into its bin\ folder
exit /b 0
