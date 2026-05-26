@echo off
set "SCRIPT_DIR=%~dp0"
set "PROJECT_DIR=%SCRIPT_DIR:\=/%"
set "PROJECT_DIR=/%PROJECT_DIR::=%"

:: Look for devkitPro MSYS2 path first, fallback to vanilla msys64
set "MSYS2_PATH=C:\devkitPro\msys2\usr\bin\bash.exe"
if not exist "%MSYS2_PATH%" (
    set "MSYS2_PATH=C:\msys64\usr\bin\bash.exe"
)

if not exist "%MSYS2_PATH%" (
    echo ERROR: Could not find bash.exe. Please ensure devkitPro is installed correctly.
    pause
    exit /b 1
)

setlocal enabledelayedexpansion
for %%f in (Textures\*.png) do (
    set "filename=%%~nf"
    if not exist "Textures\!filename!.t3s" (
        echo Creating Textures\!filename!.t3s for %%f...
        (
            echo --format=rgba8
            echo %%~nxf
        ) > "Textures\!filename!.t3s"
    ) else (
        echo Textures\!filename!.t3s already exists, skipping.
    )
)

echo.
echo Generated t3s descriptors.\n
echo.
setlocal disabledelayedexpansion
echo Building App...
%MSYS2_PATH% -lc "cd %PROJECT_DIR% && make"

if %ERRORLEVEL% equ 0 (
    if not exist BuildOutputs mkdir BuildOutputs
    echo Build successful. Copying to BuildOutputs...
    copy KwaCam.3dsx BuildOutputs\
    del KwaCam.3dsx
    if exist KwaCam.elf del KwaCam.elf
    if exist KwaCam.smdh del KwaCam.smdh
) else (
    echo Build failed.
    exit /b %ERRORLEVEL%
)
echo.
echo Built KwaCam.3dsx to BuildOutputs.
pause