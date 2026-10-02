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
- Node types: LoadTex, ExportImage, **EditImage** (manual), ImportImage, SaveTex, PackageMod; file steps CopyFile,
  MoveFile, RenameFile, DeleteFile, MakeFolder; utilities Value, Text, Split, JoinPath, PathParts, ChangeExtension,
  RequireFile. **Rule (2026-10-01): a block that changes files is a step; one that only computes or checks is a
  utility.**
- **Node types live in `core/nodes.*` (user, 2026-10-01: kept apart for cleanliness).** One function per type returns
  its `NodeSpec`, run code included (`NodeSpec::run`, given a `NodeRun`: inputs, outputs, done/wait/log/warn; core-only
  `core/node_run.hpp`). `core/graph.*` is the engine (editing, validation, file format, `run_graph`) and has no
  per-type run code. A new node type = one function in nodes.cpp plus its entry in `node_specs()`.
- **An output feeds one input (user, 2026-10-01).** Using a value in several places takes an explicit Split block:
  its output (`PortSpec::multiple`) grows a row per link, like a multiple input. A second link from an ordinary output
  is refused with a hint to put a Split on its link; older files get Splits on load. **One Split for every kind (user):**
  its pins are `PortType::Any`, and `Graph::output_type` follows it back to what's linked in, so types are still
  checked through it (a Split carrying a texture can't feed an image input; linking into a Split that would send the
  wrong kind onward is refused).
- **Utilities are small and quiet (user, 2026-10-01): "simple functions" (Split, Text, later path helpers such as Join
  path) must look less significant than the real steps.** `NodeSpec::utility`: a narrow block, a smaller grey title,
  the description only as the title's tooltip, no type name; listed after the main steps in menus ("Utilities"
  submenu on the canvas). CopyFile is a real step, not a utility.
- **File steps (2026-10-01).** Copy, Move and Rename share one helper (`transfer` in nodes.cpp): Overwrite mode, the
  run-time destination-exists log line, same-file and folder refusals, long paths; the editor's "Destination exists"
  covers all three. Move renames, or copies then removes the original across drives; a second run of a Move finds no
  source and says so ("moved by an earlier run?"). Delete goes to the Recycle Bin by default (`SHFileOperationW`,
  Windows asks first only where a drive has no Recycle Bin; under 260 characters only) and never deletes a folder;
  "Missing is fine" is on by default so re-runs pass. **Not tested automatically: the Recycle Bin path** (tests delete
  for good, so they don't fill the user's bin). Steps run in data order only: a Delete or Make folder with typed
  paths has no "after this step" (no exec pins); link something through it if order matters. Join path's "Add" must
  be relative (an absolute one would silently replace the folder) and its output takes the kind it feeds, like Value.
- **Value (user, 2026-10-01): a variable kept in the graph file**, e.g. the output folder, version or author, outside
  the Package block. Any kind, like Split: it becomes the kind of field it feeds (`Graph::wanted_type`, first link,
  through Splits), with that kind's picker (`picker_for`) and colour. One field (an output field, so not linkable);
  several uses go through a Split. Typed paths resolve against the graph's folder, like typed fields.
- **Blocks can be named (user, 2026-10-01: e.g. a Value "Mod Output Folder").** Double-click the title, or Rename...
  in the block's menu; both layouts (a label, not structure). Node param "title" (`block_title`, `set_block_title`);
  the large heading shows it, the type's own title moves to the heading's right, and run messages use it.
- **Edit format = the file extension:** PNG, TGA or JPG (`kEditImageFormats`). Noesis reads and writes each, both ways
  (spike 2026-09-30, header-identical `.tex` from TGA and JPG). BMP is left out: Noesis writes it 24-bit, without
  alpha. Previews from TGA are read by the tool's own TGA reader (WIC has none); a single TGA preview is converted to PNG.
