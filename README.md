# Jetstream — RE Engine Modding Tool

Node-based modding tool for Capcom RE Engine games. First target: Resident Evil 4 (2023).

**Status:** Milestone 1 complete. A node graph (texture → image → edited texture → Fluffy-ready zip)
runs from the desktop app and from the CLI. For using the tool, see
[docs/USER_GUIDE.md](docs/USER_GUIDE.md).

## Requirements

- Windows 10/11
- **Visual Studio Build Tools 2026** (or Visual Studio 2026) with the C++ desktop build tools. That provides:
  - MSVC and the Windows SDK
  - CMake and Ninja
  - vcpkg, which fetches the libraries below automatically
- Git and an internet connection for the first build (vcpkg downloads library sources)

You do not install any libraries by hand. `vcpkg.json` lists them and pins their versions:
toml++, Catch2, Dear ImGui (docking), imgui-node-editor, DirectXTex.

Textures convert with the tool's own converter: nothing else to install. **Noesis** (with the **fmt_RE_MESH**
plugin in `Noesis\plugins\python`) is optional: the Browser's 3D mesh view uses it, and the Pipeline panel's
"Convert textures with Noesis" uses it instead of the built-in converter. Neither is bundled (licensing); you point
the tool at your `Noesis64.exe`.

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

Tests need no game files. The texture round trips run only if you point them at local textures (and, for the
Noesis one, at Noesis). Otherwise they're reported as skipped:

```powershell
$env:REMOD_FIXTURES = "C:\spike"   # folder of original *.tex.143221013 files (never commit these)
$env:REMOD_NOESIS   = "D:\path\to\Noesis64.exe"   # optional: the Noesis converter's tests
ctest --test-dir build --output-on-failure
```

## Outputs

| File | What it is |
|---|---|
| `build\app\remod-app.exe` | Desktop app: node graph editor with a Run button. |
| `build\cli\remod.exe` | Command-line tool: `run` (a saved graph), `tex2png`, `png2tex`, `package`. |
| `build\tests\remod_tests.exe` | Test runner (usually run through `ctest`). |

## Release zip

A self-contained build for people who don't build it themselves: Release, static libraries and C++ runtime, so the
exes need nothing installed. From the Developer PowerShell:

```powershell
cmake --preset release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
cd build-release; cpack
```

This makes `build-release\remod-<version>-win64.zip`: `remod-app.exe`, `remod.exe`, `profiles\`,
`examples\texture_mod.json` and the user guide as `README.md`. The first configure takes a while: vcpkg builds the
libraries again for the static triplet. The version is `project(... VERSION)` in `CMakeLists.txt`; it shows in the
app's title and the CLI's usage.

## Using it

See **[docs/USER_GUIDE.md](docs/USER_GUIDE.md)**: setup, making a mod, every block, the Browser, the CLI and the MCP
server. It's the README in the release zip. Developer notes and decisions are in `CLAUDE.md`.

## Troubleshooting

- **`VCPKG_ROOT` is empty or the toolchain file isn't found:** you're not in the Developer PowerShell. See step 1.
- **Something is badly broken:** delete the `build` folder and run the commands again.
- **Path too long:** keep the repo and output folders near the drive root (e.g. `E:\mods`). Windows'
  260-character path limit is a known problem for RE Engine mods.

## Rules for contributors

- Never commit game files (`natives/`, `*.tex.*`, `*.pak`, `*.mesh.*`). `.gitignore` covers these.
- Project decisions and open questions live in `CLAUDE.md`.
