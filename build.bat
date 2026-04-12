@echo off
REM Build wrapper for pixelated_mods (MSVC 32-bit / x86).
REM
REM Bootstraps the MSVC x86 cross-compiler environment via vcvarsall.bat
REM so the developer does not need to start a "Developer Command Prompt"
REM shell manually. Configures CMake with the Ninja generator on the
REM first run, then delegates subsequent calls to `cmake --build`.
REM
REM Usage:
REM   build.bat               -> configure (if needed) and build
REM   build.bat clean         -> delete build\ and start fresh
REM   build.bat -- <ninja..>  -> pass extra args to the underlying build

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%build"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"

if not exist "%VCVARS%" (
    echo [build.bat] ERROR: vcvarsall.bat not found at:
    echo   %VCVARS%
    echo Install Visual Studio 2022 with "Desktop development with C++" ^(x86^).
    exit /b 1
)

REM Put the VS Installer directory on PATH so vcvarsall.bat can locate
REM vswhere.exe. Without this, vcvarsall prints "'vswhere.exe' is not
REM recognized" before falling back to hardcoded detection — the build
REM still works but the noise is ugly.
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"

if /I "%~1"=="clean" (
    echo [build.bat] Removing %BUILD_DIR%
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    shift
)

REM Source the MSVC x86 environment. vcvarsall.bat picks Hostx64\x86 on
REM a 64-bit host, which is the faster cross-compiler.
call "%VCVARS%" x86 >nul
if errorlevel 1 (
    echo [build.bat] vcvarsall.bat x86 failed
    exit /b 1
)

pushd "%SCRIPT_DIR%" >nul

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [build.bat] Configuring ^(Ninja, Release, MSVC x86^)...
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build
    if errorlevel 1 (
        popd >nul
        exit /b 1
    )
)

echo [build.bat] Building...
cmake --build build --config Release %*
set "RC=%ERRORLEVEL%"

popd >nul
exit /b %RC%