- **Block layout (user, 2026-10-01):** a large title (1.6x font) readable unzoomed, then type and summary small; fixed
  inputs, then outputs; inputs that grow a row per link (`multiple`) always last, at the bottom of the block.
  Descriptions can be hidden ("Descriptions" checkbox above the graph, `Settings::show_descriptions`); the title's
  tooltip shows them then. The type name shows in Build layout only, at the title's right. Linked rows don't name
  their source (the line shows it; the row's tooltip says "From: ..."). Build layout's "Tidy up" arranges blocks in
  columns by step order (core `tidy_layout`).
- **Graph look = design handoff option 1c, "Schematic" (2026-10-01, `docs/design_handoff_node_graph/README.md`),
  kept dark.** Palette (`pal::`, `apply_palette`; user, 2026-10-01: the handoff's mono tokens flipped for dark were
  "horrible", and pins/links must keep a colour per kind): cool dark greys from canvas to raised controls, one blue
  accent for selection, green / amber / red for done / waiting / failed. **Pins and links are coloured by kind**
  (`kind_color`): texture orange, image green, text blue, path yellow, folder teal, an open Split grey; reserved
  script purple, AI call pink. Square corners. Fonts: Segoe UI Semibold titles, Consolas values (Barlow Condensed
  isn't on Windows). Sizes are the handoff's px times `unit()` (font / 11).
  - **Families** (`NodeSpec::family`, core): Source, Transform, Manual, Flow (Split), File, Output, Value (utilities).
    The app paints the outline itself (`paint_block`; the editor's node frame is switched off): notch, hexagon, folded
    corner, stacked sheet, parallelogram, a manual step's double frame and hatched header; `+` corner marks, larger in
    the accent when selected. Pins sit on the outline (slanted edges too, `edge_x`); shape and colour say the kind:
    texture square, image circle, text/path/folder diamond; an unlinked field a small hollow circle; a multiple's empty
    slot a dashed square. Links are routed from the block box's side (`pin_anchor`), then drawn on to the pin (a pin
    inside the box, on a slanted edge or a Far Split dot's centre, would fail the router's clear-first-step check).
    **Far showed no lines (fixed 2026-10-01):** each Far pin was an empty ImGui group, and ImGui sizes an empty group to
    reach the last item drawn before it, so every Far block's box stretched over the graph and every link became a
    portal. Each pin now holds a zero-size item. Found by dumping the router's input from the running app.
  - **View changes run after the blocks are drawn** (fit on load / Tidy up, Far / Near buttons, step list and minimap
    jumps): the editor's content bounds only count blocks drawn this frame, so a fit before them did nothing (the Far
    button never worked before this). They also wait until the view's size has held for a frame: when it changes (dock
    layout settling at startup) the editor restores its previous view, undoing a fit.
  - **Two zooms.** Near (above 65%): full blocks, the minimap bottom-right (click: centre there, select the block hit).
    Far (below 55%, or the Far button): every block a 116x40-unit symbol drawn at two units, title inside, key value
    under it, ports without labels spread down the edges; a Split is a dot. Going Far, blocks first fold
    (`folding`, 0.25 s) then become symbols. Placement and Tidy up use the blocks' Near sizes even while Far, so the
    full layout stays clear. Near button: zoom 100% on the selected block. Deviation: selecting a block does **not**
    re-centre the view (clicking into a field would move it); only the step list and the minimap do.
  - **Use layout progress path:** links in their kind's colour, styled by their source's state (done solid, into a
    waiting step dashes moving, failed red dots, not reached faint dashes); blocks tinted and outlined by state (Far the
    whole block, Near the header band with DONE / YOUR STEP / FAILED), a legend above the graph, and in the Pipeline
    panel a "YOUR STEP" card plus the numbered step list (core `step_order`). Not done: the Build-layout Inspector.
  - **Nodes panel (user, 2026-10-01: blocks packed close together were "unacceptable"):** a collapsible folder per
    family (steps first, Flow and Values last), closed until opened, opened by a search; previews at most 70% with a
    margin around each.
  - **Not checked by eye yet** (2026-10-01): built, tests pass, Build layout at Near started without asserts.
- **Panels per layout (user, 2026-10-01: "we shouldn't show things that won't be in use").** Use layout: Browser,
  Graph, Pipeline, viewer and Textures; the Pipeline can be closed (its X; a "Pipeline" button above the graph brings
  it back). Build layout (user, 2026-10-01: Browser in both layouts, Nodes moved to the bottom): Browser left (the
  Browser window only, no Textures or viewer), Graph, the Pipeline's building parts only (graph file, game, status,
  problems; no Noesis, game files or log), always shown, and along the bottom a **Nodes** panel (every block type in
  family folders, search; each entry is a preview of the block as it will look, `draw_block_preview`, side by side and
  wrapping; click adds mid-view; dragging one over the graph shows a see-through copy at the graph's zoom, already
  spaced from the others as `keep_apart` will place it, and the drop lands exactly there; tooltips show description
  and pins). The dock layout is rebuilt when the layout or the Pipeline's visibility changes, so hidden panels leave
  no gap (split sizes reset then).
