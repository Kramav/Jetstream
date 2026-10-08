# Builds the new movie probe's mod (spikes/new_movie_probe.md): a movie `rmd001` at new paths, nothing of the game's
# replaced, plus the probe script, as a zip for Fluffy.
#   - rmd001.mov.1.x64 / rmd001_fhd.mov.1.x64 (streaming/): your video encoded as mva000's 4K and 1080p copies are
#     (remod's Replace movie, its own length, no sound packages);
#   - the same two names outside streaming/: the game's 38-byte stub (every movie's is the same);
#   - rmd001.pfb.17.x64, rmd001_fhd.pfb.17: mva000's, with "mv/mva000/mva000" changed to "mv/rmd001/rmd001" in
#     their paths (same length, so nothing else in the files moves). Their sound container stays mva000's; the probe
#     clears the movie's sound trigger. (The MovieResource is made in the probe from mva000's, not shipped: run 1's
#     create_userdata of a shipped .user gave an empty one.)
# Made from your extracted game files on your PC each time: game files are never committed or shipped.
#
#   powershell -ExecutionPolicy Bypass -File spikes\make_new_movie.ps1 [-Video my.mp4] [-Out folder]
# Without -Video: a 12 s test pattern made with ffmpeg (if installed).
param(
    [string]$Video = "",
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
$src = Join-Path $Natives "_chainsaw\movie\mv\mva000"
if (-not (Test-Path "$src\mva000.pfb.17.x64")) { throw "mva000 not found under $Natives (pass -Natives <...\natives\stm>)" }
if (-not (Test-Path $Remod)) { throw "remod.exe not found at $Remod (pass -Remod)" }

$work = Join-Path $Out "new_movie_work"
New-Item -ItemType Directory -Force $work | Out-Null
if (-not $Video) {
    $Video = Join-Path $work "test_pattern.mp4"
    if (-not (Test-Path $Video)) {
        $ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
        if (-not $ff) { throw "no -Video given and no ffmpeg to make a test pattern" }
        & $ff -loglevel error -y -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=12" -pix_fmt yuv420p $Video
        if ($LASTEXITCODE) { throw "ffmpeg failed" }
    }
}

# Encode with remod's Replace movie, copying both results out of the run cache.
$graph = @{
    schema_version = 0; profile = "re4r"
    nodes = @(
        @{ id = 1; type = "ReplaceMovie"; params = @{ movie = "$Natives\streaming\_chainsaw\movie\mv\mva000\mva000.mov.1.x64";
            video = $Video; same_length = "false"; sound = "false" } },
        @{ id = 2; type = "CopyFile"; params = @{ dest = "$work\rmd001.mov.1.x64"; if_exists = "overwrite"; create_dirs = "true" } },
        @{ id = 3; type = "CopyFile"; params = @{ dest = "$work\rmd001_fhd.mov.1.x64"; if_exists = "overwrite"; create_dirs = "true" } }
    )
    links = @(@{ from = @(1, "movie"); to = @(2, "source") }, @{ from = @(1, "fhd"); to = @(3, "source") })
}
$graph | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 "$work\encode.json"
& $Remod run --graph "$work\encode.json"
if ($LASTEXITCODE) { throw "remod run failed" }

# The mod folder.
$name = "remod new movie probe"
$mod = Join-Path $Out $name
if (Test-Path $mod) { Remove-Item -Recurse -Force $mod }
$base = Join-Path $mod "natives\STM\_chainsaw\movie\mv\rmd001"
$stream = Join-Path $mod "natives\STM\streaming\_chainsaw\movie\mv\rmd001"
New-Item -ItemType Directory -Force $base, $stream, (Join-Path $mod "reframework\autorun") | Out-Null
Copy-Item "$work\rmd001.mov.1.x64" "$stream\rmd001.mov.1.x64"
Copy-Item "$work\rmd001_fhd.mov.1.x64" "$stream\rmd001_fhd.mov.1.x64"
Copy-Item "$src\mva000.mov.1.x64" "$base\rmd001.mov.1.x64"
Copy-Item "$src\mva000_fhd.mov.1.x64" "$base\rmd001_fhd.mov.1.x64"

$from = [Text.Encoding]::Unicode.GetBytes("mv/mva000/mva000")
$to = [Text.Encoding]::Unicode.GetBytes("mv/rmd001/rmd001")
function Patch([string]$in, [string]$outFile) {
    $b = [IO.File]::ReadAllBytes($in)
    $n = 0
    for ($i = 0; $i -le $b.Length - $from.Length; $i++) {
        $hit = $true
        for ($j = 0; $j -lt $from.Length; $j++) { if ($b[$i + $j] -ne $from[$j]) { $hit = $false; break } }
        if ($hit) { [Array]::Copy($to, 0, $b, $i, $to.Length); $n++ }
    }
    if ($n -eq 0) { throw "no mva000 path in $in" }
    [IO.File]::WriteAllBytes($outFile, $b)
    "  $(Split-Path $outFile -Leaf): $n path(s) changed"
}
Patch "$src\mva000.pfb.17.x64" "$base\rmd001.pfb.17.x64"
Patch "$src\mva000_fhd.pfb.17" "$base\rmd001_fhd.pfb.17"

Copy-Item "$PSScriptRoot\new_movie_probe.lua" (Join-Path $mod "reframework\autorun\remod_new_movie_probe.lua")
"name=$name`r`nversion=1`r`ndescription=remod spike: a new movie id (rmd001) played in a cinematic state. Needs REFramework.`r`nauthor=remod`r`n" |
    Set-Content -Encoding ascii -NoNewline (Join-Path $mod "modinfo.ini")

$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
& "$env:SystemRoot\System32\tar.exe" -a -cf $zip -C $Out $name
if ($LASTEXITCODE) { throw "tar failed" }
"Built $zip"
