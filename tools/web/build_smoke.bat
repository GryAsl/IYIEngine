@echo off
rem Gate 1: build the raylib + WebGL 2 smoke into build\web\smoke-raylib (engine root).
call "%~dp0web_env.bat" || exit /b 1
set "OUT=%~dp0..\..\build\web\smoke-raylib"
call emcmake cmake -S "%~dp0smoke\raylib" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%OUT%" || exit /b 1
