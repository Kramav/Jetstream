@echo off
rem Rebuilds remod (app, CLI, tests). Double-click it, or run it from any terminal.
rem Pass "test" to also run the tests:  rebuild.bat test
setlocal
cd /d "%~dp0"

tasklist /FI "IMAGENAME eq remod-app.exe" 2>nul | find /I "remod-app.exe" >nul
if not errorlevel 1 (
    echo remod-app is running. Close it first, then run this again.
    goto done
)

rem Find Visual Studio / Build Tools and load its developer environment (compiler, CMake, vcpkg).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer not found. Install Visual Studio Build Tools 2026 - see README.md.
    goto done
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo No Visual Studio install with the C++ tools found - see README.md.
    goto done
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul

if not exist build\build.ninja (
    echo Configuring - the first time takes a few minutes while vcpkg builds the libraries...
    cmake --preset default || goto failed
)
cmake --build build || goto failed

if /I "%~1"=="test" (
    ctest --test-dir build --output-on-failure || goto failed
)

echo.
echo Build succeeded. App: build\app\remod-app.exe   CLI: build\cli\remod.exe
goto done

:failed
echo.
echo BUILD FAILED - see the messages above.

:done
endlocal
pause
