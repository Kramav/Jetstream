@echo off
rem Copies REFramework's log out of the game folder before the next game start empties it:
rem   spikes\full_check\save_log.cmd 3   ->  spikes\out\full_check\logs\S3.txt
rem The game folder is remod's (Pipeline panel, Game folder).
if "%~1"=="" (echo usage: save_log.cmd ^<session number^> & exit /b 1)
powershell -NoProfile -Command ^
  "$s = Get-Content -Raw \"$env:APPDATA\remod\settings.json\" | ConvertFrom-Json;" ^
  "$log = Join-Path $s.games.re4r.game_dir 're2_framework_log.txt';" ^
  "$out = Join-Path '%~dp0..\out\full_check\logs' 'S%~1.txt';" ^
  "New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null;" ^
  "Copy-Item -LiteralPath $log $out; Write-Host ('saved ' + (Resolve-Path $out))"
