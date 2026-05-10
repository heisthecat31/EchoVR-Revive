@echo off
setlocal

echo Searching for MSVC build environment...

set "VARS_BAT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
            set "VARS_BAT=%%i\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
)

:: Fallback if vswhere is not found or fails
if not defined VARS_BAT (
    for %%G in (
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "J:\vs2026\VC\Auxiliary\Build\vcvars64.bat"
    ) do (
        if exist "%%G" (
            set "VARS_BAT=%%G"
        )
    )
)

if not defined VARS_BAT (
    echo [ERROR] MSVC Build environment not found. Please install Visual Studio with C++ tools.
    pause
    exit /b 1
)

:: Initialize environment if not already initialized
if not defined VSCMD_ARG_TGT_ARCH (
    call "%VARS_BAT%"
)

echo.
echo Building dbgcore.dll...
cl.exe /LD /MD /O2 /EHsc /Fe"dbgcore.dll" dllmain.cpp patches.cpp detours.lib shlwapi.lib user32.lib shell32.lib advapi32.lib

if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Build failed!
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo Moving dbgcore.dll to Plugins folder...
if not exist "Plugins" mkdir "Plugins"
move /Y dbgcore.dll Plugins\

echo Cleaning up build artifacts...
if exist "*.obj" del *.obj
if exist "dbgcore.exp" del dbgcore.exp
if exist "dbgcore.lib" del dbgcore.lib

echo.
echo Build successful!
pause
