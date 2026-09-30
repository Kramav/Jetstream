# Jetstream — RE Engine Modding Tool

Node-based modding tool for Capcom RE Engine games. First target: Resident Evil 4 (2023).

**Status:** Milestone 1 in progress. Packaging to a Fluffy-ready zip works; texture conversion
(`.tex` ↔ image) is not implemented yet.

## Requirements

- Windows 10/11
- **Visual Studio Build Tools 2026** (or Visual Studio 2026) with the C++ desktop build tools. That provides:
  - MSVC and the Windows SDK
  - CMake and Ninja
  - vcpkg, which fetches the libraries below automatically
- Git and an internet connection for the first build (vcpkg downloads library sources)

You do not install any libraries by hand. `vcpkg.json` lists them and pins their versions:
toml++, Catch2, Dear ImGui (docking), imgui-node-editor.

## Build

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

Tests need no game files.

## Outputs

| File | What it is |
|---|---|
| `build\app\remod-app.exe` | Desktop app. Currently an empty node-editor canvas. |
| `build\cli\remod.exe` | Command-line tool. Currently packages an already-prepared `.tex` file. |
| `build\tests\remod_tests.exe` | Test runner (usually run through `ctest`). |

### CLI example

Run from the repo root:

```powershell
.\build\cli\remod.exe package --profile profiles\re4r.toml `
  --tex "C:\work\my_texture.tex.143221013" `
  --game-path _chainsaw/ui/ui3200/tex/my_texture.tex.143221013 `
  --name MyMod --out E:\mods `
  --author "Me" --version 1.0 --description "What it does" --screenshot "C:\work\preview.png"
```

This creates `E:\mods\MyMod\` containing `modinfo.ini`, the screenshot and `natives\STM\<game path>`,
plus **`E:\mods\MyMod.zip`** with the `MyMod` folder at its root. Add that zip to Fluffy Mod Manager.
Zipping uses the `tar.exe` built into Windows 10 (1803+) and 11, so nothing extra needs installing.

- **`.\` is required.** PowerShell won't run an exe by relative path without it.
- **Quote any path that contains spaces.**
- **`--game-path`** is the file's path inside the game, relative to `natives/STM`, with no leading slash.
- **Put `--out` outside the repo**, or the output shows up as untracked files in git.
- The tool refuses to overwrite an existing `MyMod` folder or `MyMod.zip`. Delete them to rebuild.

## Troubleshooting

- **`VCPKG_ROOT` is empty or the toolchain file isn't found:** you're not in the Developer PowerShell. See step 1.
- **Something is badly broken:** delete the `build` folder and run the commands again.
- **Path too long:** keep the repo and output folders near the drive root (e.g. `E:\mods`). Windows'
  260-character path limit is a known problem for RE Engine mods.

## Rules for contributors

- Never commit game files (`natives/`, `*.tex.*`, `*.pak`, `*.mesh.*`). `.gitignore` covers these.
- Project decisions and open questions live in `CLAUDE.md`.
