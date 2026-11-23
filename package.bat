@echo off
setlocal enabledelayedexpansion

set "ENGINE_DIR=%~dp0"
if "%ENGINE_DIR:~-1%"=="\" set "ENGINE_DIR=%ENGINE_DIR:~0,-1%"

:: A native game (C++, no Python; SPEC 2c): package.bat native <game dir> [output dir] packages
:: <game dir>\bin\<name>.exe, built first with tools\build_game.bat <dir> "-DIYI_WITH_PYTHON=OFF".
if /i "%~1"=="native" goto :NATIVE

:: Arguments and defaults. Python 3.14 by default; "py311" first packages the legacy 3.11 engine
:: (build\ninja-py311). Versions match tools\runtime_manifest.json.
set "PY_FULL=3.14.8"
set "PY_TAG=314"
set "BUILD_ARG="
set "BUILD_DIR=!ENGINE_DIR!\build\ninja"
if /i "%~1"=="py311" (
    set "PY_FULL=3.11.9"
    set "PY_TAG=311"
    set "BUILD_ARG=py311"
    set "BUILD_DIR=!ENGINE_DIR!\build\ninja-py311"
    shift /1
)

set "GAME_SRC=%~1"
if "!GAME_SRC!"=="" set "GAME_SRC=%ENGINE_DIR%\..\HumanResources"

set "OUTPUT_DIR=%~2"
if "!OUTPUT_DIR!"=="" set "OUTPUT_DIR=%ENGINE_DIR%\dist"

echo [IYI] Packaging game for distribution...
echo [IYI] Game source: !GAME_SRC!
echo [IYI] Output dir:  !OUTPUT_DIR!

:: 1. Verify Release build exists at build\ninja\IYIEngine.exe (build\ninja-py311 for 3.11)
set "EXE_SRC=!BUILD_DIR!\IYIEngine.exe"
set "EXE_NAME=IYIEngine.exe"
if not exist "!EXE_SRC!" (
    echo [IYI] Release build not found at !EXE_SRC!. Building now...
    call "!ENGINE_DIR!\build.bat" !BUILD_ARG! Release
    if errorlevel 1 (
        echo [ERROR] Failed to build IYIEngine.exe. Please build it first.
        exit /b 1
    )
    if not exist "!EXE_SRC!" (
        echo [ERROR] Build completed but !EXE_SRC! was not found.
        exit /b 1
    )
)

:: Validate game source
if not exist "!GAME_SRC!\project.json" (
    echo [ERROR] Game project.json not found at !GAME_SRC!\project.json
    exit /b 1
)

:: Prepare output directories
if not exist "!OUTPUT_DIR!" mkdir "!OUTPUT_DIR!"
if not exist "!OUTPUT_DIR!\python" mkdir "!OUTPUT_DIR!\python"
if not exist "!OUTPUT_DIR!\game" mkdir "!OUTPUT_DIR!\game"

:: 2. Fetch python-<version>-embed-amd64.zip into third_party\ if not cached, then extract
set "THIRD_PARTY=!ENGINE_DIR!\third_party"
if not exist "!THIRD_PARTY!" mkdir "!THIRD_PARTY!"
set "PYTHON_EMBED=python-!PY_FULL!-embed-amd64.zip"
set "PYTHON_ZIP=!THIRD_PARTY!\!PYTHON_EMBED!"

if not exist "!PYTHON_ZIP!" (
    echo [IYI] Downloading !PYTHON_EMBED!...
    curl -fSL -o "!PYTHON_ZIP!" "https://www.python.org/ftp/python/!PY_FULL!/!PYTHON_EMBED!"
    if errorlevel 1 (
        echo [ERROR] Failed to download !PYTHON_EMBED! from python.org
        if exist "!PYTHON_ZIP!" del /f /q "!PYTHON_ZIP!"
        exit /b 1
    )
) else (
    echo [IYI] Using cached !PYTHON_EMBED!
)

echo [IYI] Extracting Python embed runtime...
powershell -NoProfile -Command "Expand-Archive -Path '!PYTHON_ZIP!' -DestinationPath '!OUTPUT_DIR!\python' -Force"
if errorlevel 1 (
    echo [ERROR] Failed to extract Python embed zip
    exit /b 1
)

if not exist "!OUTPUT_DIR!\python\python!PY_TAG!.dll" (
    echo [ERROR] Extraction failed: python!PY_TAG!.dll not found in !OUTPUT_DIR!\python
    exit /b 1
)
if not exist "!OUTPUT_DIR!\python\python!PY_TAG!.zip" (
    echo [ERROR] Extraction failed: python!PY_TAG!.zip not found in !OUTPUT_DIR!\python
    exit /b 1
)

