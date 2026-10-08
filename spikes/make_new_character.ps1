# Builds the new character mod (spikes/character_probe.md, run 8 part 2): "rmc001", Ashley in a blue outfit, as a
# character of its own at new paths. Nothing of the game's is replaced:
#   - _chainsaw/character/ch/cha1/rmc001/00/rmc001_00.mesh: Ashley's body mesh (cha103_00), copied;
#   - rmc001_00.mdf2: her body material (cha103_00.mdf2) with the outfit's colour textures repointed from
#     ".../cha103/00/<name>_ALBD.tex" to ".../rmc001/00/<name>_ALBD.tex" (same length, so nothing else in the file
#     moves); the other maps (normals, masks...) stay the game's own;
#   - those colour textures and their streaming copies, recoloured blue by remod (Adjust colour);
#   - reframework/data/remod_puppets/rmc001.json: its definition (skeleton, motion banks, parts); head, hair (the
#     plain hair mesh, not the Strands) and skeleton are the game's Ashley's. The character probe's F8 builds it (later: the cutscene runtime's actors).
# Made from your extracted game files on your PC each time: game files are never committed or shipped.
#
#   powershell -ExecutionPolicy Bypass -File spikes\make_new_character.ps1 [-Darken 45] [-Out folder]
param(
    # Blue: red and green become the brightness of a copy darkened by this much (Adjust colour), blue stays. Her
    # blouse is near white, so a hue shift alone barely changed it: white turns light blue this way, dark deep blue.
    [int]$Darken = 45,
    [string]$Out = "$PSScriptRoot\out",  # git-ignored
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
$src = Join-Path $Natives "_chainsaw\character\ch\cha1\cha103\00"
if (-not (Test-Path "$src\cha103_00.mdf2.32")) { throw "Ashley's body not found under $Natives (pass -Natives <...\natives\stm>)" }
if (-not (Test-Path $Remod)) { throw "remod.exe not found at $Remod (pass -Remod)" }

$name = "remod new character rmc001"
$mod = Join-Path $Out $name
if (Test-Path $mod) { Remove-Item -Recurse -Force $mod }
$base = Join-Path $mod "natives\STM\_chainsaw\character\ch\cha1\rmc001\00"
$stream = Join-Path $mod "natives\STM\streaming\_chainsaw\character\ch\cha1\rmc001\00"
$work = Join-Path $Out "new_character_work"
New-Item -ItemType Directory -Force $base, $work, (Join-Path $mod "reframework\data\remod_puppets") | Out-Null

# The outfit's colour textures (not the hands: her skin stays as it is).
$pattern = "chc103_00_*_albd.tex.*;cha103_00_boots_albd.tex.*;cha103_button_fake_albd.tex.*;?_tex_mat_albd.tex.*"

# Recolour both copies with remod: Files in folder -> Original texture -> Export -> Adjust colour (darker) and Merge
# channels (red and green from the darker copy, alpha kept) -> Convert -> Copy.
$nodes = @(); $links = @()
function Chain([string]$folder, [string]$dest) {
    $i = $script:nodes.Count
    $script:nodes += @(
        @{ id = $i + 1; type = "FilesInFolder"; params = @{ folder = $folder; pattern = $pattern } },
        @{ id = $i + 2; type = "LoadTex"; params = @{} },
        @{ id = $i + 3; type = "Split"; params = @{} },
        @{ id = $i + 4; type = "ExportImage"; params = @{} },
        @{ id = $i + 5; type = "Split"; params = @{} },
        @{ id = $i + 6; type = "AdjustColour"; params = @{ brightness = "-$Darken" } },
        @{ id = $i + 7; type = "Split"; params = @{} },
        @{ id = $i + 8; type = "MergeChannels"; params = @{} },
        @{ id = $i + 9; type = "SaveTex"; params = @{} },
        @{ id = $i + 10; type = "CopyFile"; params = @{ dest = "$dest\{name}.tex.143221013"; if_exists = "overwrite"; create_dirs = "true" } })
    $script:links += @(
        @{ from = @(($i + 1), "files"); to = @(($i + 2), "tex") },
        @{ from = @(($i + 2), "tex"); to = @(($i + 3), "in") },
        @{ from = @(($i + 3), "out"); to = @(($i + 4), "tex") },
        @{ from = @(($i + 3), "out"); to = @(($i + 9), "original") },
        @{ from = @(($i + 4), "png"); to = @(($i + 5), "in") },
        @{ from = @(($i + 5), "out"); to = @(($i + 6), "image") },
        @{ from = @(($i + 5), "out"); to = @(($i + 8), "base") },
        @{ from = @(($i + 6), "image"); to = @(($i + 7), "in") },
        @{ from = @(($i + 7), "out"); to = @(($i + 8), "red") },
        @{ from = @(($i + 7), "out"); to = @(($i + 8), "green") },
        @{ from = @(($i + 8), "image"); to = @(($i + 9), "image") },
        @{ from = @(($i + 9), "tex"); to = @(($i + 10), "source") })
}
Chain $src $base
Chain (Join-Path $Natives "streaming\_chainsaw\character\ch\cha1\cha103\00") $stream
@{ schema_version = 0; profile = "re4r"; nodes = $nodes; links = $links } | ConvertTo-Json -Depth 6 |
    Set-Content -Encoding utf8 "$work\recolour.json"
& $Remod run --graph "$work\recolour.json"
if ($LASTEXITCODE) { throw "remod run failed" }
$made = @(Get-ChildItem $base -Filter *.tex.*).Count
if ($made -eq 0 -or @(Get-ChildItem $stream -Filter *.tex.*).Count -ne $made) { throw "recolouring made $made textures, streaming copies don't match" }

# The material, repointed to the recoloured textures.
$bytes = [IO.File]::ReadAllBytes("$src\cha103_00.mdf2.32")
$text = [Text.Encoding]::Unicode.GetString($bytes)  # strings are 2-byte aligned: char i = byte 2i
$prefix = "_Chainsaw/Character/ch/cha1/"
$hits = [regex]::Matches($text, [regex]::Escape($prefix) + "cha103/00/(?!cha103_00_hand)\w+_ALBD\.tex", "IgnoreCase")
$to = [Text.Encoding]::Unicode.GetBytes("rmc001")
foreach ($h in $hits) { [Array]::Copy($to, 0, $bytes, 2 * ($h.Index + $prefix.Length), $to.Length) }
$named = @($hits | ForEach-Object { ($_.Value -split '/')[-1] -replace '\.tex$', '' } | Sort-Object -Unique)  # a texture is named by several materials
$missing = @($named | Where-Object { -not (Test-Path "$base\$_.tex.143221013") })
if ($named.Count -ne $made -or $missing) { throw "the material names $($named.Count) outfit textures, $made were recoloured (missing: $missing)" }
[IO.File]::WriteAllBytes("$base\rmc001_00.mdf2.32", $bytes)
Copy-Item "$src\cha103_00.mesh.221108797" "$base\rmc001_00.mesh.221108797"
"  rmc001_00.mdf2: $($hits.Count) texture paths repointed to $made recoloured textures (and their streaming copies)"

# Its definition, for the probe's F8 (and later the cutscene runtime).
$c1 = "_Chainsaw/Character/ch/cha1/"
$def = @{
    name = "rmc001 (blue Ashley, a new character)"
    skeleton = "${c1}cha1.fbxskel"
    motion_bank = "_Chainsaw/AppSystem/Character/ch2Common/Motion/ch2CommonBank.motbank"
    dynamic_banks = @("_Chainsaw/Animation/ch/cha1/motbank/cha1_NPC.motbank")
    layers = 13
    parts = @(
        @{ name = "body"; mesh = "${c1}rmc001/00/rmc001_00.mesh"; material = "${c1}rmc001/00/rmc001_00.mdf2" },
        @{ name = "head"; mesh = "${c1}cha100/10/cha100_10.mesh"; material = "${c1}cha100/10/cha100_10.mdf2" },
        # Run 8: Strands hair built from files didn't show; Luis's plain hair mesh (his 20, as the game's events use)
        # did. Ashley's 20 is the same kind (Character_Hair.mmtr; the live one's "raytrace_mesh").
        @{ name = "hair"; mesh = "${c1}cha103/20/cha103_20.mesh"; material = "${c1}cha103/20/cha103_20.mdf2" },
        @{ name = "ac2100_10"; mesh = "_Chainsaw/Character/ac/ac00/ac2100/_10/ac2100_10.mesh"
           material = "_Chainsaw/Character/ac/ac00/ac2100/_10/ac2100_10.mdf2"; parent_joint = "Head" })
}
[IO.File]::WriteAllText((Join-Path $mod "reframework\data\remod_puppets\rmc001.json"), ($def | ConvertTo-Json -Depth 6))  # no BOM

"name=$name`r`nversion=1`r`ndescription=remod spike: a new character (Ashley in blue, rmc001) at new paths, nothing replaced. Built by the character probe's F8. Needs REFramework.`r`nauthor=remod`r`n" |
    Set-Content -Encoding ascii -NoNewline (Join-Path $mod "modinfo.ini")

$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
& "$env:SystemRoot\System32\tar.exe" -a -cf $zip -C $Out $name
if ($LASTEXITCODE) { throw "tar failed" }
"Built $zip"
