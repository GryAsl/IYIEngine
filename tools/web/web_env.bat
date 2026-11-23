@echo off
rem Puts emsdk (EMSDK, default third_party\web\emsdk from setup_toolchain.py) and VS CMake/Ninja on PATH.
if not defined EMSDK set "EMSDK=%~dp0..\..\third_party\web\emsdk"
if not exist "%EMSDK%\emsdk_env.bat" (echo web_env: emsdk not found at %EMSDK%, run: py tools\web\setup_toolchain.py & exit /b 1)
rem Started from Git Bash, MSYSTEM would make emsdk print bash exports instead of setting this cmd's PATH.
set "MSYSTEM="
call "%EMSDK%\emsdk_env.bat" >nul 2>&1
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VSDIR=%%i"
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
