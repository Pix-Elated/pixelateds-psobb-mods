@echo off
setlocal

:: Locate VS 2025 Community vcvarsall.bat
set "VCVARSALL=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARSALL%" (
    echo ERROR: vcvarsall.bat not found at:
    echo   %VCVARSALL%
    exit /b 1
)

:: Set up x86 MSVC environment (compiler, linker, SDK headers+libs)
call "%VCVARSALL%" x86 >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvarsall.bat x86 failed.
    exit /b 1
)

:: Run cmake configure if build dir doesn't exist yet. %~dp0 ends with a
:: backslash, and "C:\foo\" breaks cmd's quote parser — strip the trailing
:: slash before quoting.
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
if not exist "%BUILD%\build.ninja" (
    echo Configuring CMake...
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S "%ROOT%" -B "%BUILD%"
    if errorlevel 1 exit /b 1
)

:: Build
cd /d "%BUILD%"
ninja %*
exit /b %errorlevel%
