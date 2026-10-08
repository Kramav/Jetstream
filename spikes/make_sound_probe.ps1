# Builds the sound probe's mod (spikes/sound_probe.md) as a zip for Fluffy, from your extracted game files (nothing of
# the game's is replaced, and no game file is committed):
#   - remod_snd001.sbnk.1.x64: a brand-new sound bank, `remod new-sound` from ch_csa404_se (one event, one sound):
#     every id new, 3 s of beeps; its note reframework/data/remod_sounds/remod_snd001.json (path and event id);
#   - rmd002: a New movie (12 s test card) with its sound in the movie file (a beep each second), with the cutscene
#     runtime that plays it from REFramework's menu;
#   - the probe script.
#   powershell -ExecutionPolicy Bypass -File spikes\make_sound_probe.ps1 [-Out folder]
param(
    [string]$Out = "$env:USERPROFILE\Documents\remod\spikes",
    [string]$Natives = "",
    [string]$Remod = "$PSScriptRoot\..\build\cli\remod.exe"
)
$ErrorActionPreference = "Stop"

if (-not $Natives) {  # the extracted natives\STM, from the RE plugin's file beside Noesis (as remod finds it)
    $settings = Get-Content "$env:APPDATA\remod\settings.json" -Raw | ConvertFrom-Json
    $txt = Join-Path (Split-Path $settings.noesis_path) "plugins\python\RE4NativesPath.txt"
    $Natives = (Get-Content $txt -TotalCount 1).Trim().TrimEnd('\')
    if ($Natives -notmatch 'natives\\stm$') { $Natives = Join-Path $Natives "natives\stm" }
}
$like = Join-Path $Natives "_chainsaw\sound\wwise\ch_csa404_se.sbnk.1.x64"
if (-not (Test-Path $like)) { throw "ch_csa404_se not found under $Natives (pass -Natives <...\natives\stm>)" }
if (-not (Test-Path $Remod)) { throw "remod.exe not found at $Remod (pass -Remod)" }

$name = "remod sound probe"
$work = Join-Path $Out "sound_probe_work"
New-Item -ItemType Directory -Force $work | Out-Null

# The new movie with sound in its file, packaged by remod (with the cutscene runtime).
$graph = @{
    schema_version = 0; profile = "re4r"
    nodes = @(
        @{ id = 1; type = "NewMovie"; params = @{ name = "rmd002"; video = ""; sound = "true" } },
        @{ id = 2; type = "PackageMod"; params = @{ name = $name; out = $Out; version = "1"; replace = "true";
            description = "remod spike: play sounds on command (F7 a game sound, F5 a brand-new one)" } }
    )
    links = @(@{ from = @(1, "files"); to = @(2, "file") })
}
$graph | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 "$work\sound_probe.json"
& $Remod run --graph "$work\sound_probe.json"
if ($LASTEXITCODE) { throw "remod run failed" }

# The brand-new bank, its note and the probe, added to that mod folder.
$mod = Join-Path $Out $name
$wwise = Join-Path $mod "natives\STM\_chainsaw\sound\wwise"
New-Item -ItemType Directory -Force $wwise, (Join-Path $mod "reframework\data\remod_sounds") | Out-Null
$made = & $Remod new-sound --like $like --name remod_snd001 --out (Join-Path $wwise "remod_snd001.sbnk.1.x64")
if ($LASTEXITCODE -or -not ($made -match 'bank (\d+) event (\d+)')) { throw "remod new-sound failed: $made" }
$event = [uint32]$Matches[2]
"  new bank: $made"
@{ name = "remod_snd001"; bank = "@_Chainsaw/Sound/Wwise/remod_snd001.sbnk"; event = $event } | ConvertTo-Json |
    Set-Content -Encoding ascii (Join-Path $mod "reframework\data\remod_sounds\remod_snd001.json")
Copy-Item "$PSScriptRoot\sound_probe.lua" (Join-Path $mod "reframework\autorun\remod_sound_probe.lua")

$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
& "$env:SystemRoot\System32\tar.exe" -a -cf $zip -C $Out $name
if ($LASTEXITCODE) { throw "tar failed" }
"Built $zip"
