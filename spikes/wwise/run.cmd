@echo off
rem Converts every sample WAV in in\ with Wwise's command line (README.md): wem\vorbis, wem\opus, wem\pcm.
setlocal
set "HERE=%~dp0"

set "CONSOLE="
if defined WWISEROOT set "CONSOLE=%WWISEROOT%\Authoring\x64\Release\bin\WwiseConsole.exe"
if not exist "%CONSOLE%" set "CONSOLE=%ProgramFiles(x86)%\Audiokinetic\Wwise 2021.1.14.8108\Authoring\x64\Release\bin\WwiseConsole.exe"
if not exist "%CONSOLE%" (
    echo WwiseConsole.exe not found. Install Wwise 2021.1.14 with the Audiokinetic Launcher ^(README.md step 1^).
    exit /b 1
)
echo Using %CONSOLE%

set "PROJECT="
for /r "%HERE%project" %%P in (*.wproj) do set "PROJECT=%%P"
if not defined PROJECT (
    echo No Wwise project under %HERE%project\ ^(README.md step 2^).
    exit /b 1
)
echo Project %PROJECT%

if not exist "%HERE%in\tone_mono.wav" (
    echo The sample WAVs aren't in %HERE%in\ ^(make them with make_samples.sh^).
    exit /b 1
)

powershell -NoProfile -Command "(Get-Content '%HERE%samples.wsources' -Raw).Replace('Root=\"in\"', 'Root=\"%HERE%in\"') | Set-Content '%HERE%run.wsources'"
"%CONSOLE%" convert-external-source "%PROJECT%" --platform Windows --source-file "%HERE%run.wsources" --output "%HERE%wem" > "%HERE%wem_log.txt" 2>&1
set "RESULT=%ERRORLEVEL%"
type "%HERE%wem_log.txt"
echo.
if not "%RESULT%"=="0" (
    echo WwiseConsole failed ^(exit %RESULT%^). Its output is in wem_log.txt.
    exit /b %RESULT%
)
echo Done. The .wem files are under %HERE%wem