:: Copy python3XX.dll next to exe so Windows loader resolves it at startup
copy /y "!OUTPUT_DIR!\python\python!PY_TAG!.dll" "!OUTPUT_DIR!\python!PY_TAG!.dll" >nul

:: 3. Copy IYIEngine.exe to <output>\
echo [IYI] Copying IYIEngine.exe...
copy /y "!EXE_SRC!" "!OUTPUT_DIR!\IYIEngine.exe" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy IYIEngine.exe to !OUTPUT_DIR!
    exit /b 1
)
:: A Steam-enabled build loads steam_api64.dll at startup
if exist "!BUILD_DIR!\steam_api64.dll" copy /y "!BUILD_DIR!\steam_api64.dll" "!OUTPUT_DIR!\steam_api64.dll" >nul

:: 4. Fold each "libraries" folder's scripts\ and assets\ into <output>\game\ (project.md 2; the game
:: wins, then earlier libraries: copied last to first so each overwrites the ones after it), then
:: the game's own scripts\ and assets\, excluding __pycache__, .pyc and .git.
echo [IYI] Copying library and game assets and scripts...
powershell -NoProfile -Command "$src = $env:GAME_SRC; $out = Join-Path $env:OUTPUT_DIR 'game'; $libs = @((Get-Content (Join-Path $src 'project.json') -Raw | ConvertFrom-Json).libraries | Where-Object { $_ }); [array]::Reverse($libs); foreach ($lib in $libs) { $root = Join-Path $src $lib; if (-not (Test-Path $root)) { Write-Host ('[ERROR] Library folder not found: ' + $root); exit 1 }; foreach ($d in 'scripts', 'assets') { $f = Join-Path $root $d; if (Test-Path $f) { robocopy $f (Join-Path $out $d) /E /XD __pycache__ .git /XF *.pyc /njh /njs /ndl /nc /ns /nfl > $null; if ($LASTEXITCODE -ge 8) { exit 1 } } } }; exit 0"
if errorlevel 1 (
    echo [ERROR] Failed to copy the libraries
    exit /b 1
)

if exist "!GAME_SRC!\scripts" (
    robocopy "!GAME_SRC!\scripts" "!OUTPUT_DIR!\game\scripts" /E /XD __pycache__ .git /XF *.pyc /njh /njs /ndl /nc /ns >nul
    if !ERRORLEVEL! GEQ 8 (
        echo [ERROR] Failed to copy scripts directory
        exit /b 1
    )
)

if exist "!GAME_SRC!\assets" (
    robocopy "!GAME_SRC!\assets" "!OUTPUT_DIR!\game\assets" /E /XD __pycache__ .git /njh /njs /ndl /nc /ns >nul
    if !ERRORLEVEL! GEQ 8 (
        echo [ERROR] Failed to copy assets directory
        exit /b 1
    )
)

:: Each "native" folder (project.md 2b) keeps its project-relative path; only its built modules.
powershell -NoProfile -Command "$src = $env:GAME_SRC; $out = Join-Path $env:OUTPUT_DIR 'game'; foreach ($n in @((Get-Content (Join-Path $src 'project.json') -Raw | ConvertFrom-Json).native | Where-Object { $_ })) { $f = Join-Path $src $n; if (Test-Path $f) { robocopy $f (Join-Path $out $n) *.pyd *.dll /njh /njs /ndl /nc /ns /nfl > $null; if ($LASTEXITCODE -ge 8) { exit 1 } } }; exit 0"
if errorlevel 1 (
    echo [ERROR] Failed to copy the native modules
    exit /b 1
)

:: 5. Write project.json without libraries (folded in above) and with hot_reload off
echo [IYI] Writing project.json with hot_reload=false and no libraries...
powershell -NoProfile -Command "$p = Get-Content (Join-Path $env:GAME_SRC 'project.json') -Raw | ConvertFrom-Json; $p.PSObject.Properties.Remove('libraries'); $p.PSObject.Properties.Remove('desktop_libraries'); Add-Member -InputObject $p -NotePropertyName 'hot_reload' -NotePropertyValue $false -Force; $p | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $env:OUTPUT_DIR 'game\project.json') -Encoding UTF8"
if errorlevel 1 (
    echo [ERROR] Failed to write the packaged project.json
    exit /b 1
)

:: 6. Copy MSVC runtime DLLs next to the exe
:MSVC_RUNTIME
echo [IYI] Locating MSVC runtime DLLs...
set "REDIST_COPIED=0"
set "VS_DIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" (
    for /f "delims=" %%i in ('^"!VSWHERE!^" -latest -property installationPath') do (
        set "VS_DIR=%%i"
    )
)
if not defined VS_DIR (
    if exist "C:\Program Files\Microsoft Visual Studio\18\Community" set "VS_DIR=C:\Program Files\Microsoft Visual Studio\18\Community"
)