- **Drag paths from the Browser onto block fields (user, 2026-10-01), both layouts.** Files, folders and thumbnails
  are drag sources (payload `"remod_path"`, the absolute path); every path-capable field in a Near block is a target
  (`accept_path`). Core `path_fit` decides: folder fields take folders, file fields files with an extension from the
  field's filter, text fields no path; a misfit shows why on hover and the drop does nothing. A texture drop picks the
  game, like the "..." picker. Dropping only fills fields: no new blocks (see §10).
- **Zooming while holding a block keeps it under the cursor (user, 2026-10-01).** imgui-node-editor drags by
  mouse − click in graph coordinates but re-converts the screen click through the current zoom each frame, so a zoom
  moved the block. The app pins the click's graph position just before `ed::End()` (after all Suspend/Resume, which
  re-convert); no library patch.
- **Outputs are on the right.** A field for where a node writes its output (Export's PNG file) is part of that
  output (`PortSpec::field`), shown on the output side and not linkable. **Destinations too, on every block (user, 2026-10-01):** an input
  that says where a node writes (`InputSpec::result`: Copy/Move destination, Rename's new name, Make folder, Package's
  output folder) shares a row with the output it produces, on the output side, with ONE circle. By default it's on
  the right: the field is typed and the circle passes the result on (Package: the built mod's .zip, output "mod").
  The row's `<>` button (Build layout) flips it (`Graph::flip`, node param "flip:<input>"): circle on the left, a
  link sets the field (e.g. a stored Value), and the result isn't offered; flipping drops the links of the side that
  goes away. Export's "Image file" doesn't flip: its result is what the rest of the pipeline needs. **Flipping never
  moves the row (user): only the circle changes side**, so a flipped row is drawn at its output's place.
- **Manual editing is its own step (EditImage, `NodeSpec::manual`).** A run waits there until the user marks it
  done (`done` state param; the CLI's `--edited true`). A freshly re-exported PNG voids an earlier "done"
  (`RunResult::reset_edits`, applied by `apply_run`).
- **App modes:** Build layout (structure editing: add/insert/duplicate/delete/link, via core helpers
  `choices_for_pin`, `add_connected`, `choices_for_link`, `insert_node`, `duplicate_node`, `disconnect_node`)
  and Use layout (structure and block positions locked; fill in, run, edit). The switch sits centred above the
  graph. The mode is UI state, remembered in settings.
- **Links are routed by core** (`core/route.*`, `route_links`): right angles with rounded corners, around blocks,
  leaving outputs rightwards and entering inputs from the left; links avoid sharing a line, running right beside one
  (closer than 0.6 gap) and turning on each other. (route_links can still make links of one "net" share a trunk that
  branches, with junction dots; since Splits (2026-10-01) the app gives every link its own net, so nothing branches.) Two lanes around each block; spare midlines only where there's room.
  Measured 2026-10-01 on 143 generated layouts (728 links), old → new: lines beside another 115 → 6, shared lines
  5 → 0, touching corners 43 → 0. Crossings cost a little (PCB-like traces) and each link is rerouted against all
  the others (rip-up and reroute, 3 passes): crossings 585 → 424, the user's Export / Convert crossing gone. The
  app routes one quick pass while blocks move (Debug 7 ms for the example graph) and all passes once they stop
  (19 ms). Lines keep 0.9 gap (the gap = 0.8 font, a pin's stub length) from every block except at their own pins;
  only where blocks stand closer than that does a link squeeze to half the gap. The app draws them itself on the
  editor's top layer
  (imgui-node-editor only draws beziers) and hit-tests them with `hit_link` for the right-click menu.
- **Clean routes only, "like an efficiently routed PCB" (user, 2026-10-01, after a line wrapped a whole block and read
  as its border).** In core: the lane right beside a block's side (within 1.5 gaps) costs 2x, so lines take a lane a
  little away when there is one; a route that lines two sides of one block for over half their length, or runs over
  2.5x the direct distance + 10 gaps, is not clean: it tries the squeezed lanes (half the gap) once, else the link is
  a **portal pair** (`Routes::portals`): a stub at each pin ending in a numbered tag, the same number at both ends,
  hover shows a dashed line and From/To. 142 generated links: 0 portals. In the app: blocks let go closer than three
  lanes (2.4 font) to another move the least distance out (`keep_apart`); a block added onto a link or a pin
  (insert, add connected, duplicate, Use in graph) goes beside what it's linked to and pushes the blocks after it right
  (`make_room`); Tidy up spaces rows by the same gap; old files that gained blocks on load are tidied
  (`load_graph(..., &added_blocks)`); the line above the graph counts links without a clean route.
- **Every run reports each node's state** (`RunResult::nodes`, or `RunError::nodes` on failure): done, waiting
  for the user, failed with its reason, or not reached. The app shows these as block fills and link styles (above).
- **Every input has a pin** (`InputSpec`). Editable inputs can be typed or linked; a link wins.
  `multiple` inputs take any number of links, e.g. PackageMod's textures and previews.
- **Link kinds (by what flows, not by file extension).** Shown by pin shape (see the graph look above). Rules
  (`accepts`):
  same kind; anything into a Text or Path input (a texture is a file too); Text into any editable field; a Path into a
  texture or image file field; a Folder field takes text or a folder, not a file path.
- The Text node fills `{1}`, `{2}`… from its linked parts.
- CopyFile (2026-10-01, first of the file nodes): source → file or folder, Overwrite mode (`Widget::Choice`: fail /
  overwrite / skip), Create folders. Pasted quotes stripped, paths past 260 characters via `\\?\` (`long_path`).
  The editor shows "Destination exists" from core `destination_warnings` (rechecked on input change and every 1.5 s;
  no warning when a link decides the name); the run logs what it did with an existing destination.
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
- [ ] **Waiting (user, 2026-09-30): parked until it causes a real problem; no in-game test planned.**
      SaveTex no longer refuses a different mip count: the run succeeds with a warning (`RunResult::warnings`,
      a popup in the app, `WARNING:` in the CLI). Checked: a 1-mip 256x256 UI texture converts to 6 mips.
      **Mip count mismatch (used to block most UI textures).** The plugin's writer always generates mips down to 8x8
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

- **Far future (user, 2026-10-01): drop a Browser path on empty canvas to add a block holding it** (.tex → Original
  texture, image → Use existing image, anything else → Value), as an option in an options menu. Not started; today
  a drop only fills a field.

- M2: AI backend as a separate local process (not in the app). GPU/VRAM detection at install.
  img2img/ControlNet for textures, never plain text-to-image for UV-mapped textures.
- M3: REFramework Lua runtime reading manifests (triggers → actions).
- M4: C++ REFramework plugin for video playback (in-game "cutscene" videos).
- Replace ImGui front end with a polished native UI.
- **Texture and mesh browser: first version built (2026-10-01).** Use layout only. Layout: Browser left (folder tree, search,
  file paths), Graph middle, Pipeline right; along the bottom the viewer (left corner: the selected texture, or the
  picked mesh in 3D until closed; the last mesh stays) and Textures (thumbnails of the picked mesh's or the folder's
  textures, "Use in graph" via `texture_target`).
  - Core: `core/browse.*` (index, tree, search, `mesh_textures`, `preview_file`) and `read_tex_pixels`. The app
    uploads the stored mip to D3D11 as is: RE Engine's format numbers are DXGI's, so the GPU decodes BC1-7 itself.
    sRGB formats are shown as their UNORM twins (the back buffer is UNORM).
  - Measured on RE4R (Debug): index 3.4 s for 20,603 textures + 6,051 meshes (streaming/ skipped); thumbnails
    ~0.5 ms each; 20,602 of 20,603 readable (one debug texture holds no images).
  - A mesh's material is `<mesh>.mdf2.*`, `_mat`, `_00` (fmt_RE_MESH's guesses), else the folder's first `.mdf2`:
    5,972 of 6,051 meshes have one. Read with the plugin's mdf2 layout (`read_mdf2`, versions > 3): material
    names and texture paths, checked on all 6,392 RE4R materials. A material's colour texture is the plugin's rule:
    file name with `_alb`, `_albd` preferred (20,359 of 21,972 materials have one).
  - The base texture is often a small standalone copy (e.g. 512x512, 2 mips) of a `streaming/` one (2048x2048).
    The preview shows the streaming copy; the open streaming question (§9) still decides what a mod must replace.
  - **3D view** (in the Browser's details, mesh selected; app/mesh_view): Noesis exports the mesh to OBJ
    (`NoesisConverter::load_mesh`, `parse_obj`), the app draws it with D3D11 into a texture, each part with its
    material's colour texture (OBJ `usemtl` = mdf2 material name). Clicking a texture dims the parts not using it.
    Spike 2026-10-01: `?cmode <mesh> out.obj -b -noprompt` runs headless. Without `-b` the plugin's import window
    waits forever (not a dialog, so only the timeout catches it). 0.3-2.3 s for most meshes, 5.9 s / 20 MB OBJ for
    a 163k-triangle character; 24 of 26 sampled meshes load (2 hit plugin Python errors, reported). Every sampled
    part name matched a material. Highest LOD only; the main material file only (costume variants such as
    `cha000_00b.mdf2` aren't offered yet).
    Mesh groups: the plugin names OBJ groups `LOD_1_Group_<id>_...` [plugin source]; parts are kept per (group,
    material) and the view has a checkbox per group (Leon's `cha000_00`: 0 shirt and gloves, 1 pants, 2 bare
    forearms, 3-4 weapons). Which groups the game shows isn't in the mesh (not read), so all start shown.
    Noesis's OBJ writes texture v top-down already (D3D style); flipping it again mirrored every texture and put
    Leon's shirt on the skin above it in his texture sheet (fixed 2026-10-01).
    Materials follow the plugin's Noesis rules (`mesh_textures`, [plugin source]): colour texture × `BaseColor`
    parameter (solid `BaseColor` without a texture); no `_alb` texture falls back to the first `Base…Map`; alpha test
    at 0.05 only for `_hair`/`_decal`/`_dirt` master materials (ALBA alpha, ATOS/ATOC red, or an `AlphaMap`); eye
    shells without textures, tear lines, lenses and "destroy" parts aren't drawn. Layout checked on all 6,392 RE4R
    material files (22,113 materials, 20,002 with `BaseColor`, 5,544 cut-out). Not mirrored: Noesis's normal maps,
    specular and lighting.
- **Readable names (user wish, long term, 2026-10-01):** let the user give the game's cryptic folder and file names
  (e.g. `cha000`, `sm84_676_00`, `ui3200`) a human-readable nickname, shown in the Browser in place of or beside
  the real name, and searchable. Nicknames only label things: real paths stay what's packaged and stored.
  **Built 2026-10-01** (core `names.*`, `Nicknames`): one JSON file per game, `%APPDATA%\remod\names\<game>.json`, a
  flat `{"path": "nickname"}` object keyed by the lowercase path under the natives root (folders and files), so a
  list can be shared by copying the file. A file that can't be read is reported and never saved over. Browser:
  right-click a folder, file or thumbnail → Name...; the nickname shows beside the real name (thumbnails: in place of
  it), search results show the nearest named folder's, and `search` matches the nicknames of a path and every folder
  above it ("leon albd"). The game is the graph's profile. Open: a shipped community list (not built; the format
  allows one).

## 11. References

- Fluffy packaging guide (loose files vs PAK, modinfo.ini) [guide]: https://www.patreon.com/posts/113314784
- REtool [guide]: https://www.patreon.com/FluffyQuack/posts/retool-modding-36746173
- Mod iteration workflows [guide]: https://www.patreon.com/posts/45460253
- Noesis manual (command-line mode) [official]: https://richwhitehouse.com/noesis/nms/index.php?content=userman
- fmt_RE_MESH plugin (fork checked) [guide]: https://github.com/SilverEzredes/fmt_RE_MESH-Noesis-Plugin_SILVER
- DirectXTex license [official]: https://github.com/microsoft/DirectXTex/blob/main/LICENSE
- REFramework [official]: https://github.com/praydog/reframework
- imgui-node-editor [official]: https://github.com/thedmd/imgui-node-editor
