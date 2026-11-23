@echo off
rem Configure and build the engine with Emscripten into build\web\engine (engine root).
rem Extra args are build targets (default: all). Set IYI_WEB_GAME=<game folder> to preload another game.
call "%~dp0web_env.bat" || exit /b 1
set "OUT=%~dp0..\..\build\web\engine"
set "GAMEARG="
if defined IYI_WEB_GAME set "GAMEARG=-DIYI_WEB_GAME=%IYI_WEB_GAME%"
rem IYI_WEB_EXCLUDE: ";"-separated file_packager patterns kept out of game.data (web_build.py: git-ignored files).
set "GAMEARG=%GAMEARG% "-DIYI_WEB_EXCLUDE=%IYI_WEB_EXCLUDE%""
call emcmake cmake -S "%~dp0..\.." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DIYI_STEAM=OFF -DIYI_BUILD_TOOLS=OFF -DIYI_BUILD_TESTS=OFF %GAMEARG% || exit /b 1
if "%~1"=="" (cmake --build "%OUT%") else (cmake --build "%OUT%" --target %*)
