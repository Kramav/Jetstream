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
  - **Adopted for texture pixels (user, 2026-10-01; encoding 2026-10-02):** DirectXTex 2026-05-07 (MIT),
    core-private (`default-features: false`, feature `dx11`). It works only on pixel data: our own code reads and
    writes the `.tex` files (`read_tex_pixels`, `NativeConverter`). It decodes (`decode_tex`), makes mips and
    encodes (`NativeConverter::save_tex`). Uses its own converter, not WIC's (`TEX_FILTER_FORCE_NON_WIC`), so any
    thread can call it. BC6H / BC7 encode on the GPU (DirectCompute, a D3D11 device per conversion), falling back
    to the CPU when there's no GPU: CPU BC7 took 188 s for one 512x512 in the Debug build, the GPU 0.6 s for a
    1024x1024 (load and save).
  - GoogleTest verified but not used.
- Images (combining preview PNGs): **WIC**, built into Windows, used from core. No image library dependency.
- Build (Developer PowerShell for VS 2026, which sets `VCPKG_ROOT`):
  `cmake --preset default` → `cmake --build build` → `ctest --test-dir build --output-on-failure`
- **Release zip (2026-10-03):** preset `release` (`build-release`, triplet `x64-windows-static`, static CRT), then
  `cpack` in it → `remod-<version>-win64.zip`: both exes, `vcomp140.dll` (DirectXTex's OpenMP has no static form;
  shipped app-local), `profiles\`, `examples\texture_mod.json` (= `schemas/graph.v0.example.json`; the other example
  graphs hold personal paths, not shipped), `docs/USER_GUIDE.md` as `README.md`. Version: `project(VERSION)`, shown
  in the window title and CLI usage. Checked: Release tests pass (fixtures too), no warnings, the unzipped CLI runs
  tex2png → png2tex → package with only System32 on PATH, the unzipped app starts. Not checked: a PC without
  Visual Studio.
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

1. **LoadTex** — game `.tex` → image + `TexMeta`.
2. **ExportImage / ImportImage** — write a PNG for external editing; read the edited PNG back.
3. **SaveTex** — image + `TexMeta` → `.tex` matching the original's dimensions/format.
4. **PackageMod** — write `ModName/natives/STM/...` + `modinfo.ini` + screenshot → archive.

Implementation note: a converter (`ITextureConverter`) turns `.tex` ↔ PNG/TGA/JPG file to file, so LoadTex writes the
image directly and ExportImage/ImportImage reduce to handing that file to the user and checking the edited one (same
size). The CLI exposes the steps as `tex2png`, `png2tex` and `package`.

**Noesis is optional (user, 2026-10-02: no dependency on, or exposure to, Rich Whitehouse or the plugin's
authors).** `make_converter` picks one:
- **Built in (default), `NativeConverter`.**
  - Load: `decode_tex` at full size, written by `save_image_bgra` (PNG / JPG via WIC, TGA by our own writer).
  - Save: the edit gets the original's mip count (`GenerateMipMaps`) and format (DirectXTex), then is written
    **over a copy of the original, each mip at its table entry's pitch and size** (padding zeroed). So the header,
    flags, mip table and file length stay the original's, by construction.
  - Checked on 7 real RE4R textures (BC7 and BC7 sRGB, BC4, the 468-wide padded `stamp_im`, 1-mip UI frames): the
    header and mip table come out byte-identical, and the mean pixel difference after a round trip is 0.1-1.5 of 255.
  - Not checked in game yet.
- **Noesis (optional), `NoesisConverter`.** The app's "Convert textures with Noesis" (`Settings::noesis_textures`);
  `--noesis` in the CLI; `noesis` in the API. Its behaviour is in §9.
- **The run cache** keys each texture on `ITextureConverter::id`, so switching converters converts again.
- **The 3D view reads meshes natively too** (2026-10-02, `read_mesh`, §10 browser). Noesis is its fallback only for
  what that can't read.

Graph (`core/graph.*`, file format `schemas/graph.v0.example.json`):
- Node types: LoadTex, ExportImage, **EditImage** (manual), ImportImage, SaveTex, AdjustColour, ResizeImage,
  OverlayImage, ReplacePhoto, Preview, PackageMod; file steps CopyFile,
  MoveFile, RenameFile, DeleteFile, MakeFolder; utilities Value, Text, Split, JoinPath, PathParts, ChangeExtension,
  CutText, RequireFile. **Rule (2026-10-01): a block that changes files is a step; one that only computes or checks is a
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
  (`RunResult::reset_edits`, applied by `apply_run`). **Open with (user, 2026-10-01; they use GIMP):** an optional
  field `editor` (a program, `.exe`, picker opens in Program Files) that "Open in editor" uses (block and the YOUR
  STEP card); empty: Windows' editor for the file type, as before. It can be linked from a Value (one for every Edit
  image block); core `known_value` reads a field's value before a run (typed, or a Value's through Splits).
  Starting the program is the app's (`open_in_editor`, ShellExecute); a failure goes to the status line.
- **Undo / redo (user, 2026-10-01):** core `History` keeps snapshots of the whole graph (blocks, links, values,
  positions; `Graph::operator==`, up to 200 steps). The app syncs block positions into the graph every frame and
  calls `track` once the graph is settled (nothing active or dragged, no block still being placed), so every kind of
  edit is one step with no per-edit code. Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z (a text box being typed in keeps its own),
  and Undo / Redo buttons in the Pipeline panel; an undo moves blocks back without refitting the view.
- **Unsaved changes (user, 2026-10-01):** the graph differs from the one last loaded or saved (`saved_graph`, taken
  a frame after a load or New, once the editor has placed and rounded positions). `*` in the window title and on
  Save; Ctrl+S saves; New, Load and closing the window (WM_CLOSE, also from the taskbar while minimized) ask Save /
  Don't save / Cancel. View changes (fit, Far / Near) run after the blocks are drawn.
- **Fields show their values (user, 2026-10-01):** hovering a filled field shows the whole value (e.g. a long path)
  instead of the hint; a linked field shows what it holds instead of "linked" (core `link_value`): first its
  **preview** (core `preview_values`, recomputed when the graph changes: `NodeSpec::pure` blocks are run for real,
  steps give their predictable outputs through `NodeSpec::preview` without touching files: Original texture / Export
  image / Use existing image / Make folder / Require file their path, Edit image what comes in, Copy / Move
  `destination_for`, Rename folder + new name, Package `<out>\<name>.zip`; a block whose input isn't known gets
  nothing), else the source's value from the last run (`RunResult::values`, also on `RunError`), else "linked
  (known after a run)". Only Convert image to texture (a temporary .tex) and what depends on it wait for a run. A new
  step type should get a `preview` if its output is predictable.
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
  app doesn't route while blocks are being dragged (user, 2026-10-01: routing every frame made dragging lag in the
  Debug build): links are plain elbows between their pins until the blocks are let go, then one quick pass and then
  all passes (19 ms in Debug for the example graph). Value previews and thumbnails ignore block positions
  (`shape_of`), so moving blocks doesn't recompute them. Lines keep 0.9 gap (the gap = 0.8 font, a pin's stub length) from every block except at their own pins;
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
- ExportImage never overwrites an existing PNG **that goes to an Edit image step** (through Splits, `for_editing`):
  the user's edit. One run exports every PNG that needs editing and skips only what depends on a waiting EditImage.
  So a mod is: Run, edit, Done editing, Run. **Not going to an Edit image, the file is a working file (user,
  2026-10-02: "an intermediary image", e.g. Replace photo's frame; deleting it by hand was bad UI):** previews start
  from the texture, never from the file, and a run exports it again only when needed (user: avoid needless write
  cycles): the texture differs from the recorded one, or the file's write time differs from the recorded one (changed
  or deleted outside the tool). An unchanged Run writes nothing for it (it used to cost ~2.5 MB: PNG, Noesis's temp
  copy and log). **Also skipped when unchanged (user, 2026-10-02):** an image block's Save to (the result is compared
  with the file, read back; "unchanged" in its message) and Package (`build_package(..., &unchanged)`: the same files
  with the same bytes, the same modinfo.ini, nothing else in the folder, its zip there: left as it is).
  **Run cache (2026-10-02): an unchanged run writes nothing** (`RunOptions::cache_dir`, app and CLI:
  `%LOCALAPPDATA%\remod\run_cache`, `default_cache_dir()`; empty = the run's temporary folder, as in most tests).
  Convert image to texture names its `.tex` by a hash of the image's and the original's bytes (and the converter's
  `id`) and reuses one already there ("unchanged: reused ..."; nothing is converted, which also saves seconds); an
  image block without Save to names its PNG by a hash of its pixels. Written as `.part` then renamed, so a cut-off
  write is never reused; a reuse touches the file; oldest files go past 512 MB. ponytail: `std::hash` (64-bit
  FNV-1a), not a cryptographic hash. A Noesis update doesn't clear the cache (older results were valid); a change to
  the built-in converter's output should bump its `id` ("native1"). Downstream
  steps get the cache file's path (a Move of it just means a fresh conversion next time).
  **A file for editing whose texture changed (user, 2026-10-02: a new frame texture still previewed, and would have packaged,
  the old frame's PNG; overwrite chosen over a backup or a refusal):** each run records the texture the file is from
  (node state `exported_from` / `exported_time`, via `RunResult::state` / `RunError::state` and `apply_run`); a
  different texture re-exports over the file (logged; the Edit image's "done" resets), and previews skip the stale
  file and start from the texture. No record (graphs from before, the CLI, which doesn't save): the file is trusted.
- PackageMod packages every linked texture. Several previews are tiled into one `preview.png`.
  "Replace existing" overwrites only a previous build of the same mod.
- Old graph files are migrated on load: PackageMod's typed `screenshot` becomes ImportImage → preview.
- LoadTex infers the game path when the `.tex` sits inside a `natives/STM/...` tree; otherwise set `game_path`.
- **Image blocks (user, 2026-10-01): Adjust colour, Resize image, Overlay image** (steps, Transform family; core
  `adjust_colour` / `resize_image` / `overlay_image` on `Bgra` pixels, WIC for reading, scaling and PNG). Each writes a
  PNG to the run's temporary folder, or to its optional **Save to** (`InputSpec::result`, on the output row; written
  again each run), which is then its output and known before a run (`preview`). Colour changes and overlays keep the
  alpha channel as it is (RE textures often hold other data there). Resize: width / height (one empty keeps the
  shape) or "Match size of" an image or texture (`read_tex_meta`); fit (bars) / fill (crop) / stretch. Numbers are
  `Widget::Number` fields (range, display format, text for 0 such as "auto"; stored as text): a drag field in the
  app (Ctrl+click types), checked again in the run ("Hue must be a number from -180 to 180"). **Every such
  slider has a "back to default" button beside it (user, 2026-10-02):** a drawn arrow, dimmed at the default, sets
  `InputSpec::initial`; the hover says the default. New sliders get it by being `Widget::Number`.
  **Live thumbnails (user, 2026-10-01):** core `preview_image` works out an image block's (`NodeSpec::thumbnail`)
  result in memory at <= 256 px from its real input (an image block upstream, through Splits, or the file a preview
  or field names), scaling sizes and positions to the shrunk copy (`ImagePreview::scale`). The app recomputes them in a
  background job when the graph changes (positions aside; one job at a time, the next starts when it's done), caches
  shrunk files by path and write time, and draws each under its block on a checkerboard. **Previews start at the
  texture (user, 2026-10-01: "I cannot access the output without running"):** Export image hands on its PNG once it
  exists (it may hold an edit), else its texture, which the app's loader decodes (`decode_tex`, DirectXTex); Edit
  image and Use existing image hand on theirs. So a fresh graph shows thumbnails before any run. Checked in the app
  on a test graph (Original texture → Export image, nothing exported → Adjust colour: thumbnail made from the
  texture). Padded-width textures decode at their visible width, as the built-in converter exports them (Noesis
  exports the padded one, §9).
  **Larger previews (user, 2026-10-02: "pop out any and all preview images"; and "it just shows up in the existing
  mesh and texture preview area"):**
  - Clicking a block's thumbnail: Use layout shows it in the Browser's viewer (`Browser::show_in_viewer`, until a
    texture or mesh is picked there again), with a Pop out button; Build layout (no viewer) pops it out at once.
  - Popped-out windows: one per block, any size, closed with their X or with the block.
  - Larger previews are worked out at 1024 px (`State::big`, a second preview set beside the 256 px thumbnails, its
    own job and file cache), only for blocks in the viewer or popped out; the thumbnail stands in meanwhile.
  - Browser textures and images: right-click → Pop out (outside the game files too), or the viewer's Pop out button.
  - Every one zooms and pans like the viewer (app `zoom_area`: wheel, drag, double-click fits).
  - In-app windows, not OS windows: ImGui's multi-viewport mode isn't on. They never dock
    (`ImGuiWindowFlags_NoDocking`): docked into the Graph's slot, dragging the slot moved the graph with them (user).
  - **Previews follow the files they read (user, 2026-10-02):** each preview job records every file its loader read
    with its write time (missing ones too); once a second the app checks them, and a change (a picture saved over, an
    edit saved in GIMP) or a file appearing works the previews out again. Before, only a graph change did.
  - **Preview block (user, 2026-10-02: "place the image preview output onto my graph directly"):** one input of any
    kind (an image, a texture, or a path to one: `preview_image` passes it through), a Size field (px at 100% zoom;
    `NodeSpec::view_size` names it, so the app sizes the block and its picture from it and uses the 1024 px preview).
    Output family, pure, no outputs; a run marks it done and touches nothing. A value that also goes on needs a Split
    (an output feeds one input).
- **Replace photo (ReplacePhoto, user 2026-10-01: frames "filled end to end, fit with the curve, keeping the dirt"):**
  a picture in place of the photo in an opaque frame (RE4R UI frames such as `ui3200/.../cs_ui3210_file_039_00_iam`:
  alpha is only the frame's outline; the old photo is opaque, its ageing baked in). Core `photo_area`: a frame's inner
  edge runs parallel to its outline, so the brightness change across the outline (Sobel projected onto the distance
  field's normal, signed) is averaged at each distance from it: rings add up, the photo's content cancels; the
  innermost peak (>= half the strongest, searched to 80% of the depth) is the photo's edge ("Frame width", 0 = auto),
  then snapped within a band (15% of the width) by dynamic programming over 720 rays (a closed, smooth contour).
  An unsigned average failed on the real texture (the grimy photo had as much edge as the rings: it picked 164 px
  inside the photo); the signed one found 91 px, on the edge, at full size and on the 256 px thumbnail. Grow and
  Feather. **No picture on the frame (user, 2026-10-02: auto "slightly clipped" the frame on some sides and corners,
  fixed by hand with Frame width):** measured per column on `file_039` and `questfile_main_002_01` (where the mask
  starts vs that column's frame-to-photo step). Causes and fixes:
  - Grow defaulted to 2 px: 42-86% of columns covered the frame by 2+ px. Default now 0.
  - The snap score was capped at half its 95th percentile, so every clear edge scored the same and the path wandered
    1-3 px. Now capped at the 95th percentile (a ramp's steepest point wins), and the pull towards the ring is 3x
    harder from outside it (0.15 vs 0.05 per band), so of two equal edges the inner one wins. questfile's top: 36% of
    columns covering -> 5%.
  - A signed score (only edges stepping frame-to-photo) was tried and failed: a photo can be lighter than its frame
    in places and darker in others.
  - Feather blurred the mask both ways, putting a third of the picture on the frame's last pixel; now it softens
    inwards only.
  Where unsure (file_039's dark, stained bottom; mitred corners) the edge now leans into the photo, so a sliver of
  the old photo can show there instead. Core `replace_photo`: the
  picture covers the area (Fill), then the old photo's ageing at slider strengths. **None of them may carry the old
  picture over (user, 2026-10-02: Scratches overlaid the old image; the see-through look was everywhere):**
  - tone: brightness mean / spread, colourfulness, and the old toning as a line over brightness (sepia is browner in
    the shadows; fitted twice, the second time without the pixels far off the first line, i.e. stains);
  - shading: brightness by distance from the photo's edge and direction round it (36 sectors x 24 rings up to a
    quarter of its size, interpolated, eased out), relative to the innermost ring in the same direction and only
    darker (a frame casts shadow). Relative to the photo's mean it copied the old composition (light wall, dark floor);
    allowing lighter brightened the edge beside the old man's dark coat;
  - stains: the colour the toning line doesn't explain, blurred 3 px, applied as a multiplied tint
    ((brightness + colour) / its strongest channel), so stains darken (user: stains were too light). Colour away from
    one flat cast had followed the old picture's content;
  - scratches: the old brightness minus its median (5x5 at full size, inside the photo only; a median keeps step
    edges, so outlines don't pass, specks and thin scratches do), small residuals dropped (under 2x the typical one),
    and faded where the median image is busy (eyes, buttons, patterns sit among the picture's own edges). A box
    high-pass drew every outline of the old picture. Default 0.
  Checked on the real texture with a flat grey picture, each term alone (images saved and looked at): no faces in
  any; jacket pattern and a few buttons remain faint in Scratches at 100. Test: a sepia photo with a dark disc,
  specks and a stain: no edge where the disc was, specks and a darker stain carried. "Show edge" draws the found
  outline on the thumbnail only.
  **Framing (user, 2026-10-02: heads near an edge got cut off):** Zoom (100-400%) and Picture X / Y (-100 = its
  left / top edge, 100 = right / bottom): core `Framing`; the part that shows is cut from the picture, then scaled to
  the area. An empty Zoom (older graphs) is 100%.
  **Frame width on auto shows what was found (user, 2026-10-02: "a frame of reference"):** "auto (~91 px)", from the
  thumbnail's search (core `ImagePreview::found`, real pixels; ~ because a thumbnail pixel is several real ones), and a
  drag starts there (a click alone stays on auto). Larger moves the edge inwards; past the frame's middle the
  thumbnail says so with the limit (`preview_image`'s `why`, shown red under any block whose preview fails). Checked on the real texture (images saved and
  looked at, not screenshots). Thumbnails shrink one-mip textures (most RE4R UI textures) to 256 px.
- **Cut text (CutText, user 2026-10-01: "truncate text based on the character content"):** keeps the part of a text
  or path after / before / from / up to a marker, at its first or last occurrence (core `cut_text`), ignoring case and
  treating `\` and `/` alike; a missing marker fails the run. E.g. a texture path cut after `natives/STM/` gives the
  in-game path for LoadTex's `game_path` when the file isn't inside a natives tree.
- **Fan-out (2026-10-02, priority 1, §10): Files in folder repeats the blocks it feeds, once per file.**
  - **The block:** `FilesInFolder`, Source family, pure, so previews list the folder. Fields:
    - Folder.
    - Files: patterns with `*` and `?`, `;` between several; empty = every texture by name.
    - Include subfolders.
    - If a file fails: Stop the run (default) / Skip that file (user).

    Its output is a **list** (`PortSpec::list`), of the kind it feeds, like Value. Items are sorted by their path in
    the folder. Textures under a natives tree carry their game path.
  - **The engine repeats; node code doesn't know.**
    - `list_source`: a block is repeated for the list that reaches one of its single-value inputs.
    - A block taking a list only into `multiple` inputs (Package's textures and previews) runs once and gets every
      item.
    - Outputs are kept per item (`RunState::outputs`, key (node, port, item)), and `NodeRun::values` hands a
      repeated block the current item's value.
    - The run goes block by block, every item in turn, so one run still exports every image to edit, then waits.
    - Two different lists meeting at one block is refused by validate.
  - **`{name}` (user):** in a repeated block, `NodeRun::text` fills `{name}` with the item's name. That's a texture's
    name before `.tex` (`x.tex.143221013` -> `x`), else the name without its extension. A Value holding it may feed
    a repeated block. Elsewhere, validate refuses it.
    - Two items writing one file fail the run, with the hint to put `{name}` in the file name (`NodeRun::claim`,
      called by every Write change and by Export image even when it keeps its file).
    - Temporary files are per item (`NodeRun::temp_file`).
  - **Done per item (user):** state a repeated block keeps is per item (`NodeRun::state` / `set_state`: node param
    `<name>@<item key>`, e.g. `done@ui/a.tex.143221013`, `exported_from@...`; apply_run removes a state set to "").
    - Items marked done go on to the next steps; the others wait.
    - A block that isn't repeated waits while any item of its list waits (`blocked`): Package never packages part of
      a list.
    - The app: the YOUR STEP card lists each waiting image with Open and Done / Undo, plus "Done editing all"; the
      block's button marks them all; "Edit again" clears all.
    - API: `edit_done` takes an `item`.
  - **Failures:**
    - Stop: a RunError naming the item, e.g. "Original texture (node 2) (bad): ...".
    - Skip (`RunState::skipped`): the item is left out of every later block and of collecting inputs, with a warning;
      the block shows Failed.
  - **Statuses:** `NodeStatus::items` (name, key, state, message, file). The block's own state and message sum them
    up: waiting > failed > not reached > done, e.g. "3 items: 1 done, 2 waiting for you". The API's run reply gives
    each node's `items`.
  - **Previews and the plan** run every item, so `plan_changes` lists every item's writes.
  - **Which file previews show (2026-10-02, user):** RunValues, link values and thumbnails show one item of a list:
    - the one the list block's `show` state names (an item key), else the first;
    - if the shown one didn't get that far, the first (`first_values`, `shown_item`).
    - `preview_values(..., &lists)` hands front ends each list block's items; the app's Files in folder block has
      ◀ ▶ arrows and "Previews: <name> (n of N)".
  - **Not done:** see §10 Batch.
  - Checked: tests (a 3-texture pipeline with per-item edits, one file name for all, stop / skip on a bad texture,
    plan and validate, an API copy of two files). **Not checked by eye** (the YOUR STEP list, the block buttons).
- **Run program (2026-10-02, priority 3, §10; user: "claude -p, scripts, a possible ComfyUI bridge"):** `RunProgram`,
  a Transform step.
  - **Program:** a path (relative to the graph's folder), or a bare name looked up there, then on PATH, trying .exe,
    .cmd, .bat, .ps1 and .py (`find_program`; `claude` finds claude.exe or npm's claude.cmd). No picker filter, so
    bare names pass validation.
  - **Arguments:** split like a command line (`CommandLineToArgvW`), then `{in}` (the single input, any kind),
    `{out}` (Output file) and `{name}` (repeated for a list) are filled in. Filling after splitting keeps a path with
    spaces one argument. Using `{in}` or `{out}` with nothing there fails the run. A Text block can feed the
    Arguments field to build them from several values.
  - **Output file** (`result` row, flippable like other destinations): must exist after the run, then it's passed on
    with the input's game path.
  - **Time limit:** advanced, 600 s.
  - **Outputs:** "what it printed" (stdout and stderr, one pipe, trailing space trimmed) and the file.
  - **How it runs:**
    - `run_process` on a private desktop (a dialog kills it and reports its text), in the graph's folder (new `cwd`).
    - .ps1 through System32's PowerShell `-NoProfile -ExecutionPolicy Bypass -File`; .py through py.exe, else
      python.exe.
    - .bat / .cmd through System32's `cmd.exe /d /s /c` (new `run_process_line`, the line as is). cmd reparses its
      line and nothing escapes that safely, so arguments holding `& | < > ^ % !`, quotes or line breaks are refused,
      never passed.
    - A non-zero exit fails with the output's tail.
  - **Guardrails:** `ChangeKind::Run` (the program's path). Its preview and run record it, and the Guard always asks
    approval for it, whatever the folder (plan action "run program"). After that nothing checks what the program
    does: it runs as the user. The app and the CLI don't ask (the user's own graph).
  - **Runs again only when needed (2026-10-02, priority 4; user: no needless runs).** It records `ran_key` (a hash of
    the program's path, write time and size, the graph folder, the output file, the arguments as filled in, and the
    input: a file, or text naming one, by its bytes), the output's write time (`ran_time`) and what it printed
    (`ran_text`, up to 64 KB, else nothing is kept).
    - It records them per item and per custom block like any state; the app and the API keep them (`apply_run`), the
      CLI doesn't.
    - Next run, the same key, the output untouched and **Always run** (advanced checkbox) off: it passes on that
      result without starting anything ("unchanged: kept the last result"), and doesn't ask the guard.
    - Not watched: files only the arguments name, the web, the time; Always run is for those.
    - Checked: a counting batch file (unchanged run skipped with its printed text kept; reruns on new input bytes, a
      deleted output, Always run; re-spaced arguments still unchanged).
  - Checked: tests with cmd.exe, batch files, a .ps1, refused characters, time-out, a list of files, an API run
    needing approval. Not tried: real `claude -p`, which isn't installed on the build machine.
- **Custom nodes (2026-10-02, priority 4's subgraphs; user: "make subgraphs a kind of custom node"; core
  `custom.*`).** A block type the user makes from a graph.
  - **Definition:** `CustomNode` {type `custom:<name>`, title, summary, graph}.
  - **Pins (user: Input / Output blocks):** inside, `NodeInput` blocks (Pin name, Default) are its input pins and
    `NodeOutput` blocks (Pin name) its output pins, top to bottom.
    - A pin is of the kind it's linked to inside.
    - An input with a default is a typed field on the block, not required. Pin ids are `in<inner id>` /
      `out<inner id>`, stable while the inner block stays.
    - Editing the custom node's own graph, an Input gives its default like a Value, so previews work in there.
  - **Storage (user: library + copy):**
    - The library is `%APPDATA%\remod\nodes\<name>.json` (`custom_library_dir`, `load_custom_library`,
      `save_custom_node`).
    - Each graph keeps copies of the custom nodes it uses (`Graph::customs`, file `custom_nodes`; `save_graph` writes
      only the ones in use; nested ones inside their own definition's graph), so a graph opens and runs anywhere.
    - `add_node` copies a definition in on first use.
    - A library version that differs is offered in the Pipeline panel ("Use the library's", `library_differs`,
      `update_custom`).
  - **Registry:** `register_custom` makes a type a block type. `find_spec` falls back to it; `custom_specs` lists
    them, and `all_specs` is built-ins plus customs, for menus, `choices_for_*` and the API's `types`.
    - Specs and strings are kept forever (a deque), so pointers a background preview holds stay valid.
    - The app registers the library at start; `load_graph` registers the graph's copies, which win.
    - ponytail: global; one graph open at a time.
  - **Expansion: the engine never meets a custom block.** `expand_customs` replaces each (nested too) with its inner
    blocks, titled "<block> (in <custom block>)", with new ids above the graph's.
    - A link into a pin goes on to what its Input feeds; a typed value (or the default) becomes a Value block; what an
      Output receives goes on along the block's links.
    - Inner state lives on the custom block as `<inner id>:<key>`, e.g. `3:done`, `2:exported_from@<item>`.
    - `validate`, `run_graph`, `preview_values` and `plan_changes` expand first. `fold_results` maps results back:
      - the block's status sums up its inner blocks (waiting > failed > not reached > done);
      - its `items` list each inner Edit image (and each list item of one), with the full key `set_edit_done` takes
        for a custom block;
      - its outputs' values;
      - inner state under the prefix (reset edits included);
      - plan changes and unknowns put down to it.
    - So fan-out, Edit image waits, previews, the run cache and the guardrails all work inside one.
  - **Making (user: from a selection):** Build layout, block menu, "Make custom node..." (the selected blocks, or that
    one), then a title. `make_custom_node`:
    - every link crossing the selection becomes a pin, named after what it joins;
    - the blocks are replaced by one custom block where they were;
    - the new type is registered, copied into the graph, and saved to the library.
  - **Editing:** the block menu's "Edit custom node" opens its graph in the canvas (`State::parent` keeps the
    layout).
    - Save (button or Ctrl+S) writes the definition to the library and into the layout's copy.
    - "Back to the layout" asks about unsaved changes. New, Load and closing refuse while one is open (so the
      layout's unsaved changes can't be lost).
    - ponytail: the layout's undo history resets on Back.
  - Checked: tests (made from a selection with an Edit image inside, run, per-step Done, state on the block; typed pin
    vs default; save / load; library round trip and differs / update; nested; unknown and self-containing types;
    plan; API types and add). **Not checked by eye** (the menu items, the popup, the edit / Back flow, the Custom
    nodes folder).
- **Channel tools (2026-10-02; §9's survey, §10).** Image blocks with thumbnails; core `channel_image` /
  `merge_channels` (image.*).
  - **Pick channel:** one channel as a grey image (opaque), or Colour (alpha made opaque), to see or edit data
    alone.
  - **Merge channels:** a base image with its colour from "Colour from" (alpha kept) and any single channel from a
    grey image (its red), e.g. an AI picture as the colour of the original `albd`, keeping its alpha.
  - Every image must be the base's size (the run says so, pointing to Resize image); the thumbnail stretches to fit.
  - Merging nothing in fails the run.
  - Checked: tests (pixels both ways, a graph run, the thumbnail).
- **Masks (2026-10-02; §10's "change only the jacket").** Image blocks with thumbnails.
  - **Mesh mask (`MeshMask`):**
    - a mesh (read natively, `read_mesh`), the texture or image it's for ("Size of": the mask's size) and Materials
      (wildcards, `any_pattern`; empty = all);
    - gives a mask: white where those parts' UV triangles fall (core `uv_mask`; UVs past 0-1 wrap, a triangle moved
      by whole tiles to stay in one piece), then grown by Grow (advanced, 2 px) over the seams;
    - no matching material fails the run listing the mesh's materials (`pick_parts`), for the user and an AI;
    - the thumbnail draws it straight at its own size.
  - **Blend in mask (`MaskBlend`):** Original, Edited and Mask (any grey image: a Mesh mask, Pick channel, a painted
    file) give the edit where white, the original where black, mixed between (core `masked_blend`).
    - Feather (px, softened inwards and outwards: a box blur of the mask) and Invert (advanced).
    - The original's alpha is kept. Sizes must match (the thumbnail stretches).
  - Masks from pictures need no block of their own.
  - Checked: tests (`uv_mask` on a triangle, wrapped and grown; blend pixels, invert, feather, sizes; a graph run; a
    real mesh: one material's area, and a wrong name listing them). Looked at (an image file, not a screenshot): the
    mask of Leon's `Pants_Mat` blended white over `cha000_00_lowerbody_albd` covers exactly the trouser panels, so
    the UV orientation is right.
  - Not done: a materials dropdown filled from the mesh (the field is text), and matching a mesh's textures
    automatically (`mesh_textures` needs the game files index).
- Intermediate `.tex` files go to a per-run temp folder, deleted afterwards.
- Runs from `remod run --graph <file> [--noesis <exe>]` and from the app's Run button. Programs edit graphs through
  `remod api` (core `ApiSession`, §10 M2).

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
profile's `tex_suffix`, which identifies the game (`profile_for_texture`). **Files may be named anything ending in
`.tex` or `.tex.<suffix>` (user, 2026-10-02: tools also use `.tex.re2remake`, `.tex.re3remake`): `is_tex_name`.**
An image suffix (`x.tex.png`) isn't a texture. The built-in converter never looks at the name; the Noesis one
feeds Noesis a temp copy named `src.tex.<version>`. Packaging names every texture `<name>.tex.<profile suffix>` in
the mod, whatever its suffix was, and refuses a file whose header is another game's version (`build_package`).
The game-files index lists suffixed names only (materials find textures by the suffix).

**Header layout by version** (`parse_tex_header`, [REE-Lib TexFile.cs, MIT]). Both layouts have the same 16-byte
mip entries `{u64 offset, u32 row pitch, u32 size}` per image and mip, image 0's first:
- **Legacy, 32 bytes** (8 RE7, 10 RE2, 11 DMC5, 190820018 RE3): mip count @14, image count @15.
- **Modern, 40 bytes** (28, 30, 34, 35, 143221013, 760230703, 251211553, 240606151, 240701001): image count @14,
  mip table bytes @15.
- **GDeflate** (241106027 MH Wilds, 250813143 RE9, 251111100): compressed mip data; refused, pointing to Noesis.
- **Unknown versions:** refused.

Only RE4R is tested on real files; the legacy layout only on a synthetic one.

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
| Noesis | **Optional** (2026-10-02): texture conversion if chosen; the 3D view's fallback for meshes the tool can't read | Freeware, no redistribution terms found | Don't bundle. Offer `winget install -e --id RichWhitehouse.Noesis`, or detect existing install |
| fmt_RE_MESH Noesis plugin | **Optional**, with Noesis | Fork checked had **no license file** (all rights reserved by default); original repo not checked | Don't bundle, don't copy from it; the user installs it. Format knowledge is cited from REE-Lib (MIT) instead |
| REE-Lib (kagenocookie/RE-Engine-Lib) | Reference only: `.tex` and `.mdf2` layouts (`TexFile.cs`, `MdfFile.cs`); later `.mesh` (`MeshFile.cs`) | MIT [official] | Not linked (C#). No code copied; if any ever is, add its MIT notice |
| REtool | PAK extraction / optional PAK creation | No license found | Don't bundle. Detect path; guide manual install |
| Fluffy Mod Manager | Install/test mods | No license found | Don't bundle. User selects path |
| texconv (DirectXTex) | Not needed: the DirectXTex library does the encoding (§2, §4) | MIT [official] | — |
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
- [x] **Built-in converter (2026-10-02): no mismatch.** It keeps the original's mip count and layout (§4), so the
      two items below apply to the optional Noesis converter only. Still to check in game: listed under "In-game
      checks" below.
- [ ] **Noesis only. Waiting (user, 2026-09-30): parked until it causes a real problem; no in-game test planned.**
      SaveTex no longer refuses a different mip count: the run succeeds with a warning (`RunResult::warnings`,
      a popup in the app, `WARNING:` in the CLI). Checked: a 1-mip 256x256 UI texture converts to 6 mips.
      **Mip count mismatch (used to block most UI textures).** The plugin's writer always generates mips down to 8x8
      [plugin source]. Found 2026-09-30 in the REtool extraction: 477 of 493 RE4R UI textures have 1 mip, and
      others stop above 8x8 (e.g. 256x256 with 5 mips). SaveTex refuses any mip mismatch, so only textures whose
      chain ends at 8x8 convert today. Open: does the game accept a texture with *more* mips than the original?
      If not, the extra mips must be dropped after export (header + mip table rewrite per the plugin's layout),
      which needs an in-game check before relying on it.
- [ ] **Padded width on export (Noesis only).** `cs_ui3200_stamp_im` (468x440) exports as a **512x440** PNG: the
      plugin decodes the stored (pitch-padded) rows, and the size check refuses it. The built-in converter exports
      the visible 468 and writes back at the original's padded pitch (checked: header and mip table byte-identical).
- [ ] **Streaming textures.** 17,728 of 38,331 RE4R textures have a high-resolution copy under
      `natives/STM/streaming/<same path>`. Does replacing a texture require replacing its streaming copy too?
      LoadTex logs a note when one exists.
- [ ] Screenshot size/aspect requirements for Fluffy, if any (formats: jpg/png/tga/bmp [guide]). The tool
      combines several previews into one 512-px-per-tile grid PNG.
- [ ] Should a tier-1 package include `manifest.json`, or only tier 2/3?
- [x] **What RE4R textures pack in each channel** (read-only spike 2026-10-02, for channel tools). The extracted
      game files (20,828 textures outside `streaming/`, 6,392 `.mdf2`) were surveyed. Three sources:
      - **[game data]** which material slot each file-name suffix fills, from every `.mdf2`;
      - **[game data]** the formats, from the headers;
      - channel statistics (40 textures per suffix, decoded at <= 128 px), plus one character texture per suffix
        saved channel by channel and looked at.

      Results:
      - **The format tells colour from data, from the header alone.** Colour textures are `*_SRGB`:
        - `albd` (BC1 3,038 / BC7 2,060);
        - `alba`, `albm`, `alb`;
        - UI `im` / `iam`.

        Data textures are `*_UNORM`: `nrrc`, `nrmr`, `atos`, `atoc`, `occ`, `lymo`, `msk*`. A tool can therefore say
        "this holds data, not colour" without trusting names.
      - Suffix, material slot, and what the channels show. Meanings are **[inferred]** from slot names and looks; no
        shader was read and nothing was tested in game.

        | Suffix | Files | Slot | Channels |
        |---|---|---|---|
        | `albd` | 5,102 | BaseDielectricMap | RGB colour. Alpha is a mask in 15 of 40, white in the rest. On a flashlight it is dark on the lens and metal parts. Inferred: dielectric vs metal. |
        | `albm` | 56 | BaseMetalMap / BaseDielectricMap | RGB colour. Alpha is mostly 0. Inferred: metal. |
        | `alba` | 392 | BaseAlphaMap | RGB colour. Alpha varies in 36 of 40. Inferred: opacity. |
        | `nrmr` | 1,069 | NormalRoughnessMap | RGB is a unit normal (mean \|len²−1\| 0.03; R and G relief, B near white). A is a flat-region mask. Inferred: roughness. |
        | `nrrc` | 4,226 | NormalRoughnessCavityMap | **Not RGB.** The relief is in **G and A** (a swizzle: X in A). R is a flat-region mask (inferred: roughness). B is nearly white (inferred: cavity). |
        | `atos` | 1,795, mostly BC1 | AlphaTranslucentOcclusionSSSMap | R is near white (inferred: alpha). G is near constant (inferred: translucency). B looks like ambient occlusion. A: none in BC1. |
        | `atoc` | 789 | AlphaTranslucentOcclusionCavityMap | Like `atos`, but A varies. |
        | `occ` | 2,381, BC4 | OcclusionMap | One channel. |
        | `msk1` / `msk3` | | | Mostly BC4, one channel. |
        | `msk4` | | Several slots | No single layout. The sample, a WrinkleNormal / Detail slot, was a unit normal. |

      Consequences:
      - The image blocks already keep alpha, so colour edits on `albd` / `alba` / `albm` leave the alpha data alone.
      - An AI result (plain RGB) needs the original's alpha put back: a merge.
      - Data textures shouldn't get colour edits at all.
      - Any tool treating `nrrc` as a normal must read G and A, not R and G.
      - Split / merge itself needs no meanings; only labels do.

      Still open: whether these meanings are right (a shader or in-game test).

### In-game checks, for when mod-making starts
**User, 2026-10-02:** the tools are being built now; these get checked once real mods are made with them. Don't
propose them as next steps before then.

- [ ] **Built-in converter (§4).** Build one mod with it and load it through Fluffy. Ideally include:
  - a 1-mip UI texture (e.g. `cs_ui3210_file_039_00_iam`);
  - the padded `cs_ui3200_stamp_im` (468 visible, stored 512).

  Look for a correct image, nothing shifted or skewed (padding), no mip shimmer.
- [ ] **Streaming copies (§9).** Does replacing a texture also need its `streaming/` copy? This decides what
  character / AI textures must target.
- [ ] **Channel meanings (§9 table).** Are the inferred meanings right (e.g. `albd` alpha = dielectric vs metal,
  `nrrc` normal in G/A)?
- [ ] **Noesis converter only:** does the game accept a texture with more mips than the original (§9, parked)?

## 10. Later milestones (do not start)

- **Priority order (user, 2026-10-02). See also the M2 "Order" below.**
  1. **Fan-out over many textures:** Batch, below. **First version done 2026-10-02** (§4 Fan-out).
  2. **Native TEX handling** to remove the Noesis requirement for textures. **Done 2026-10-02** (§4).
  3. **A generic external-process node:** `claude -p`, scripts, a possible ComfyUI bridge. **Done 2026-10-02**
     (§4 Run program). Its guardrail rule: starting a program always needs approval (`ChangeKind::Run`). Not done:
     a ComfyUI bridge itself (a script the block runs would do), and passing images to `claude -p`.
  4. **Caching and subgraphs.** Caching partly exists: the run cache means an unchanged run writes nothing (§4).
     **Run program cached 2026-10-02** (§4 Run program). **Subgraphs done 2026-10-02 as custom nodes** (§4 Custom
     nodes). The priority list is done.
- **Future work (user, 2026-10-01; "we are building the engine, not the implementation yet"):**
  - **Batch:** lists through links: an output can carry a list, a block fed one runs per item, collecting inputs
    (Package's textures) take them all; a "Files in folder" source (folder, pattern, subfolders); Export image would
    need a per-item name (e.g. a folder field). User (2026-10-01): not yet, "dumb rebuilding textures isn't really
    helpful" until there's more to do per texture. **Now priority 1 (2026-10-02). First version built (§4 Fan-out):
    lists through links, Files in folder, `{name}`, done per item, stop / skip.** Not done yet:
    - ~~thumbnails show the first item only~~: done, the list block picks which (§4 Fan-out);
    - list links look like any other (a doubled line?);
    - two lists meeting (zip or cross product) is refused;
    - other list sources (the Browser's selection, a typed list);
    - a game path for textures outside a natives tree (e.g. `{name}` in LoadTex's In-game path works, but only for
      one folder);
    - a Package repeated per item shares one temporary preview file (sequential, so it works).
  - **Channel tools:** split / merge channels, so colour can be edited without touching data packed in another
    channel. Which RE4R textures pack what: §9 (spike 2026-10-02; meanings inferred, layouts observed). **Built
    2026-10-02** (§4 Channel tools).
  - **Mod options:** one mod with variants to choose in Fluffy.
  - Frames: done as **Replace photo** (§4; RE4R's UI frames are opaque, no transparent opening). Open: a picture
    printed at an angle or in perspective (four-corner warp) if one turns up; in-world paintings usually have their
    own picture texture (Resize "Match size of" covers them).
  - Declined: **Install to Fluffy** (the user installs by hand). A template library: only one real workflow exists
    yet (the RE4R texture mod).
- **Done (2026-10-02): more verticality in the left-to-right flow, narrower graphs** (was "Later: Tidy up should
  allow more vertical inputs"; user chose "fields linked from above", "branches stack down", then the rule below,
  wrapping, Splits without a column and folding unused settings):
  - core `tidy_layout`: the longest chain of links is one row; every other block as late as it can go (just before
    the first block it feeds: a side branch lines up under where it joins); a helper feeding the main chain in a row
    above, right over what it feeds; every other branch (linked blocks off the main chain, a Preview) in a row below,
    sharing a row with branches whose columns don't overlap. A Split takes no column: it sits in the gap before
    what it feeds, level with the middle of what feeds it. Past `max_width` (the app: the view's width at about 70%
    zoom) the columns wrap onto a new band underneath, cut where no branch (or helper and its block) spans the cut
    if there's such a place.
  - **From above only from small helpers (user, 2026-10-02: "only from value nodes, not large nodes"):** core
    `is_helper` (a utility with nothing linked into it: Value, Text) and `from_above(link)`. Such a link comes into
    the block's top edge (any input, not only fields), spread along it in row order; everything else from the left.
    Grabbed and dropped on at its row's label (`ed::PinRect`). Core `LinkRoute::to_top`: the stub points up and the
    path arrives from above.
  - **A Split is its junction dot at every zoom** (was a narrow block at Near): the Far drawing (`draw_far_block`,
    `symbol` in draw_canvas); its links meet at the dot, new ones drag from its right half.
  - **Settings only while in use (user, 2026-10-02: "only display ... ageing, scratches, feathering ... if we are
    using them"):** `InputSpec::advanced` (Replace photo's Frame width, Grow, Feather, Zoom, Picture X / Y, the four
    ageing sliders, Show edge; the Preview's Size). Such a field has a row only while changed from its initial value
    (core `at_initial`: numbers by value) or linked; the rest fold into one "+ N settings at their defaults" row that
    opens them (per block, while the app runs). The Nodes panel's previews fold them too.
  - **Top to bottom (`Graph::downward`, "flow": "down") is built but has no switch (user, 2026-10-02: big blocks like
    Replace photo don't suit it at Near zoom; a later consideration).** Core swaps x and y around the left-to-right
    code (`tidy_layout`, `make_room`, `route_links` take `downward`); the app spreads ports along top and bottom
    edges. A graph saved that way still shows that way.
  - Checked: tests (main row, helper above, side branch lined up below, Preview below, Split in the gap, wrapping and
    its cut, from_above, at_initial; routes into a top edge from above; downward swaps); the app starts on graphs with
    Splits, a linked field and a downward one without asserting.
    **Not checked by eye.**
- **Far future (user, 2026-10-01): drop a Browser path on empty canvas to add a block holding it** (.tex → Original
  texture, image → Use existing image, anything else → Value), as an option in an options menu. Not started; today
  a drop only fills a field.
- **Native `.mesh` reader: built 2026-10-02** (§10 browser, 3D view). Not done:
  - other games' mesh versions (REE-Lib `MeshFile.cs` has their layouts);
  - geometry only in a `streaming/` copy (none seen yet);
  - shadow- or occluder-only files (nothing to show).

- M2: AI backend as a separate local process (not in the app). GPU/VRAM detection at install.
  img2img/ControlNet for textures, never plain text-to-image for UV-mapped textures.
  - **Two ways in (user, 2026-10-02):** a single AI call as a block in the graph, and an **AI orchestrator** (an
    MCP-server-style interface) that builds and edits graphs itself. The orchestrator must know which blocks can come
    next and which can't: core already decides this (`node_specs()` with descriptions and pins, `accepts`,
    `choices_for_pin`, `add_connected`, `insert_node`, validation, `preview_values`), so it would be a thin layer over
    core like the app and the CLI, never its own rules.
  - **The orchestrator needs (2026-10-02, agreed with the user):**
    1. **Core usable by a program:** every edit as a call (list block types, add, link, set a field, validate, preview,
       run) with structured results and errors, not text for a person. **Built 2026-10-02:** core `ApiSession`
       (`core/api.*`: one JSON request -> one JSON reply; ops types, new, open, save, graph, add, remove, set, link,
       unlink, next, validate, preview, plan, run, edit_done; errors are `{"ok": false, "error": ...}` with core's own
       reasons) and `remod api` (JSON lines on stdin/stdout). Added blocks go in a row to the right; Tidy up in the app
       arranges them.
    2. **Guardrails on file-changing steps. Built 2026-10-02 (user: "add guardrails"):**
       - **Every change goes through one check:** every step that changes the user's files calls `NodeRun::change`
         (write / remove / make folder) before it does, in its run and its preview, with the same paths. This
         covers Export, Save to, Package's folder and zip, Copy / Move / Rename (Move and Rename also remove the
         source), Delete and Make folder. The run's temporary folder and the run cache aren't counted.
       - **The plan:** core `plan_changes` collects those changes from the previews, so nothing is touched.
         Blocks whose paths are only known in the run are listed as unknown (`may_change_files`).
       - **The rules (core `Guard`):**
         - Refused, with no approval possible: the game files (the RE plugin's NativesPath folder) and Noesis's
           folder.
         - Needs approval: removing any file, writing outside the graph's folder, and starting a program (Run
           program, `ChangeKind::Run`: nothing checks what it does).
         - Fine: everything else inside the graph's folder.
       - **Checked during the run too:** a run with `RunOptions::check_change` set asks the guard before every
         change, and the step fails with the reason. So a path decided only in the run can't get around the plan.
       - **The API flow:** `plan` returns each change with its verdict and an id made from the changes. `run` must
         name that id: if the changes differ now, it's refused and needs a new plan. Refused changes stop it, and
         changes needing approval need `approve: true`.
       - **Approval belongs to the user.** The program in between (an MCP server) must ask the user and never
         decide it itself. `approve` can't prove a person agreed.
       - **The app and the CLI's `run` aren't guarded:** there the user runs their own graph.
       - Checked: tests, plus a `plan` of the user's example graph against their real Noesis setup. Its two writes
         to `C:\spike` needed approval and its package write was fine. No API run with real Noesis yet.
    3. **Results it can see. Done 2026-10-02:** API op `image` (id, size 16 to 1024): the block's picture as the app's
       thumbnails work it out (`preview_image`, a loader shrinking images and decoding textures' mips), as base64
       PNG. `preview` also gives each list's files, and `set` takes a list block's `show`.
    4. **Manual steps. Done 2026-10-02:** a run stopped at an Edit image replies `"paused": true`, with the step
       "waiting for the user" and its file. The program hands control back, and `edit_done` marks the step done once
       the user has finished.
    5. **Block descriptions for an AI. Done 2026-10-02:** `ai_note(type)` (one table in nodes.cpp), `types`' "ai";
       custom nodes give their summary. Keep them current when blocks change (they name rules: {name}, Merge
       channels after AI pictures, never edit_done without the user).
    6. **The MCP server. Done 2026-10-02 (user: inside remod.exe, approval by a Windows dialog):** core `McpServer`
       (`core/mcp.*`) speaks MCP (JSON-RPC 2.0, one message per line: initialize, ping, tools/list, tools/call;
       notifications get no reply) over `ApiSession`; `remod mcp` runs it on stdio.
       - **Tools, one per op:** list_blocks, new_graph, open_graph, save_graph, show_graph, add_block, remove_block,
         set_field, link, unlink, next_blocks, validate, preview, view_image (an MCP image), plan_run, run, edit_done.
         The server's instructions teach the order of work.
       - **Approval is the user's alone:** no tool takes `approve` (one sent is dropped).
         - `run` plans, and if anything needs approval, asks `McpOptions::approve` with the list of changes.
         - The CLI's answer is a Windows dialog (Yes / No, No by default, topmost), which the AI can't click.
         - Then it runs against that plan, with approve = the answer.
       - The user's settings decide Noesis-or-built-in and the game files folder; the library's custom nodes are
         block types for `remod mcp` and `remod api` too.
       - Checked: tests (handshake, no approve anywhere, a run declined then allowed by the answer only, an AI's own
         "approve" ignored, view_image's PNG), and a stdio smoke test of `remod mcp`. **Not tried from a real Claude
         client yet**, nor the dialog itself (it needs the user).
  - **Order (2026-10-02; see also the priority order at the top of §10):** the orchestrator first, over the existing blocks (no generation needed: "replace this
    frame's photo with my picture and package it" works with today's blocks), then the generation block. The texture
    tools below move from "before M2" to "before the generation block".
  - **Tools the AI's results must pass through, to have before the generation block (user, 2026-10-02: "give the AI as
    many polished tools as we can"):**
    1. **Channel tools** (split / merge, §10 Future work): an AI result is plain RGB and would overwrite data packed
       in a channel. Spike done (§9): sRGB = colour, UNORM = data; `albd` alpha is data; `nrrc`'s normal is in G/A.
       **Built 2026-10-02** (§4 Channel tools: Merge channels with "Colour from" keeps the original's alpha).
    2. **Streaming copies** (§9): character textures are the ones with `streaming/` copies; whether a mod must
       replace both decides what every AI texture targets. Needs an in-game test (the user's call when).
    3. **Masks / regions:** "change only the jacket". **Built 2026-10-02** (§4 Masks: Mesh mask, Blend in mask).
    4. **Batch** (§10 Future work): worth it once there's real work per texture, which AI is.
    Done: an unchanged run writes nothing (the run cache, §4); AI iteration means many runs.
- M3: REFramework Lua runtime reading manifests (triggers → actions).
- M4: C++ REFramework plugin for video playback (in-game "cutscene" videos).
- Replace ImGui front end with a polished native UI.
- **Texture and mesh browser: first version built (2026-10-01).** Layout: Browser left (both layouts; Build layout
  shows only it), Graph middle, Pipeline right; in Use layout along the bottom the viewer (left corner: the selected
  texture or image, or the picked mesh in 3D until closed; the last mesh stays) and Textures (thumbnails of the picked
  mesh's or the place's textures and images, "Use in graph" via `texture_target`).
  - **Browses anywhere (user, 2026-10-01):** the tree is Pinned ("Game files" = the indexed REtool folder, then the
    user's pins, `Settings::pinned_folders`, right-click a folder → Pin / Unpin) and This PC (drives, `drive_roots`;
    folders read when opened, `list_folder`, kept until Refresh), with an address bar (Up, a typed path, Refresh).
    Outside the game files every file is listed (`file_kind`: textures, meshes and png/tga/jpg images get previews,
    images via `read_image_bgra`). Files are known by absolute path; inside the game files also relative to them.
    Search: inside the game files the index search (nicknames too), elsewhere a name filter of the current folder.
    **Nicknames apply inside the game files only (user: no file bloat).** A mesh's textures are matched only inside
    the game files (it needs the index); elsewhere the 3D shape shows untextured. The index still skips `streaming/`;
    browse to it on disk to see those copies.
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
  - **Native reading (2026-10-02; user: no Noesis needed):** core `read_mesh` reads the `.mesh` itself
    ([REE-Lib MeshFile.cs, MIT]; RE4R's version 220822879 only).
    - It takes the most detailed LOD whose triangles are in the file, one part per (group, material), in game units
      (metres; Noesis writes centimetres, and the view fits any size).
    - Checked against Noesis on 8 RE4R meshes (Leon, a head, props, a weapon): the same triangles, bounds (×100),
      (group, material) parts and UVs; normals agree with the geometry alike (REE-Lib decodes a byte n as
      (2n + 1) / 255).
    - Over every 10th RE4R mesh: 577 of 606 read, about 0.15 s each in Debug. The other 29 hold only shadow or
      occluder geometry.
    - The app reads natively and falls back to Noesis only when that fails and Noesis is set. The Noesis way is
      below.
  - **3D view** (in the Browser's details, mesh selected; app/mesh_view): Noesis exports the mesh to OBJ
    (`NoesisConverter::load_mesh`, `parse_obj`), the app draws it with D3D11 into a texture, each part with its
    material's colour texture (OBJ `usemtl` = mdf2 material name). Clicking a texture dims the parts not using it.
    Spike 2026-10-01: `?cmode <mesh> out.obj -b -noprompt` runs headless. Without `-b` the plugin's import window
    waits forever (not a dialog, so only the timeout catches it). 0.3-2.3 s for most meshes, 5.9 s / 20 MB OBJ for
    a 163k-triangle character; 24 of 26 sampled meshes load (2 hit plugin Python errors, reported). Every sampled
    part name matched a material. Highest LOD only; the main material file only (costume variants such as
    `cha000_00b.mdf2` aren't offered yet).
    Mesh groups: the plugin names OBJ groups `LOD_1_Group_<id>_...` (seen in its OBJ); parts are kept per (group,
    material) and the view has a checkbox per group (Leon's `cha000_00`: 0 shirt and gloves, 1 pants, 2 bare
    forearms, 3-4 weapons). Which groups the game shows isn't in the mesh (not read), so all start shown.
    Noesis's OBJ writes texture v top-down already (D3D style); flipping it again mirrored every texture and put
    Leon's shirt on the skin above it in his texture sheet (fixed 2026-10-01).
    Materials match how Noesis shows them (`mesh_textures`, observed): colour texture × `BaseColor`
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
