# CLAUDE.md — RE Engine Modding Tool

Node-based modding tool for Capcom RE Engine games. First target: **Resident Evil 4 (2023), "RE4R"**.
Long-term: AI-assisted asset generation plus optional REFramework runtime features.
**Current scope: Milestone 1 (M1) only. No AI, no REFramework work.**

Source labels used below: **[official]** = official/authoritative docs, **[guide]** = community guide,
**[inferred]** = design decision or assumption from planning, **[TBD-spike]** = must be confirmed by a manual in-game test.

---

## 1. Hard rules

- Windows-first. Native desktop executable. **No web UI, no browser, no webview, no Python UI.**
- **The UI contains no logic.** All behavior lives in `core/`. The ImGui front end is expected to be
  replaced by a more polished native UI later; `core/` must not depend on ImGui or any UI library.
- Never commit game files. Gitignore at minimum: `natives/`, `*.tex.*`, `*.pak`, `*.mesh.*`, extraction caches.
- Never vendor third-party tools whose license does not allow redistribution (see §6).
- Do not add AI features during M1.
- Do not guess RE Engine format details. Unknowns go in §9 and are marked `[TBD-spike]`.
- Keep disk writes low: extract game PAKs **once** into a reusable cache; never re-extract per run.
  Keep log verbosity low by default.
- Keep working paths short (Windows 260-char path limit is a known problem for RE mods) [guide].

## 2. Stack [inferred — decided in planning]

- C++20, CMake, MSVC, vcpkg (manifest mode) for dependencies.
- UI (M1): Dear ImGui (docking branch) + imgui-node-editor (thedmd).
  - Pin both versions. Known docking-branch interaction bug, reportedly fixed:
    https://github.com/thedmd/imgui-node-editor/issues/230 [official]
- C++20, not 23: MSVC 14.50 has no stable `/std:c++23` (only preview `/std:c++latest`). Core reports
  errors with exceptions (no `std::expected` in C++20).
- Libraries, verified 2026-09-30 (maintained, permissive license, in vcpkg). Versions pinned via
  `vcpkg.json` builtin-baseline + overrides:
  - **Adopted:** toml++ 3.4.0 (MIT, profiles), Catch2 3.16.0 (BSL-1.0, tests),
    Dear ImGui 1.92.9 docking (MIT), imgui-node-editor 0.9.3#4 (MIT; upstream lightly maintained,
    last tag Oct 2023, vcpkg patches it for ImGui 1.92). Issue #230's fix (#205, commit 3fdb8e3) is in v0.9.3.
  - **Adopted for the graph file:** nlohmann/json 3.12.0#2 (MIT), core-private.
  - **Adopted for the app's file pickers:** nativefiledialog-extended 1.4.0 (Zlib), app-only (never in core).
  - GoogleTest verified but not used.
- Images (combining preview PNGs): **WIC**, built into Windows, used from core. No image library dependency.
- Build (Developer PowerShell for VS 2026, which sets `VCPKG_ROOT`):
  `cmake --preset default` → `cmake --build build` → `ctest --test-dir build --output-on-failure`
- Long-term polished UI: undecided. Keep the core-to-UI boundary clean so it can be swapped.

## 3. Architecture

```
core/        Graph engine, node types, game profiles, tool wrappers, packaging. No UI deps.
cli/         Runs a saved graph headlessly. Used for tests and automation.
app/         ImGui front end. Thin layer over core/.
profiles/    Per-game data files (TOML). Adding a game = adding a profile, not code.
schemas/     Manifest + graph file schemas (versioned).
runtime/     (Later) REFramework Lua core + optional C++ plugin. Empty in M1.
tests/
docs/
```

### Output tiers
1. **Asset-only** — file replacements packaged for Fluffy Mod Manager. No REFramework. ← M1
2. **Scripted** — adds Lua; requires REFramework. (Later)
3. **Native** — adds C++ REFramework plugin (e.g. video playback). (Later)

