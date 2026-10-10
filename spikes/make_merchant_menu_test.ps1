# Builds the merchant menu test mod (spikes/merchant_menu_test.md): the merchant's first-menu layout with a 5th entry
# (spikes/gui_add_menu_item, REE-Lib) and the test script, zipped for Fluffy into spikes\out\. Needs the .NET 10 SDK and
# REE-Lib cloned to %LOCALAPPDATA%\remod\tools\RE-Engine-Lib (see the guide). The game file comes from your extracted
# files and goes only into the git-ignored out\ folder.
param([string]$Natives = "D:\Modding\Resident Evil\REtool\RE4\re_chunk_000\natives\stm")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$gamePath = "_chainsaw\ui\ui3500\gui\cs_ui3510.gui.540034"
$src = Join-Path $Natives $gamePath
if (-not (Test-Path $src)) { throw "not found: $src (pass -Natives <your natives\stm folder>)" }

$tool = Join-Path $here "gui_add_menu_item"
& "C:\Program Files\dotnet\dotnet.exe" build $tool -nologo -v q | Out-Null
if ($LASTEXITCODE -ne 0) { throw "building gui_add_menu_item failed" }
$exe = Get-ChildItem -Recurse (Join-Path $tool "out\bin") -Filter gui_add_menu_item.exe | Select-Object -First 1

$work = Join-Path $here "out\merchant_menu_test"
$mod = Join-Path $work "remod merchant menu test"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$guiOut = Join-Path $mod ("natives\STM\" + $gamePath)
New-Item -ItemType Directory -Force (Split-Path $guiOut) | Out-Null
# Run 2: two entries (each talk topic can be an entry of its own): si_menu_4, then si_menu_5 copied from it.
& $exe.FullName add $src $guiOut si_menu_3 si_menu_4
if ($LASTEXITCODE -ne 0) { throw "adding the 5th entry failed" }
& $exe.FullName add $guiOut $guiOut si_menu_4 si_menu_5
if ($LASTEXITCODE -ne 0) { throw "adding the 6th entry failed" }

$autorun = Join-Path $mod "reframework\autorun"
New-Item -ItemType Directory -Force $autorun | Out-Null
Copy-Item (Join-Path $here "merchant_menu_test.lua") $autorun
@"
name=remod merchant menu test
version=1
description=Two new entries in the merchant's first menu (spikes/merchant_menu_test.md). Needs REFramework.
author=remod
"@ -replace "`r?`n", "`r`n" | Set-Content -NoNewline -Encoding ascii (Join-Path $mod "modinfo.ini")

$zip = Join-Path $here "out\remod merchant menu test.zip"
if (Test-Path $zip) { Remove-Item $zip }
& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $work "remod merchant menu test"
if ($LASTEXITCODE -ne 0) { throw "zipping failed" }
Write-Output "built $zip"
