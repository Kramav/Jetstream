# Jetstream — RE Engine Modding Tool

Node-based modding tool for Capcom RE Engine games. First target: Resident Evil 4 (2023).

**Status:** Milestone 1 in progress. A node graph (texture → PNG → edited texture → Fluffy-ready zip)
runs from the desktop app and from the CLI.

## Requirements

- Windows 10/11
- **Visual Studio Build Tools 2026** (or Visual Studio 2026) with the C++ desktop build tools. That provides:
  - MSVC and the Windows SDK
  - CMake and Ninja
  - vcpkg, which fetches the libraries below automatically
- Git and an internet connection for the first build (vcpkg downloads library sources)

You do not install any libraries by hand. `vcpkg.json` lists them and pins their versions:
toml++, Catch2, Dear ImGui (docking), imgui-node-editor.

To convert textures you also need **Noesis** with the **fmt_RE_MESH** plugin in `Noesis\plugins\python`.
Neither is bundled (licensing); you point the tool at your `Noesis64.exe`.

## Build

**Quick way:** double-click **`rebuild.bat`** in the repo root. It finds Visual Studio itself, configures on
first use and builds. `rebuild.bat test` also runs the tests. Close `remod-app` first; the script
stops and tells you if it's still open.

**Manual way:**

1. Open **Developer PowerShell for VS 2026** from the Start menu. A normal PowerShell window won't work,
   because the developer shell is what puts CMake and the compiler on PATH and sets `VCPKG_ROOT`.
2. Go to the repo root (the folder containing `CMakePresets.json`) and build:

```powershell
cd E:\Company\Github\Jetstream
cmake --preset default
cmake --build build
```

The first `cmake --preset default` takes a few minutes while vcpkg builds the libraries. Later runs reuse them.
After pulling changes, `cmake --build build` alone is usually enough.

Ignore the `'vswhere.exe' is not recognized` line the developer shell may print; it's harmless.

## Run the tests

```powershell
ctest --test-dir build --output-on-failure
```

Tests need no game files. One test, the texture round trip, runs only if you point it at local textures
and Noesis. Otherwise it's reported as skipped:

```powershell
$env:REMOD_FIXTURES = "C:\spike"   # folder of original *.tex.143221013 files (never commit these)
$env:REMOD_NOESIS   = "D:\path\to\Noesis64.exe"
ctest --test-dir build --output-on-failure
```

## Outputs

| File | What it is |
|---|---|
| `build\app\remod-app.exe` | Desktop app: node graph editor with a Run button. |
| `build\cli\remod.exe` | Command-line tool: `run` (a saved graph), `tex2png`, `png2tex`, `package`. |
| `build\tests\remod_tests.exe` | Test runner (usually run through `ctest`). |

## Making a texture mod with a graph

A graph chains the steps: **LoadTex → ExportImage → SaveTex → PackageMod** (plus a link from LoadTex to
SaveTex's `original` input). `schemas\graph.v0.example.json` is a ready-made one; copy it and change the paths.
Relative paths in a graph are relative to the graph file's folder.

It takes **two runs**:
1. **First run:** exports the texture to the PNG named in ExportImage, then stops and tells you to edit it.
2. Edit the PNG. Keep the same width and height.
3. **Second run:** ExportImage keeps your edited PNG, SaveTex encodes it like the original, and PackageMod
   writes the mod folder and zip.

If the `.tex` sits inside an extracted `natives\STM\...` folder, LoadTex works out the in-game path by itself.
Otherwise fill in LoadTex's `game_path`.

Texture file names don't matter: `x.tex`, `x.tex.143221013` or a renamed file all work. The game is
detected from the texture's header, and the in-game version suffix (e.g. `.143221013`) is added when
packaging. Picking a texture in the app switches the graph's **Game** to the one the texture belongs to.
Supporting another RE Engine game means adding a profile in `profiles\`; its textures are then recognised too.

**From the app:**
1. Start `build\app\remod-app.exe`.
2. In the **Pipeline** panel, pick a graph with the **`...`** button next to *Graph file* (this loads it),
   or build one: right-click the canvas to add nodes, drag from an output pin to an input pin to link,
   and press Delete to remove. Pick `Noesis64.exe` with its `...` button, then click **Run**.
3. Every path field in a node has a **`...`** button that opens the Windows file or folder picker.
4. The panel lists any problems (missing fields, unconnected inputs) and shows the run log.
5. **Save** / **Save As...** write the graph, including node positions.

The app remembers the graph file and Noesis paths between sessions in `%APPDATA%\remod\settings.json`.
It writes that file only when you Load, Save or Run and a path changed.
It looks for the `profiles` folder in the current folder, then next to and above the exe; from
`build\app` it finds the repo's.

**From the CLI** (normal PowerShell, repo root):

```powershell
.\build\cli\remod.exe run --graph C:\work\my_mod.json --noesis "D:\path\to\Noesis64.exe"
```

## Single steps with the CLI

The same steps are also available as individual commands:

Run from the repo root in a normal PowerShell window. `$noesis` is just shorthand:

```powershell
$noesis = "D:\path\to\Noesis64.exe"

# 1. Original texture -> PNG (also prints size, format and mip count)
.\build\cli\remod.exe tex2png --profile profiles\re4r.toml --noesis $noesis `
  --tex "C:\work\my_texture.tex.143221013" --out "C:\work\my_texture.png"

# 2. Edit the PNG in any image editor. Keep the same width and height.

# 3. Edited PNG -> new texture, using the original as the template (same format and mips)
.\build\cli\remod.exe png2tex --profile profiles\re4r.toml --noesis $noesis `
  --png "C:\work\my_texture.png" --original "C:\work\my_texture.tex.143221013" `
  --out "C:\work\new.tex.143221013"

# 4. Package for Fluffy
.\build\cli\remod.exe package --profile profiles\re4r.toml `
  --tex "C:\work\new.tex.143221013" `
  --game-path _chainsaw/ui/ui3200/tex/my_texture.tex.143221013 `
  --name MyMod --out E:\mods `
  --author "Me" --version 1.0 --description "What it does" --screenshot "C:\work\preview.png"
```

`png2tex` checks the result against the original (size, format, mip count) and fails rather than
producing a mismatched texture. Multi-image (array) textures aren't supported yet.

Step 4 creates `E:\mods\MyMod\` containing `modinfo.ini`, the screenshot and `natives\STM\<game path>`,
plus **`E:\mods\MyMod.zip`** with the `MyMod` folder at its root. Add that zip to Fluffy Mod Manager.
Zipping uses the `tar.exe` built into Windows 10 (1803+) and 11, so nothing extra needs installing.

- **`.\` is required.** PowerShell won't run an exe by relative path without it.
- **Quote any path that contains spaces.**
- **`--game-path`** is the file's path inside the game, relative to `natives/STM`, with no leading slash.
- **Put `--out` outside the repo**, or the output shows up as untracked files in git.
- The tool never overwrites: it refuses if an output file, the `MyMod` folder or `MyMod.zip` already exists.

## Troubleshooting

- **`VCPKG_ROOT` is empty or the toolchain file isn't found:** you're not in the Developer PowerShell. See step 1.
- **Something is badly broken:** delete the `build` folder and run the commands again.
- **Path too long:** keep the repo and output folders near the drive root (e.g. `E:\mods`). Windows'
  260-character path limit is a known problem for RE Engine mods.

## Rules for contributors

- Never commit game files (`natives/`, `*.tex.*`, `*.pak`, `*.mesh.*`). `.gitignore` covers these.
- Project decisions and open questions live in `CLAUDE.md`.