The packager selects the lowest tier the graph needs and only declares an REFramework dependency for tier 2/3.

### Runtime design (later milestones, recorded now so M1 doesn't block it)
One generic runtime is shipped once; each mod ships **data only** (manifest + assets), not code.

## 4. Milestone 1 — definition of done

A streamlined pipeline, no AI:

1. **LoadTex** — game `.tex` → image + `TexMeta` (via Noesis).
2. **ExportImage / ImportImage** — write a PNG for external editing; read the edited PNG back.
3. **SaveTex** — image + `TexMeta` → `.tex` matching the original's dimensions/format (via Noesis;
   texconv fallback only if Noesis can't encode).
4. **PackageMod** — write `ModName/natives/STM/...` + `modinfo.ini` + screenshot → archive.

Implementation note: Noesis converts `.tex` ↔ PNG file-to-file, so LoadTex writes the PNG directly and
ExportImage/ImportImage reduce to handing that PNG to the user and checking the edited one (same size).
Pixels never pass through the tool. The CLI exposes the steps as `tex2png`, `png2tex` and `package`.

Graph (`core/graph.*`, file format `schemas/graph.v0.example.json`):
- Node types: LoadTex, ExportImage, **EditImage** (manual), ImportImage, SaveTex, PackageMod, Text.
- **Outputs are on the right.** A field for where a node writes its output (Export's PNG file) is part of that
  output (`PortSpec::field`), shown on the output side and not linkable.
- **Manual editing is its own step (EditImage, `NodeSpec::manual`).** A run waits there until the user marks it
  done (`done` state param; the CLI's `--edited true`). A freshly re-exported PNG voids an earlier "done"
  (`RunResult::reset_edits`, applied by `apply_run`).
- **App modes:** Build layout (structure editing: add/insert/duplicate/delete/link, via core helpers
  `choices_for_pin`, `add_connected`, `choices_for_link`, `insert_node`, `duplicate_node`, `disconnect_node`)
  and Use layout (structure locked; fill in, run, edit). The mode is UI state, remembered in settings.
- **Every run reports each node's state** (`RunResult::nodes`, or `RunError::nodes` on failure): done, waiting
  for the user, failed with its reason, or not reached. The app shows these as coloured node borders and badges.
- **Every input has a pin** (`InputSpec`). Editable inputs can be typed or linked; a link wins.
  `multiple` inputs take any number of links, e.g. PackageMod's textures and previews.
- Link types are Tex, Image and Text. Any output can go into a Text input; a Text output can go into any
  editable input.
- The Text node fills `{1}`, `{2}`… from its linked parts.
- ExportImage never overwrites an existing PNG (the user's edit). One run exports every PNG that needs editing
  and skips only what depends on a waiting EditImage. So a mod is: Run, edit, Done editing, Run.
- PackageMod packages every linked texture. Several previews are tiled into one `preview.png`.
  "Replace existing" overwrites only a previous build of the same mod.
- Old graph files are migrated on load: PackageMod's typed `screenshot` becomes ImportImage → preview.
- LoadTex infers the game path when the `.tex` sits inside a `natives/STM/...` tree; otherwise set `game_path`.
- Intermediate `.tex` files go to a per-run temp folder, deleted afterwards.
- Runs from `remod run --graph <file> --noesis <exe>` and from the app's Run button.

**Status: M1 complete (2026-09-30).** All three criteria below are met; the user confirmed the tool-built mod
works end to end.

Done when:
- The graph runs from both the app and the CLI.
- One RE4R loading-screen/UI texture mod built by the tool loads correctly in game via Fluffy.
- Unit tests pass (§8).

First target is a 2D UI/loading-screen texture (no UV concerns). Character albedo textures come after M1.

## 5. Data contracts [inferred — v0, may evolve]

### TexMeta
```
source_path     original .tex path (relative to natives root)
game_profile    e.g. "re4r"
width, height
format          e.g. BC7 (read from the original, never assumed)
mip_count
array_count
```
SaveTex must reproduce the original's width/height/format/mips.

Texture identity comes from **content, not names**: the header's version number (e.g. 143221013) equals a
profile's `tex_suffix`, which identifies the game (`profile_for_texture`). Files may be named `.tex` or
`.tex.<version>`. LoadTex feeds Noesis a temp copy named `src.tex.<version>`, and packaging appends the suffix
to a plain `.tex` game path.

### Manifest v0
```json
{
  "schema_version": 0,
  "runtime_version": null,
  "game": "re4r",
  "tier": 1,
  "requires_reframework": false,
  "assets": [],
  "triggers": []
}
```
`triggers` stays empty until tier 2. `runtime_version` is null for tier 1.

### Game profile (profiles/re4r.toml)
```toml
[game]
id              = "re4r"
name            = "Resident Evil 4 (2023)"
tex_suffix      = "143221013"                 # [guide] RE4R texture version suffix
natives_root    = "natives/STM"               # [guide]
packaging       = ["loose_archive", "pak"]    # [TBD-spike] which Fluffy accepts for RE4R
pak_script      = "Create-PAK-2023.bat"       # [guide] REtool script for 2023-or-older games
noesis_export   = "TBD"                       # [TBD-spike] ?cmode option selecting RE4R tex output
file_list       = "TBD"                       # [TBD-spike] PAK file list source/version
```

## 6. External dependencies — "managed dependencies"

Bundle only what licensing allows; install or detect the rest on first run, with user consent.

| Tool | Role | License status | Handling |
|---|---|---|---|
| Noesis | TEX ↔ image conversion | Freeware, no redistribution terms found | Don't bundle. Offer `winget install -e --id RichWhitehouse.Noesis`, or detect existing install |
| fmt_RE_MESH Noesis plugin | RE Engine format support | Fork checked had **no license file** (all rights reserved by default); original repo not checked | Don't bundle. Download from repo at setup with consent, into `Noesis/plugins/python` |
| REtool | PAK extraction / optional PAK creation | No license found | Don't bundle. Detect path; guide manual install |
| Fluffy Mod Manager | Install/test mods | No license found | Don't bundle. User selects path |
| texconv (DirectXTex) | Fallback DDS encoding | MIT [official] | May bundle, include license notice |
| tar.exe (bsdtar) | Mod `.zip` creation | Ships with Windows 10 1803+ | Not bundled; run from System32 via `run_process` |
| REFramework | Tiers 2/3 only | Not checked | Not needed in M1. Detect via `dinput8.dll` in game dir [guide] |

Plugin note [official, plugin README]: it stores the extracted `re_chunk_000.pak` base directory per game
in a txt file next to the plugin — the tool must set/verify this.

## 7. Tool wrapper requirements [inferred]

- All external tools run as subprocesses with timeouts, captured stdout/stderr, and exit-code checks.
- Noesis command-line mode: `Noesis.exe ?cmode infile outfile -options` [official].
- Never assume a conversion succeeded: verify output exists and TexMeta matches.

## 8. Testing (no game required)

- Path mapping from game-relative paths to package paths.
- `modinfo.ini` contents and package folder layout.
- Profile loading/validation.
- Round-trip: original TEX → image → TEX reproduces the original's TexMeta and image data.
  Fixtures are **local only**, located via env var `REMOD_FIXTURES`. Tests skip if unset.
  Never commit fixture game files.

## 9. Open questions / spike results  [TBD-spike]

Fill these in from the manual spike before implementing the affected code:

- [x] Does fmt_RE_MESH's RE4R tex export work under Noesis `?cmode`? Exact options? **Yes, `-b`**
      (now `noesis_export = "-b"` in `profiles/re4r.toml`). Implemented as `NoesisConverter` in
      `core/texture_converter.cpp`; the round-trip test passes against the spike textures.
      Noesis behavior the wrapper relies on (observed 2026-09-30):
      - Its console text is **not** written to stdout/stderr (0 bytes when redirected).
        **`-logfile <path>` does capture it** (ReadMe: "direct all output for the given module to a file").
        The wrapper passes it on every call and puts the log in the error when a conversion fails.
      - It **exits 0 even when a conversion fails** (a junk input gave exit 0 and no output; a missing input gave exit 1).
        Success is therefore judged from the output file only: it must exist and its PNG size or `.tex` header
        must match the original.
      - **Noesis's embedded Python needs `PYTHONIOENCODING` when its output is piped.** Without it (e.g. the app
        started from Explorer), the Python plugins silently don't load and every RE texture is
        `Detected file type: Unknown`. With `PYTHONIOENCODING=utf-8` they load. Reproduced 2026-09-30 (system ACP 1252);
        the exact failure inside Python 3.2 is unknown. The wrapper sets it for Noesis only.
      - **A plugin (Python) error opens a MessageBox and waits forever**, even in `?cmode` (ReadMe; seen with a
        header-only `.tex`). The wrapper runs Noesis on a private invisible desktop, so the dialog never reaches the
        user's screen. It watches that desktop, and on a dialog kills Noesis at once and reports the dialog text.
      **Import confirmed (spike 2026-09-30, Noesis v4474 `Noesis64.exe`, fmt_RE_MESH already installed):**
      `Noesis64.exe ?cmode <in>.tex.143221013 <out>.png` works headless (no dialog), prints
      `Detected file type: RE Engine Texture [PC]` and `BC7_UNORM_SRGB 8` (= format name + bits per pixel,
      per the plugin's `print(formatName, bpp)`), and writes a 1024x1024 RGBA PNG.
      **Export confirmed headless (spike 2026-09-30):**
      `Noesis64.exe ?cmode C:\spike\edit.png C:\spike\srcout.tex.143221013 -b`
      - Prints `-b parameter accepted.` No dialog.
      - Uses `src.tex.143221013` (same folder) as its template.
      - The plugin *injects* into that template: it copies its header and takes the target format from it.
        So SaveTex needs the original `.tex` present.
      - With `-b`, the template is found from the output name: strip `out.` and the extensions, then add
        `.tex.<version>`. So name the output `<X>out.tex.143221013` next to `<X>.tex.143221013`.
      - Without `-b` it opens a "Choose a tex file to inject" dialog, which hangs a headless run.
      - Multi-image (array) textures open a dialog even with `-b` [plugin source]: not supported headless.
      - A PNG input is always re-encoded. Mips are regenerated down to 8x8 (the loop stops once both sides
        are ≤4), not copied from the template.
      - Result for this texture: output header matches the template exactly (1024x1024, type 99 = BC7_UNORM_SRGB,
        8 mips, 1 image, 1,398,248 bytes). Header fields were read using the plugin reader's layout:
        magic@0, version@4, width@8, height@10, images@14, mipHeaderSize@15 (/16 = mips), format@16
        [plugin source]. A template with a different mip count would not be matched; SaveTex must verify this.
      - Template `.tex` came from another mod, not the extracted game files.
      Also: this Noesis folder already has `plugins/python/RE4NativesPath.txt` (the §6 base-dir file).
- [x] Does Noesis encode BC7 itself? **Yes** (spike 2026-09-30): the export above encoded BC7 with no texconv.
      texconv is not needed for BC7.
- [ ] Target texture path used for the first mod: `natives/STM/...`
      Packaging test (2026-09-30) used `_chainsaw/ui/ui3200/tex/cs_ui3210_questfile_main_002_02_iam.tex.143221013`
      (a `.tex` taken from an existing mod, not converted by us). Not a good in-game test target;
      the first real target is still to be chosen.
- [ ] Does Fluffy accept a loose-file archive for RE4R, or is a REtool-built PAK needed?
      **Partial (spike 2026-09-30):** Fluffy *installs* a loose-file archive built by `remod package`.
      **Confirmed working end to end with a tool-built mod (user, 2026-09-30).** Loose-file .zip is enough for
      RE4R; no REtool PAK needed.
- [ ] PAK file list source for the current game version.
- [x] RE4R game version the spike was done on: Steam App ID 2050650, **Build ID 22377325** (spike 2026-09-30).
- [ ] Does the RE4R `tex_suffix` (143221013) change across game updates?
- [x] Archive format and layout (spike 2026-09-30): Fluffy accepts `.zip` and `.7z`. Published mods put the
      mod folder at the archive root (`ModName/modinfo.ini`, `ModName/natives/...`), and the tool does the same.
      (The spike's archive had the contents at the root instead and Fluffy still installed it.)
- [ ] Text encoding Fluffy expects in `modinfo.ini` for non-ASCII text (ASCII vs UTF-8).
      **Confirmed (spike 2026-09-30):** a `modinfo.ini` written by `remod package` (flat lowercase `key=value`,
      CRLF line endings, ASCII-only values) shows name/version/description/author/screenshot correctly in Fluffy.
- [ ] **Mip count mismatch (blocks most UI textures).** The plugin's writer always generates mips down to 8x8
      [plugin source]. Found 2026-09-30 in the REtool extraction: 477 of 493 RE4R UI textures have 1 mip, and
      others stop above 8x8 (e.g. 256x256 with 5 mips). SaveTex refuses any mip mismatch, so only textures whose
      chain ends at 8x8 convert today. Open: does the game accept a texture with *more* mips than the original?
      If not, the extra mips must be dropped after export (header + mip table rewrite per the plugin's layout),
      which needs an in-game check before relying on it.
- [ ] **Padded width on export.** `cs_ui3200_stamp_im` (468x440) exports as a **512x440** PNG: the plugin decodes
      the stored (pitch-padded) rows. Converting back would write a 512-wide texture. Open: crop to 468 on export
      and pad on import, or does the game need the padded layout? Needs a spike.
- [ ] **Streaming textures.** 17,728 of 38,331 RE4R textures have a high-resolution copy under
      `natives/STM/streaming/<same path>`. Does replacing a texture require replacing its streaming copy too?
      LoadTex logs a note when one exists.
- [ ] Screenshot size/aspect requirements for Fluffy, if any (formats: jpg/png/tga/bmp [guide]). The tool
      combines several previews into one 512-px-per-tile grid PNG.
- [ ] Should a tier-1 package include `manifest.json`, or only tier 2/3?

## 10. Later milestones (do not start)

- M2: AI backend as a separate local process (not in the app). GPU/VRAM detection at install.
  img2img/ControlNet for textures, never plain text-to-image for UV-mapped textures.
- M3: REFramework Lua runtime reading manifests (triggers → actions).
- M4: C++ REFramework plugin for video playback (in-game "cutscene" videos).
- Replace ImGui front end with a polished native UI.
- **Texture browser (user priority, long term):** Noesis-style browsing of the REtool extraction (38k textures
  for RE4R): folder tree, search, thumbnails and preview, then pick into a graph. For now the texture picker just
  opens in the REtool folder.

## 11. References

- Fluffy packaging guide (loose files vs PAK, modinfo.ini) [guide]: https://www.patreon.com/posts/113314784
- REtool [guide]: https://www.patreon.com/FluffyQuack/posts/retool-modding-36746173
- Mod iteration workflows [guide]: https://www.patreon.com/posts/45460253
- Noesis manual (command-line mode) [official]: https://richwhitehouse.com/noesis/nms/index.php?content=userman
- fmt_RE_MESH plugin (fork checked) [guide]: https://github.com/SilverEzredes/fmt_RE_MESH-Noesis-Plugin_SILVER
- DirectXTex license [official]: https://github.com/microsoft/DirectXTex/blob/main/LICENSE
- REFramework [official]: https://github.com/praydog/reframework
- imgui-node-editor [official]: https://github.com/thedmd/imgui-node-editor
