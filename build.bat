@echo off
REM Build MewPaletteExtender.dll!

set "DESTINATION_DIR=C:\Users\Pseudonym_Tim\Desktop\Tools\Mewtator\mods\MewPaletteExtender"
REM set "DESTINATION_DIR=D:\SteamLibrary\steamapps\common\Mewgenics"

REM Toggle deployment mode:
REM true = Mewtator deploy (Mewtator deploy, set to existing Mewtator mod folder)
REM false = Normal deploy (Normal deploy, set to game root directory)
set "MEWTATOR_DEPLOY=true"

setlocal

REM Locate Visual Studio via vswhere...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Is Visual Studio installed?
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do (
    set "VSDIR=%%i"
)

if not defined VSDIR (
    echo ERROR: Could not find a Visual Studio installation.
    pause
    exit /b 1
)

if not exist "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo ERROR: vcvarsall.bat not found at "%VSDIR%\VC\Auxiliary\Build\"
    pause
    exit /b 1
)

echo Setting up x64 MSVC environment...
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

REM Build...
echo.
echo Building MewPaletteExtender.dll...

cl /LD /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS /TC src\MewPaletteExtender.c /Fe:MewPaletteExtender.dll /link user32.lib ole32.lib windowscodecs.lib

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build FAILED.
    pause
    exit /b 1
)

echo.
echo Build succeeded!

REM Clean intermediate files...
del /Q MewPaletteExtender.obj MewPaletteExtender.lib MewPaletteExtender.exp 2>nul

REM Determine deploy path...
if /I "%MEWTATOR_DEPLOY%"=="true" (
    set "DEPLOY_DIR=%DESTINATION_DIR%"
) else (
    set "DEPLOY_DIR=%DESTINATION_DIR%\mods"
)

REM Create deploy directory if needed...
if not exist "%DEPLOY_DIR%" (
    mkdir "%DEPLOY_DIR%"
)

REM Deploy main files...
copy /Y MewPaletteExtender.dll "%DEPLOY_DIR%\MewPaletteExtender.dll"

if exist description.json (
    copy /Y description.json "%DEPLOY_DIR%\description.json"
)

if exist preview.png (
    copy /Y preview.png "%DEPLOY_DIR%\preview.png"
)

REM Deploy palette configuration...
if exist palette_rows.txt (
    copy /Y palette_rows.txt "%DEPLOY_DIR%\palette_rows.txt"
) else (
    echo WARNING: palette_rows.txt not found; skipping palette row deploy.
)

REM Deploy PNG palette strips...
if exist palettes\ (
    if not exist "%DEPLOY_DIR%\palettes" (
        mkdir "%DEPLOY_DIR%\palettes"
    )

    robocopy palettes "%DEPLOY_DIR%\palettes" *.png /E >nul

    if %ERRORLEVEL% GEQ 8 (
        echo ERROR: Failed to copy PNG palette strips.
        pause
        exit /b 1
    )
) else (
    echo WARNING: palettes folder not found, skipping PNG palette strip deploy...
)

echo Deployed to %DEPLOY_DIR%
pause