if defined VS_DIR powershell -NoProfile -Command "$dll = Get-ChildItem -Path (Join-Path $env:VS_DIR 'VC\Redist\MSVC') -Recurse -Filter 'vcruntime140.dll' -ErrorAction SilentlyContinue | Where-Object { $_.DirectoryName -like '*x64*Microsoft.VC*.CRT' } | Select-Object -Last 1; if (-not $dll) { exit 1 }; foreach ($n in 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') { Copy-Item (Join-Path $dll.DirectoryName $n) $env:OUTPUT_DIR -Force -ErrorAction Stop }" && set "REDIST_COPIED=1"

if "!REDIST_COPIED!"=="0" (
    echo [WARNING] Could not locate MSVC runtime DLLs: vcruntime140.dll, vcruntime140_1.dll, msvcp140.dll
    echo [WARNING] The packaged folder may not run on machines without the MSVC Redistributable installed.
) else (
    echo [IYI] Copied MSVC runtime DLLs.
)

:: 7. Print summary
echo =======================================================================
echo [IYI] Packaging complete!
echo [IYI] Output directory: !OUTPUT_DIR!
powershell -NoProfile -Command "$sum = (Get-ChildItem -Path $env:OUTPUT_DIR -Recurse | Measure-Object -Property Length -Sum).Sum / 1MB; Write-Host ('[IYI] Total size:       {0:N2} MB' -f $sum)"
echo [IYI] The folder is now double-clickable: !OUTPUT_DIR!\!EXE_NAME!
echo =======================================================================
exit /b 0

:: A native game: its exe and the DLLs beside it (Steam), project.json and assets under game\ with
:: every library's assets folded in (they are namespaced, assets\<lib>\), no Python and no scripts.
:NATIVE
set "GAME_SRC=%~2"
set "OUTPUT_DIR=%~3"
if "!OUTPUT_DIR!"=="" set "OUTPUT_DIR=%ENGINE_DIR%\dist"
if not exist "!GAME_SRC!\project.json" (
    echo [ERROR] Game project.json not found at !GAME_SRC!\project.json
    exit /b 1
)
set "EXE_NAME="
for %%f in ("!GAME_SRC!\bin\*.exe") do set "EXE_NAME=%%~nxf"
if "!EXE_NAME!"=="" (
    echo [ERROR] No exe in !GAME_SRC!\bin. Build it: tools\build_game.bat ^<dir^> "-DIYI_WITH_PYTHON=OFF"
    exit /b 1
)
if exist "!GAME_SRC!\bin\python3*.dll" (
    echo [ERROR] !GAME_SRC!\bin holds a Python DLL: the exe was built with Python. Rebuild with "-DIYI_WITH_PYTHON=OFF".
    exit /b 1
)
echo [IYI] Packaging native game !EXE_NAME! from !GAME_SRC! into !OUTPUT_DIR!...
if not exist "!OUTPUT_DIR!\game" mkdir "!OUTPUT_DIR!\game"
robocopy "!GAME_SRC!\bin" "!OUTPUT_DIR!" *.exe *.dll /njh /njs /ndl /nc /ns >nul
if !ERRORLEVEL! GEQ 8 (
    echo [ERROR] Failed to copy the exe
    exit /b 1
)
if exist "!GAME_SRC!\assets" (
    robocopy "!GAME_SRC!\assets" "!OUTPUT_DIR!\game\assets" /E /XD .git /njh /njs /ndl /nc /ns >nul
    if !ERRORLEVEL! GEQ 8 (
        echo [ERROR] Failed to copy assets directory
        exit /b 1
    )
)
:: Library assets fill only what the game lacks (the game and earlier libraries win, SPEC 2);
:: project.json without libraries and with hot_reload off.
powershell -NoProfile -Command "$src = $env:GAME_SRC; $out = $env:OUTPUT_DIR; $p = Get-Content (Join-Path $src 'project.json') -Raw | ConvertFrom-Json; foreach ($lib in @($p.libraries)) { if ($lib) { $a = Join-Path (Join-Path $src $lib) 'assets'; if (Test-Path $a) { robocopy $a (Join-Path $out 'game\assets') /E /XC /XN /XO /njh /njs /ndl /nc /ns /nfl > $null; if ($LASTEXITCODE -ge 8) { exit 1 } } } }; $p.PSObject.Properties.Remove('libraries'); $p.PSObject.Properties.Remove('entry_script'); Add-Member -InputObject $p -NotePropertyName 'hot_reload' -NotePropertyValue $false -Force; $p | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $out 'game\project.json') -Encoding UTF8"
if errorlevel 1 (
    echo [ERROR] Failed to write the packaged project.json
    exit /b 1
)
goto :MSVC_RUNTIME
