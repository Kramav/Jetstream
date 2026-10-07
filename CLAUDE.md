# CLAUDE.md — RE Engine Modding Tool

Node-based modding tool for Capcom RE Engine games. First target: **Resident Evil 4 (2023), "RE4R"**.
Long-term: AI-assisted asset generation plus optional REFramework runtime features.
**Current scope: M1 is done (polish goes on). M3 route 1 (replacing movies) is done (2026-10-06), their sound
packages too, and any game sound by id (Replace sounds; no longer a restriction); route 2 waits for its spike. Then M2 (REFramework scripting); see §10. AI texture generation (M4) isn't started.**

Source labels used below: **[official]** = official/authoritative docs, **[guide]** = community guide,
**[inferred]** = design decision or assumption from planning, **[TBD-spike]** = must be confirmed by a manual in-game test.

---

## 1. Hard rules

- Windows-first. Native desktop executable. **No web UI, no browser, no webview, no Python UI.**
- **The UI contains no logic.** All behavior lives in `core/`. The ImGui front end is expected to be
  replaced by a more polished native UI later; `core/` must not depend on ImGui or any UI library.
- Never commit game files. Gitignore at minimum: `natives/`, `*.tex.*`, `*.pak`, `*.mesh.*`, extraction caches.
- Never vendor third-party tools whose license does not allow redistribution (see §6).
- No AI texture generation before M4 (the orchestrator, M4's first part, was built early: §10 M4).
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
  - **Adopted for game audio (2026-10-06; user: no Wwise dependency):** libopus 1.6.1, libvorbis 1.3.7 (with libogg
    1.3.6), all BSD, core-private (`core/wwise.*`, §10 Story movies' sound); plus ww2ogg's codebook library (§6).
  - **Adopted for REFramework scripts (2026-10-07):** Lua 5.4.8 (MIT; a vcpkg override, REFramework embeds 5.4.3),
    core-private (`core/game_code.*`): the syntax check only, nothing runs.
  - GoogleTest verified but not used.
- Images (combining preview PNGs): **WIC**, built into Windows, used from core. No image library dependency.
- Build (Developer PowerShell for VS 2026, which sets `VCPKG_ROOT`):
  `cmake --preset default` → `cmake --build build` → `ctest --test-dir build --output-on-failure`
- **Release zip (2026-10-03):** preset `release` (`build-release`, triplet `x64-windows-static`, static CRT), then
  `cpack` in it → `remod-<version>-win64.zip`: both exes, `vcomp140.dll` (DirectXTex's OpenMP has no static form;
  shipped app-local), `profiles\`, `examples\texture_mod.json` (= `schemas/graph.v0.example.json`; the other example
  graphs hold personal paths, not shipped), `docs/USER_GUIDE.md` as `README.md`, **licences (2026-10-06):**
  `THIRD_PARTY_NOTICES.md` (every shipped component, its licence and source) and `licenses\<port>.txt` (each vcpkg
  port's `copyright` file, plus `third_party/ww2ogg/COPYING`); a new shipped dependency gets a row there and a port
  in the CMake `foreach`. `data\packed_codebooks_aoTuV_603.bin` (§6). Version: `project(VERSION)`, shown
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
  OverlayImage, ReplacePhoto, Preview, LuaScript (§10 M2), PackageMod; file steps CopyFile,
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
- PackageMod packages every linked texture, and every linked **other file** (a Path with a game path: Replace
  movie's movies) as it is. No texture is required (a mod can be movies only); nothing at all fails the run.
  Several previews are tiled into one `preview.png`.
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
- **Conditions (2026-10-03; user: "we need conditional nodes", a yes/no pin kind).**
  - **Nothing:** a `Value` can be nothing (`Value::nothing`, `why`; `NodeRun::nothing`). A block whose *required*
    input gets only nothing doesn't run: `NodeState::NotNeeded`, message "not needed: <why>", and it passes nothing on
    from every output (a list output: no items; the blocks repeated for it are Not needed too). `NodeRun::values`
    leaves nothing out (Package collects the rest); an optional input fed nothing is empty (`text` gives "", not the
    typed value). Previews do the same, so a branch not taken is known and `plan_changes` leaves its writes out. Run
    values show it as "nothing: <why>". A repeated block sums up as done if any item is, else Not needed.
  - **Why this shape:** the branch is cut *before* its steps, so the branch not taken never writes a file (a "choose
    A or B" at the end would have run both).
  - **Kind `PortType::Bool`** ("true" / "false"): into conditions and Text inputs (checkbox fields such as Replace
    existing), never paths; text into a condition is a typed yes / no (`NodeRun::condition`: true/false, yes/no, 1/0,
    empty = no). App: near-white triangle pins; a Value feeding one is a checkbox.
  - **Blocks** (utilities, pure): **If** (value + Condition, a checkbox when unlinked: a hand switch), **First of**
    (two inputs, `first` and `else`, the first with a value; inputs of one kind, `mixed_kinds` in can_connect and
    validate; `output_type` follows the first linked one), **File exists**, **Text matches** (`any_pattern`, `\` and
    `/` alike), **Not**. And/Or not built (no graph needs them yet).
  - **Streaming copy** (`StreamingCopy`, Source, pure): the base texture in, **full size** (the streaming copy if
    there is one, else the texture) and **streaming copy** (or nothing) out; game path `streaming/<path>`. Refuses a
    texture outside a natives tree (can't look) and a streaming copy itself. The graph: edit full size; Resize (Match
    size of: base) → Convert (original: base); Convert (original: streaming copy); both into Package. `resize_image`
    returns the image as is at its own size, so without a copy the base branch changes nothing.
  - App: Not needed blocks faded with a solid faint outline and "NOT NEEDED"; links into them or carrying nothing
    sparse dots (legend "Not needed"); step list "Not needed". API state "not needed", kind "condition"; `ai_note`s.
  - Checked: tests (If yes / no / by hand and the plan; if / else via Not and First of; Text matches; kind rules;
    streaming with and without a copy, a mixed folder, the two refusals). **Not checked by eye** (pins, Not needed
    look, the Value checkbox). No ready-made custom node is shipped (the library is the user's); the guide says to
    make one from the chain.
- **Built from blocks (2026-10-03; user: "lets start making blocks out of blocks").** Built-in custom nodes ship
  in `blocks/` beside `profiles/` (`builtin_blocks_dir`; installed into the zip), in the library's JSON format.
  `load_block_library` = those, then the user's library (a user's saved version of a built-in type wins); the app,
  `remod api` and `remod mcp` register it at start. Nodes panel: a "Built from blocks" folder (`is_builtin_block`),
  then "Custom nodes". Opening one (Edit custom node) shows its blocks; saving puts the user's version in their
  library.
  - **Convert with streaming copy** (`blocks/convert_with_streaming.json`): edited image, texture, streaming copy
    in; texture and streaming texture (or nothing) out. Inside: Splits, Resize image (stretch, Match size of the
    texture; titled "Shrink to the texture's size"), Convert × 2. Its Input pins' kinds come from the first link
    out of each Split (`wanted_type`), so the texture Split links to Convert's original before Resize's match.
  - **Streaming copy stays a single block** (not made of blocks): looking for the file and its refusals (outside a
    natives tree, a streaming copy itself) need clear messages that Cut text / File exists / If can't give. Its
    second output is labelled "streaming copy (if any)".
  - Checked: a test loads `blocks/` and runs the block with and without a copy (a converter that refuses a
    wrong-sized image, so the shrink is proven). **Not checked by eye** (the Nodes folder, opening it).
- **Part texture (`PartTexture`, 2026-10-03; §10 "Retexture part", step 1).** Source, pure. Mesh (in a natives tree)
  + Materials (patterns, as Mesh mask) [+ advanced Texture name, e.g. `*_nrmr*`] -> the texture those parts use, with
  its game path. Reads the mesh's material file only (`mesh_textures`, twice: first for the names it lists, then
  against just those files on disk; no 3 s index). Refuses: mesh outside a natives tree, no material matching (lists
  them), none of the matched having that texture, matched parts using different textures (lists which). Checked on
  Leon's `cha000_00`: `Pants_Mat` -> `cha000_00_LowerBody_ALBD`, `Shirts_Mat` -> `..._UpperBody_ALBD`.
  - **Materials dropdown (app):** a ▼ beside any `materials` field of a block with a `mesh` input lists the mesh's
    material names (core `mesh_material_names`; cached per mesh, "Read again" in the list). **Not checked by eye.**
  - **Recolour part** (`blocks/recolour_part.json`, made by `scripts/make_recolour_part.py`, which copies Convert with
    streaming copy into it; a test checks the copy matches): mesh, materials, hue / saturation / brightness /
    contrast -> texture, streaming texture, preview. Part texture -> Streaming copy -> Export (no file) -> Adjust
    colour -> Blend in mask (Mesh mask of the parts at the full size) -> Convert with streaming copy. Checked on Leon
    (`Pants_Mat`, hue 120): only the trouser panels changed (the result image looked at); 2 textures packaged.
- **Polish (2026-10-03):**
  - Pickers, Browser drops and "Use in graph" store a path inside the Game files folder as `{game}\...`
    (`with_game_token`; ignoring case and slashes), so the user's own graphs move between PCs too.
  - A custom node's input pin is typed like the field it feeds inside (`fed_input`, through Splits): its slider
    (range, format, reset to the pin's default), checkbox, list, picker and hint. Recolour part's hue is a slider.
  - Part texture's advanced *Material file* (`mesh_textures`' `material_file`): a costume variant's `.mdf2`. Checked:
    Leon's `cha000_00b.mdf2` gives `cha000_00b_LowerBody_ALBD`.
  - Test "every example graph runs" (needs `REMOD_GAME` = the extracted natives\STM; skipped otherwise): loads, validates
    and runs all examples in a copy with the real converter. Passes on the user's files.
- **Export image without a file (2026-10-03):** `PortSpec::field_optional`. Empty = a working copy in the run cache
  (named by the texture's bytes and the converter; reused), for graphs and built blocks nobody edits by hand. Still
  required when it goes to an Edit image (validate says so). Export image now makes its file's folder.
- **`{game}` (2026-10-03):** in any field, the Game files folder (`set_game_files_dir`, `fill_game`; `NodeRun::text`
  and thumbnails fill it in). The app sets it from its Game files setting (else the RE plugin's NativesPath), the CLI
  from the app's settings. ponytail: one folder for every game. So graphs (the examples, shared mods) run on any PC.
- **Loading fills missing fields with their initial values** (`migrate`), as add_node does: graphs a program wrote,
  and graphs from before a block gained a field. **A graph with no block positions is tidied on load**
  (`load_graph`'s `added_blocks`).
- **Previews know the game (2026-10-03):** `preview_pass` loads the graph's profile (it used a blank one, so blocks
  needing the natives root, Streaming copy and Part texture, were unknown before a run).
- **Files in folder into texture inputs lists only textures** (a pattern like `*.tex*` matched a Noesis leftover
  `x.texout.tga` in the real game folder).
- **Examples (2026-10-03; user: "a lot of example graphs for users"):** `examples/` (shipped), written by
  `scripts/make_examples.py` (no positions: tidied on open; `{game}` paths; sample `picture.png`, `logo.png`), listed
  in `examples/README.md`. **Read-only (user, 2026-10-03):** `save_graph` refuses the examples folder
  (`is_example_path`: `<tool>/examples` beside profiles), so the app, the API and an AI can't change one; the app's
  Save on an example is Save As (opening in Documents) and its title says so. Runs still write their outputs
  (`edits/`, `mods/`) beside them. Thirteen: hand edit, recolour, photo frame, logo, many textures, Recolour part, character
  edit with streaming, conditions filter, channels, if / else, backup, movie test card, movie edited by hand
  (2026-10-06). **All thirteen run** (the examples test: `--edited true`, the real game files, outputs in a scratch copy;
  example 13's edit stood in by the movie itself); the photo frame and logo results looked at as images. Not opened in
  the app (layout on open not checked by eye).
- Intermediate `.tex` files go to a per-run temp folder, deleted afterwards.
- Runs from `remod run --graph <file> [--noesis <exe>]` and from the app's Run button. Programs edit graphs through
  `remod api` (core `ApiSession`, §10 M4).

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
| ww2ogg's codebooks (`packed_codebooks_aoTuV_603.bin`, hcs64/ww2ogg commit 14ed9b0) | Wwise's Vorbis codebook library: checks libvorbis's setup against the replaced WEM's before writing (§10 story movies' sound) | BSD 3-clause (Xiph.org, Adam Gashlin) [official, its COPYING] | **Shipped (user, 2026-10-06):** `third_party/ww2ogg/`, installed to `data\`, licence in `licenses\ww2ogg.txt`. No ww2ogg code is used |
| libvorbis 1.3.7 / libogg 1.3.6, libopus 1.6.1 | Writing (and decoding) Wwise Vorbis / Opus WEMs (`core/wwise.*`) | BSD 3-clause [official] | **Adopted 2026-10-06:** vcpkg, core-private, static in the release zip; in the notices |
| Lua 5.4.8 | Syntax-checking REFramework scripts (`core/game_code.*`) | MIT [official] | **Adopted 2026-10-07:** vcpkg (override), core-private, static; in the notices |
| REFramework's SDK dump (`il2cpp_dump.json`) | The game's type names for checking scripts | The user's own file, made by REFramework (MIT) | Never shipped or copied; the user picks it |
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
  Fixtures are **local only**, located via env var `REMOD_FIXTURES`. Tests skip if unset. `REMOD_GAME` (the
  extracted natives\STM) runs the shipped examples on the real game files.
  Never commit fixture game files.

## 9. Open questions / spike results  [TBD-spike]

Fill these in from the manual spike before implementing the affected code.

**Finding a game's switch from the SDK dump (lesson, 2026-10-07: six probe runs to stop the player; user: "we were
just looking in the wrong space ... so we don't end up in this rabbit hole again").** Don't guess setters by name and
try them one per run. Instead:
1. **A flag that doesn't change when set is a copy, not the switch.** The game recomputes it each frame from somewhere
   else (`EnableOperation` came from the operation stop). Find what writes it: grep the dump cache
   (`%LOCALAPPDATA%\remod\game_code\*.txt`, T / F / M lines) for related names across managers and contexts, not
   just the class you started in.
2. **Look for the game's own request API first:** `request*` / `proc*` methods on a manager singleton, with an enum
   saying who and why (`CharacterManager.requestOperationStop(CharacterControlIndex, PauseLayer)`), used by the
   game's menus or events. Requests are usually per frame: send them every frame while needed.
3. **Ask what the game already does that looks like the goal** (its inventory and menus stop the player) and find
   that mechanism, rather than a generic-sounding one (input devices, command groups, behaviour flags).
4. **Every probe shows the game's own read-back on screen** each frame, so a run says at once whether a write took.
5. **Check the probe that ran is the one written:** its "loaded (run N)" log line and the installed file's time (a
   test once ran the previous version).

**Every spike gets a step-by-step guide (user, 2026-10-07: "whenever we have a spike, make a guide for doing it in the
spikes folder"):** `spikes/<name>.md`, written for the user (what it answers, time, what's needed, numbered steps in
remod and the game, taking it out, where the result lands, what to do if something goes wrong), listed in
`spikes/README.md` with its status.

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
      **Decided (user, 2026-10-03): if a texture has a streaming copy, the mod replaces both**, as best-practice mods
      do. A texture without one is replaced alone, as today. With one, the edit is made at the streaming copy's
      (full) size and both are written from it: the streaming copy, and the base at the base's size and mips.
      **Built 2026-10-03: the Streaming copy block** (§4 Conditions). The in-game check (§9 list) only confirms it's
      needed; it no longer blocks work.
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
- [ ] **Streaming copies (§9).** Does replacing a texture also need its `streaming/` copy? Work assumes yes (user,
  2026-10-03: if a texture has a streaming copy, replace both); this only confirms it.
- [ ] **Channel meanings (§9 table).** Are the inferred meanings right (e.g. `albd` alpha = dielectric vs metal,
  `nrrc` normal in G/A)?
- [ ] **Noesis converter only:** does the game accept a texture with more mips than the original (§9, parked)?
- [ ] **Replaced movies (§10 M3 route 1).** Example 12 (mva000's test card) and a logo's (mv7001: beeps): does it
  play; is the file's sound played (the logos); does a different length play fully (Same length off); which movie
  plays where.
- [ ] **Replaced game sounds (§10 Game sounds).** Example 14: is the intro's English narration three beeps (a
  bank's first part and a package together); does a sound of another length play fully (Same length off); and a
  sound embedded in a bank (no package).
- [ ] **Lua scripts through Fluffy (§10 M2 plan, step 1).** Does Fluffy install a mod's `reframework/autorun` files
  at the game's root, and remove them on uninstall?
- [ ] **Replaced movie sound (§10 Story movies' sound).** Example 12 also replaces mva000's sound packages: is the
  beep heard each second, and the original's music, dialogue and effects gone; does a WEM one zero byte longer per
  packet (libvorbis) play cleanly; with Same length off, does a sound of another length play fully without a bank
  edit.

## 10. Milestones after M1

**Order (user, 2026-10-03):** M2 REFramework scripting, M3 movies and cutscenes, M4 AI (was M2), M5 the REFramework
runtime (was M3). The old M4 (a C++ plugin for video) is part of M3.

- **NEXT (2026-10-07): the user runs the route 2 movie probe (`spikes/movie_probe.md`); AFTER that, finishing the
  cutscene (M3 route 3) is the next step (user: "finishing the cutscene is the next step AFTER I do this spike").**
  Route 3 so far (M3 "Route 3" below): format, runtime, probe and the Cutscene block built (2026-10-07); the
  cutscene probe ran the same day; the test cutscene (`spikes/cutscene_test.json`) then played in game with the
  camera held and cutting on time. **Freezing the player and hiding the HUD: built into the runtime 2026-10-07**
  (`GAMES.re4.freeze` / `hud`; `play` freezes and hides, `stop` (end, Stop, Reset Scripts) puts both back; failures
  reported in the menu and log, never stopping the cutscene), from `spikes/hud_freeze_probe.md` [game data, user]:
  - freeze: the player context's `get_HeadUpdater()` (`chainsaw.PlayerHeadUpdater`, a via.Behavior) `set_Enabled(false)`
    stops him when standing; switched off mid-move the camera acted oddly (with the game's camera). Cutscenes must be
    able to start while the player moves (user, 2026-10-07: "we can't have an automatic hook otherwise"). Tried and
    dropped: waiting for him to stand still (input not blocked meanwhile); probe run 3, Leon's behaviour tree flag
    `PlayerHeadUpdater.get_BehaviorTreeVariablesHub()` (`chainsaw.Accessor_Ch0CommonBTreeUserVariables`)
    `set_EnableOperation(false)` held each frame: no error, but `PlayerHeadUpdater.get_EnableOperation()` stayed true
    and the controls worked. **Now (user's suggestion): the head updater is switched off at once when a cutscene
    starts, before the camera and animations** (the camera is ours during it). **In game (user, 2026-10-07, on a
    second look): with a direction held while the cutscene starts, Leon keeps walking (the head updater off keeps its
    last command).** Probe run 4: `share.hid.DeviceSystem` (singleton) `set_ActiveCommand(share.hid.CommandTag)`
    without the `Character` group (tags None, System, Character, Develop, All), held each frame before UpdateBehavior:
    set and held (4294967295 -> ...93; tags None 0, System 1, Character 2, Develop 0x80000000, All 0xFFFFFFFF), but
    Leon still moved and the camera panned: no effect. Run 5: ActiveCommand = None (all game controls), and the head
    updater off plus `changeMotion(1000, 160)` (his idle) at once: **neither changed anything** (user, 2026-10-07).
    REFramework's FreeCam "Disable Movement" has the same problem (user). **Why EnableOperation never changed: it's
    copied each frame from the operation-stop system** [dump]: `chainsaw.CharacterManager.requestOperationStop(
    CharacterControlIndex target, character.PauseLayer layer)` (+ `procOperationStop`, an `OperationStopRequestList`
    of `OperationStopInfo`), read back as `CharacterContext.get_OperationEnable()` / `get_OperationStopState()`;
    targets None, Player_1..10; layers None, Self, Inventory, Tutorial, File, Map, Craft (the game's menus). Probe
    run 6 sends it every frame before UpdateBehavior, layer by layer. **Run 6 works (user, 2026-10-07): every layer
    stops Leon even with a direction held; the runtime uses Self** (OperationEnable / EnableOperation read false while sent, true again
    after). **The runtime now uses it** (`GAMES.re4.hold_player`, every frame of a cutscene before UpdateBehavior;
    control returns when it's no longer sent); the head-updater freeze is gone. Not yet seen in a cutscene. (The
    user has since updated REFramework again: hooks are available if ever needed.)
    **Plan (user, 2026-10-07; order theirs):** the operation stop (run 6), then 2. a read-only watch probe (log which
    player / input values change when the game's own events, ladder, door kick, interaction, take control; then set
    the same), 4. a method hook on the input update (after checking the updated REFramework's anti-tamper status in
    its log), 3. hold him in place each frame (position + idle). The menu shows
    "Leon's animation now" (bank, motion, frame of end; `current_motion`) for writing `motions`. Still to see: a
    different animation (not his idle) played by a cutscene while he's frozen. Untried: `share.hid.DeviceSystem.set_HIDInputMode`
    (values unknown, might block REFramework's keys too).
    `onChangeEventPause(true)` did NOT freeze him.
  - HUD: `chainsaw.OptionManager` singleton `getCurrentOptionValue` / `setCurrentOptionValue(OptionID.DisplayUI, n)`:
    0 nothing, 1 and 3 crosshair and red damage edges, 2 everything (the user's). Ponytail: it's the player's saved
    option; a crash mid-cutscene could leave it at 0 (set it back in the game's options). The game's own
    `chainsaw.GuiDrawController.setStatus(DrawOffAttribute.EventHide, bool)` has no path to its instance in the dump
    outside `HighwayGuiManager`.
  - Not yet seen together in a cutscene, nor whether changeMotion animates him while frozen.
  Still to do: a cutscene from the
  user's own recording (F10), more actors, triggers (M5), sound. M2's step 5
  (settings-window preview) is skipped for now. M2 steps 1-4 are built; step 2
  waits for a real RE4R dump from the user to be checked on. The survey is done (M2 "Survey"). Before it: M3 routes 1 and 2, below: replace a game movie, then play a movie when we choose.
  **Route 1 done (user, 2026-10-06: "then we can consider route 1 done", once sound was re-encoded).** Its in-game
  check is in §9's list. Route 2: prepared, see M3 "Prepared".
- **M2: REFramework scripting (user, 2026-10-03).** Help users make script mods for REFramework (Lua in
  `reframework/autorun`). Survey: REFramework (MIT) is the runtime and has no packaging conventions; EMV Engine (MIT,
  RE4R through the SILVER fork) is a shared Lua library; REE Content Editor (MIT, very active) edits and patches RE
  Engine files, no runtime scripting. Steps, cheapest first (superseded by the Plan below; kept for the reasoning):
  1. Packaging: Package for Fluffy takes Lua scripts (`reframework/autorun/<Mod>/`, at the game's root, not under
     natives) and marks the mod as needing REFramework (§3: the lowest tier the graph needs).
  2. Game knowledge: REFramework's SDK dump (types, methods, fields) searchable in the Browser, so blocks and the AI
     check a hook's names before the game runs. The dump's format and how it's made: Survey below.
  3. Trigger -> action blocks: a mod is data, run by the one generic runtime (M5). Each trigger needs an in-game spike
     for its hook point.
  4. AI-written Lua for the rest (`claude -p` / MCP), checked against step 2, packaged by step 1.
  **Decided (user, 2026-10-07, replacing that day's "I can learn lua"): Claude writes the Lua ("i hate writing
  code").** The user describes the mod, tests it in game and says what's wrong; the tool makes the AI reliable
  (game names looked up and checked, errors fed back) and one-click. Lua, not C# (re-engine-mcp's polished loop is
  C#): a Lua mod runs on the stable REFramework every player has; C# needs the nightly and .NET on every player's
  PC. A separate script editing mode (a script graph, like a custom node) is only needed if scripts become mostly
  blocks; not planned. Can't be checked before the game: whether a hook does the right thing (the user plays).
  - **Survey (2026-10-07; licences and activity from GitHub's API; Nexus and Patreon pages couldn't be fetched):**
    | Candidate | Covers | Gap | Licence | Maintained |
    |---|---|---|---|---|
    | REFDumpFormatter (kagenocookie) | `il2cpp_dump.json` -> LuaCATS definitions of the game's types (or C#), per class or per namespace; also ships definitions of REFramework's API (`Includes/lua/REFramework`: re, sdk, imgui, draw, fs, json, ...) and the `.luarc.json` setup | .NET 8; tested on DD2 / DMC5, not RE4R; its API files last updated 2025-01 (imgui partial) | MIT | v1.6.6, 2026-03 |
    | REFramework's Dump SDK (DeveloperTools > ObjectExplorer) | Writes `il2cpp_dump.json` to the game folder: an object keyed by full type name, with `parent`, `flags`, `fields`, `methods` (`params`, `returns`; keyed name + index), `properties`, `RSZ`, `reflection_*` [official, ObjectExplorer.cpp] | Needs the running game; "will probably crash during the IDA SDK dump step but that's fine" (REFDumpFormatter's README) | MIT | Nightly 2026-09; stable v1.5.9.1 2025-03 |
    | REasy's `dump_il2cpp.ps1` (seifhassine) | The same dump with no REFramework, reading the running game's memory only; type-database versions 66, 67, 69-84 | Needs the running game | MIT | Pushed 2026-09 |
    | re-engine-mcp (praydog) | MCP server: an AI searches 100k+ types, reads / writes fields, calls methods in the running game | Made for C# plugins (REFramework nightly + .NET 10), not Lua | MIT | 2026-03 |
    | REFramework-LLS, REFrameworkLuaTypedef (infinitY0369) | Definitions of REFramework's API only | Stale (2023, 2024, pinned to an old REFramework commit) | None | No |
    | "REFramework Lua API" (Nexus, RE4R mod 2670) | A wrapper with completion and checked errors, generated from REFramework's code | Beta, not the whole API | Unknown (page not fetched) | ? |
    | Lua Language Server / EmmyLua analyzer | VS Code's engine; a command-line `--check` | Can't check names scripts pass as strings (below) | MIT / MIT | 3.19.1 / 0.25.1, 2026-08 |
    | Lua 5.4 (REFramework embeds 5.4.3 with sol2) | Its parser alone (`luaL_loadbuffer`) is a syntax check | - | MIT | - |
    Nothing packages Lua mods or previews a settings window. **The gap:** REFramework reaches game code by strings
    (`sdk.find_type_definition("chainsaw.X")`, `:get_method("m")`, `sdk.hook`), which no language server checks: a
    typo or a name a game update removed fails only in game. A name check against the dump is ours to build.
  - **How REFramework runs scripts [official, ScriptRunner.cpp / REFramework.cpp, read 2026-10-07]:** at start and on
    Reset Scripts it runs every `.lua` directly in `<game>\reframework\autorun\` (not subfolders; each can be
    unticked in its menu); `require` finds `autorun\?.lua` and `autorun\?\init.lua`, so a script's own modules go in
    `autorun\<name>\`. **No reload on file change:** only Reset Scripts (or its menu's checkboxes). The log is
    `<game>\re2_framework_log.txt` (every game); the last script error also shows in the ScriptRunner menu. Lua errors
    reach the log only with "Log Lua Errors to Disk" on (off by default; step 4 below).
  - **Plan (user, 2026-10-07: AI writes the code, "as easy as possible").** Each step usable alone, cheapest first:
    1. **Script block, packaging, Test in game.** `LuaScript` (Source): a `.lua` file (its folder of modules, if
       one has its name, goes with it) -> a Path with a game-root path, `reframework/autorun/<name>.lua`; into
       Package's other file, written as `<Mod>/reframework/autorun/...` beside `natives/`. A mod with one is tier 2
       (`requires_reframework`; "Needs REFramework" added to modinfo's description). **Test in game** (block button,
       app): copies it into `<game>\reframework\autorun\` (the game's folder: a new setting) and says to press Reset
       Scripts; **Remove from game** undoes it. Through the API, writing into the game's folder needs approval.
       **Built 2026-10-07:** `LuaScript` (pure; list output "script files": the `.lua`, then `<stem>\`'s files by
       path), core `script_files`, `is_game_root_path` (`reframework/...` -> `<Mod>/reframework/...` in
       `package_path`), `build_package` appends "Needs REFramework." to the description, `install_in_game` (refuses a
       folder without `dinput8.dll`; overwrites) / `remove_from_game` (the files, then emptied folders, never
       autorun), Settings `game_dir` (the Pipeline panel's "Game folder"), the block's two buttons (Use layout; status
       line says to press Reset Scripts). No manifest.json is written (§9's open question; `ManifestV0` still
       unused). Not done: Test in game through the API / MCP (step 3 needs it, with the guard). Checked: tests
       (layout and modinfo, install over and remove, a graph run into Package, a missing script). **Not checked by
       eye:** the buttons, the Game folder field.
    2. **The game's names, as tools for the AI.** The user dumps once per game update (REFramework's Dump SDK; the
       guide says how) and picks `il2cpp_dump.json` in Settings (never copied). Core reads it into a compact index
       (types, parents, fields, methods with params and returns; cached by the dump's write time). Core
       `check_lua`: Lua 5.4's parser (vcpkg `lua`, MIT, core-private) for syntax, then the name check: string
       literals given to `find_type_definition` / `get_managed_singleton` / `typeof` / `create_instance`, and
       `:get_method` / `:get_field` on one of those (or a local set from one in the same file), against the index:
       unknown type, unknown member with the nearest names, a signature that doesn't match. ponytail: names built at
       run time aren't checked. MCP tools `search_game_code` and `check_script`; CLI `remod check-lua`; the Script
       block shows the result, Package warns.
       **Built 2026-10-07** (`core/game_code.*`):
       - `load_game_code`: nlohmann SAX over the dump (`_wfopen_s` FILE*), keeping only `parent`, `fields` (type,
         `Static` in flags) and `methods` (name = key minus its `id`, params {type, name}, returns, static); RSZ,
         addresses, properties and reflection skipped as they stream past.
       - Cache: text lines (T / F / M, tab-separated) under `%LOCALAPPDATA%\remod\game_code\<hash of path>.txt`; its
         first line holds the dump's path, size and write time, so another dump reads again; a bad cache is ignored.
       - Lookups as REFramework's [official, RETypeDefinition.cpp]: name, then `name(T1, T2)` prototype, the type then
         its parents; generic definitions (a `!` in a type) skipped. `X[]` counts as known when `X` is.
       - `game_code()` global (Settings `sdk_dump`, "SDK dump" in the Pipeline panel; the CLI and `remod mcp` read the
         app's setting, `check-lua --dump` overrides), reloaded when the file changes. ponytail: one dump for every game.
       - `check_lua`: syntax by `luaL_loadbufferx` (Lua **5.4.8**, a vcpkg override: the baseline's 5.5 refuses valid
         5.4, e.g. assigning a for-loop variable); names by our own Lua tokenizer (comments, long brackets, escapes) and
         a pattern scan: sdk.find_type_definition / get_managed_singleton / create_instance / typeof("T"), then
         `:get_method` / `:get_field` on a type, `:call` / `:get_field` / `:set_field` on an object, directly or through a
         variable assigned from one (`local t = ...`; reassigned to anything else: dropped; `a.t = ...`: not followed).
         Suggestions: up to 3 names within edit distance max(2, len/4), ignoring case; a wrong prototype lists the
         method's real ones.
       - `search_game_code`: words (all, ignoring case) in type names, then in `Type.member`, types first, sorted; a
         query naming a type exactly lists its own members with signatures.
       - API ops `game_code`, `check_script` (file or text; `names_unchecked` says why); MCP tools `search_game_code`,
         `check_script`, and the instructions tell the AI the autorun rules, to read the REFramework book, to look up
         every name and to check until clean.
       - Lua script block: no longer `pure` (a first dump read takes a while; previews give its files only); its run
         checks every `.lua` it packages: a syntax error fails it (`file:line: syntax: ...`), a name is a warning
         ("N problem(s): see the warnings"); its message says when names weren't checked and why.
       - Checked: tests (synthetic dump in ObjectExplorer's layout: junk skipped, ids stripped, statics, lookups through
         parents and prototypes, bad / empty / missing dump; cache used, refused when cut short, re-read on a new dump;
         search; syntax messages and lines; names: types, methods, fields, prototypes, singletons, create_instance,
         comments and strings skipped, reassignment; API and the block's warn / fail), `remod check-lua` by hand.
         **Real RE4R dump (user, 2026-10-07):** 1.03 GB; the first read (Debug CLI, `check-lua` on the cutscene runtime:
         no problems) took 307 s; the cache (112 MB of text) still takes ~100 s to read (Debug CLI): too slow for the app,
         to fix (a Release build, or a binary / lazily read cache). Search only matches names (not field types): the
         cache file can be grepped by type. Tests leave 480-byte caches in the user's `%LOCALAPPDATA%\remod\game_code`
         (to fix: a temp cache dir in tests). Not yet measured: memory; whether nested types' names match what
         find_type_definition takes. **Not checked by eye:** the SDK dump field.
    3. **Write with AI** on the Script block: "What should it do?" and **Write it** run `claude -p` (as Run program
       does) with `remod mcp` attached and instructions (REFramework book pages, the checks, the autorun rules
       above); it writes the `.lua`, runs `check_script` until clean, and the block shows the result. **Ask for a
       change** for each round after. The user never opens an editor (the file stays plain Lua, editable by hand).
       **Hand editing always stays (user, 2026-10-07: "always have a way for human editing ... sometimes i prefer to
       handle the files themselves").** **Built 2026-10-07** (core `script_ai.*`, app):
       - Block (Use layout): "Ask Claude to write it:" / "Ask Claude for a change:" box (state `ai_request`), **Write
         it with Claude** / **Ask Claude** (one at a time; "Claude is working..."), **Open in editor** (advanced field
         Open with, else Windows' program for .lua), Claude's note under it (state `ai_notes`, with any problems still
         left), then Test in game / Remove from game. The field Script may name a file that doesn't exist yet.
       - `find_claude`: claude.exe / claude.cmd on PATH, `%USERPROFILE%\.local\bin\claude.exe`, else the newest VS Code
         extension's `resources\native-binary\claude.exe` (this PC: 2.1.285). `find_remod_cli`: beside the app, else
         `..\cli` (build tree).
       - `ask_claude`: writes `%LOCALAPPDATA%\remod\ai\request.md` (the script's path, "read it first, keep the user's
         edits" or "write it from scratch", its modules folder, the request, the autorun rules, the REFramework book,
         search_game_code for every name, check_script until clean, comments for a person, settings via re.on_draw_ui
         and json, "don't write files", answer = note then one ```lua block) and `mcp.json` (remod.exe mcp); runs
         `claude -p "Follow the instructions in request.md..." --output-format json --tools Read,Grep,Glob,WebFetch
         --allowedTools Read Grep Glob WebFetch(domain:cursey.github.io) mcp__remod__search_game_code
         mcp__remod__check_script --permission-mode dontAsk --mcp-config <mcp.json> --strict-mcp-config --add-dir <the
         script's folder>` in that folder (a .cmd through cmd.exe, refusing `&|<>^%!"`), 15 min limit; takes the last
         JSON line's `result` (`is_error` -> its words), `split_answer` (last ```lua block, else last fenced one; words
         before it = the note) and `check_lua` on the script. Claude writes nothing.
       - **Only on a click, never per use (user, 2026-10-07: "do not automate an api call for claude ... disable that
         if you are charging me per use").** Nothing calls Claude by itself (not a run, a save, an error or a test:
         tests use a stand-in). `per_use_billing`: ANTHROPIC_API_KEY / CLAUDE_CODE_USE_BEDROCK / _VERTEX in the
         environment, or an apiKeyHelper or those in `%USERPROFILE%\.claude\settings.json`'s env, and ask_claude
         refuses before starting anything. The user is on Claude Pro (no key): a call counts toward the plan's limits,
         so no price is shown (Claude Code's `total_cost_usd` is a list-price estimate, not a charge).
       - `save_script`: the old file to `<name>.lua.bak`, then the new; if the file's write time changed since asking
         (`file_stamp`: edited, or made, by hand meanwhile), the user's is left alone and Claude's goes to
         `<stem>_claude.lua`. Status line: what happened.
       - Closing the app while Claude works doesn't wait (ponytail: its future is leaked; Claude Code finishes alone).
         Only the main script is written; modules are read, not changed.
       - Checked: tests (split_answer; save new / .bak / edited-meanwhile; ask_claude through a stand-in claude.cmd
         answering as `-p --output-format json` does, its request file, an is_error reply, no reply, no request, no
         Claude). **One real run** with these flags (Claude Code 2.1.285, Opus 5.5): it called search_game_code ("no SDK
         dump set") and check_script (clean), no permission denials, a correct hello-window script, 15 s, $0.12.
         **Not checked by eye:** the block's box, buttons and note.
    4. **Errors from the game back to the AI.** After Test in game, the tool reads `re2_framework_log.txt` (from
       where it was at Test in game) and shows the script's errors on the block; **Fix it** hands them to Claude (a
       button: never sent by itself).
       **Built 2026-10-07.** REFramework's side [official, ScriptRunner.cpp / .hpp, REFramework.cpp, read 2026-10-07]:
       script errors go to the log (`spdlog::error`, via `spew_error`) **only with ScriptRunner's "Log Lua Errors to
       Disk" on, off by default** (`<game>\re2_fw_config.txt`: `ScriptRunner_LogToDisk=true`); the log is opened with
       truncate, so it's **emptied each game start** (not on Reset Scripts); lines are spdlog's default
       `[date time] [REFramework] [error] <message>`, a message's further lines (a traceback) not starting with `[`;
       a script that fails to load also shows a MessageBox in game.
       - Core (package.*): `framework_log`, `lua_errors_logged` (that config line), `script_errors_in_log(log, from,
         script)`: error entries from byte `from` (0 if the log is shorter now: the game restarted) that name
         `<stem>.lua` or `<stem>/` as a word (after a separator, space or quote: `other_my_mod.lua` isn't it), each once
         with a count (on_frame errors repeat every frame), its line from `<stem>.lua:<n>` (a module's line isn't).
         "Unknown error in on_frame" names no script: left out.
       - App: Test in game records the log's size per block (`State::log_from`) and, if logging is off, says to turn
         on Log Lua Errors to Disk once. **Game errors** (button) reads since then (since the game started, without a
         Test in game this session), shows them in amber under the block (`State::game_errors`, not saved), and the
         status line says how many or why none. **Fix with Claude** (only when there are errors; one click) puts
         "Fix these errors REFramework logged in game:" and them in the request box and asks, as Ask Claude does.
         Nothing reads the log or asks Claude by itself.
       - Checked: tests (config on / off / missing, a log with an old error, a repeated error with traceback, another
         script's, a module's, a warning, an unattributed error; from Test in game, from the start, after a restart).
         **Not checked:** a real REFramework log (the error text's exact form), by eye.
    5. **Settings-window preview** (last): the script's `re.on_draw_ui` run in the app against stub REFramework
       tables (`imgui.*` -> the app's ImGui, `json` / `fs` in a scratch folder, `sdk` / `re` doing nothing), so its
       menu shows without launching the game.
    - **Optional beside it: re-engine-mcp** (the user's PC only: REFramework nightly + .NET 10; the finished Lua mod
      doesn't need it): Claude looks around the running game ("which field holds Leon's health?") before writing a
      hook. Likely also the way to do M3 route 2's spike (calling the movie types live) [inferred, untried].
    - **In game (§9 list):** Fluffy installs a mod's `reframework/` files at the game's root [guide, unconfirmed].
    - Dropped (only help a human typing): generated VS Code definitions, `.luarc.json`, name completion, Copy hook
      snippets. Not planned: trigger -> action blocks (step 3 of the old list) until scripts show what repeats.
- **M3: Movies and cutscenes (user, 2026-10-03: "scripting our own cutscene").**
  - **Prepared (2026-10-03):**
    - Core `read_mp4_info` (core/movie.*): an MP4's video size, length, frame rate, codecs, audio, from its boxes.
      CLI `remod movie-info --file <f>`. On the game's files: mva000 3840x2160, 60.9 s, 30 fps, avc1, no audio;
      mva300 1440x1080, 80.1 s, avc1 + mp4a; mv7001 4K 29.97 fps with audio; mva402 328x408 silent; the 38-byte
      stub "isn't an MP4". Tests: synthetic boxes (index after the data, silent, the stub, cut short), and the real
      movies with `REMOD_GAME`.
    - Packaging already takes a movie: `build_package` copies a file that isn't a texture as it is, at its game path
      (test: both copies of mva000 at `streaming/_chainsaw/movie/mv/mva000/`). The CLI's `package` takes one file;
      the graph's Package takes them through its **other file** input (2026-10-06).
    - Route 2's probe: `spikes/movie_probe.lua` (REFramework, read-only): logs the methods and fields of the movie
      types the game's exe names (`via.movie.Movie`, `MovieManager`, `MovieResource`, `MovieResourceHolder`,
      `MovieEntry`, `MovieContext`, `chainsaw.FullScreenMovieGui` and its `OpenParam` / `CloseParam`) to
      `reframework/data/remod_movie_probe.txt`; with WATCH_CALLS on, also which of their methods the game calls while
      a movie plays.
  - **Replace movie block: built 2026-10-06** (`ReplaceMovie`, Transform; core `encode_movie`, Media Foundation).
    - In: Game movie (either copy, inside a natives tree: the game path), Your video (optional).
    - Out: new movie, and 1080p copy (`<id>_fhd`, or nothing). Each is encoded H.264 High at its original's size,
      frame rate (whole or x/1.001) and average bit rate (unconstrained VBR). Hardware encoder allowed.
    - The video: the source reader's video processor scales it, keeping the shape (black bars, observed). Frames
      are repeated or dropped to the frame rate. Its length is kept (logged when it differs).
    - **Sound (2026-10-06; user: "we need to reencode the sound accordingly as well"):** only where the original has
      sound in the file (mv7000 / 7001: AAC LC 48 kHz stereo ~380 kbps; mva300 / 301: the same format at ~2 kbps,
      likely silence) [game data]; the other 25 have no sound track (their sound is in the sound bank) and their
      replacements get none either. The track: AAC (Windows' encoder: 44.1 / 48 kHz, 1 / 2 / 6 channels, 192 kbps)
      at the original's rate and channels, written alongside each frame and exactly as long as the picture. From the
      video's first audio track through a second source reader asked for PCM at that rate and channels (Windows
      converts: 44.1 kHz mono -> 48 kHz stereo checked, pitch kept: a 440 Hz tone measured 440), cut or padded with
      silence; a video without sound gives silence; a test card a 1 kHz beep for the first tenth of each second.
      `encode_movie` returns what the sound is, shown in the block's message. `read_mp4_info` reads the
      AudioSampleEntry's rate and channels (`MovieInfo::audio_rate` / `audio_channels`; `movie-info` prints them).
      ponytail: the audio track's own start offset and gaps are ignored (decoded sound is taken as continuous).
    - No video: a GDI test card (the id, "s / total s"), as long as the original. Same frame count as the game's
      (mva000: 1828 and 1827).
    - The run cache: keyed on the files' path, size and write time (not their bytes) and "mf2". `prune_cache` keeps
      every file just written; ponytail: one 512 MB limit, a 4K movie can push every texture out.
    - Example `12_replace_a_movie` (the mva000 test card): 4K in 17 s, 1080p in 5 s, on this PC.
    - Checked: tests (card, video scaled and retimed to 29.97, the block both copies, without an fhd, the stub
      refused, a re-run unchanged, Package with nothing). ffprobe on the results: profile, level (5.1 / 4.0), size,
      fps and frame count match the originals. Frames looked at as images (card; mva402 fitted into 4K).
    - **Same length as the original (user, 2026-10-06: "do the videos need to be the same length?"), on by default:**
      a longer video is cut, a shorter one holds its last frame (`encode_movie`'s `same_length`). Why the default:
      `mva000.pfb` names the `.mov`, a render texture and `snd_cont_mva000.user` (sound container); neither it,
      `mva000.user` nor `movieloadtable.user` holds the length (searched for 60.93 s / 1828 frames as float, double
      and int) [game data], so the length is the MP4's, and the sound bank is timed to the original. Off: the
      video's own length; whether the game plays that fully is still the in-game question.
    - **Editing a movie by hand (user, 2026-10-06):** `ExportMovie` (Source) copies the full-size movie (`movie_files`:
      either copy picked gives `<id>` and `<id>_fhd`) to its Video file, kept between runs unless from another movie
      (state `exported_from`, `fresh_exports`). `EditVideo` (manual): a video editor renders a new file rather than
      saving over its source, so it has a **Your edit** field (default `<video>_edited.mp4`) and passes that on;
      done with no such file fails the run. Manual steps are found by `NodeSpec::manual`, not by type
      (`for_editing`, validate's required export file, `set_edit_done`, the API's `edit_done`); `edit_is_done` is
      shared with Edit image. Example `13_edit_a_movie_by_hand` (mva402: small, so the examples test stays quick; the
      test stands in the movie itself as the edit).
    - Checked (2026-10-06): tests (same length cut and held; the export / wait / done-without-edit / encode / own
      length / another movie flow), example 13 from the CLI (a 5 s edit came out 15 s). **Not checked by eye:** the
      Edit video block and its YOUR STEP card in the app.
    - **Movie preview (user, 2026-10-06: "a movie preview in use mode, where the mesh and previews live"):** core
      `MoviePlayer` decodes on a thread of its own (Media Foundation source reader, RGB32 scaled to fit 1280 px,
      Lock2D for the row order), opens paused on the first frame; play / pause / seek; `take` hands the newest frame.
      Game movies by their `.mov.1.x64` name: `open_reader` (shared with `encode_movie`) gives Windows the byte stream
      as `video/mp4` for names it doesn't know, after `read_mp4_info` (so the stub says to use the streaming one).
      App `MovieView` (app/movie_view.*): a dynamic D3D11 texture, Play / Pause, a time bar (dragging seeks),
      `zoom_area`. The viewer's fourth mode, "Movie###viewer" with a close X: a movie clicked in the Browser
      (`FileKind::Movie`: any name with `.mov.` in it, as the game's `<id>.mov.1.x64`, and mp4, m4v, mov, wmv, avi,
      mkv), or a block's **Preview movie** button (Use layout; any block whose last run's file is a movie). **Fixed
      2026-10-06 (user: "not occurring for me"):** the first rule wanted digits only after `.mov.` (as `.mesh.<n>`),
      so the game's movies were Other and clicking them did nothing; the tests had covered the player, not the name. Picking a texture or mesh, a block's picture, or Build
      layout stops it. Measured: the 4K intro plays at 30 fps in Debug (software decode, 59 frames in 2 s).
      ponytail: no hardware decoding and no frame dropping (a slower decode falls behind the clock); no sound.
      Checked: tests (first frame paused, fitted size, seek, play to the end, the `.x64` name, the stub; the real
      intro with `REMOD_GAME`). **Not checked by eye:** the viewer, its controls, the block button.
    - **Story movies' sound: built 2026-10-06** (in the game's sound packages, not the file: §10 Future work, "Story
      movies' sound"). Not done: other games' movie folders.
  - **Route 1 spike (the user's, in game):** build example 12 (or Replace movie on another movie), install the zip
    through Fluffy. Find out: does it play; a different length; a silent movie's sound (its Wwise container); which
    movie plays where (mva000 is likely the intro: 4K, 61 s).
  - **Route 2 spike:** guide `spikes/movie_probe.md` (a Lua script block on `spikes/movie_probe.lua`, Test in game,
    a movie played, Remove from game; optionally the SDK dump last). WATCH_CALLS is on by default since 2026-10-07
    (one run gives both parts). Result: `<game>\reframework\data\remod_movie_probe.txt`, read from the Game folder.
    It decides whether Lua can start the game's movie player on a new `.mov`, or a C++ plugin is needed. The SDK dump
    alone gives its first part (the movie types' methods and fields).
    **Run 2026-10-07 (user), then the game crashed on New Game.** Results in the game's
    `reframework\data\remod_movie_probe.txt` [game data]:
    - `via.movie.Movie` (a Component) has `set_ResourceURL` / `get_ResourceURL`, `set_ResourceHandle`, `prepare`,
      `play`, `pause`, `stop`, `reset`, `seek(position_usec)`, `jumpToTime`, `get_Ready`, `get_DurationTime`,
      `get_CurrentVideoTime`, `get_MovieTextureResource` / `set_MovieTextureResource` (a render target), `fillTexture`.
      `via.movie.MovieResource` and `MovieEntry` don't exist in RE4R.
    - `chainsaw.FullScreenMovieGui` (a GUI behaviour): `setup`, `changeStep(next)`, `CurrStep`, `_OpenParam`
      (`CallbackOnReady`), `_CloseParam` (`CallbackOnClosed`), `_IsReady`, `setAlpha`.
    - While the startup logos played, the game called `MovieManager.update` / `draw`, `Movie.play`, `reset`,
      `fillTexture`, the state and size getters, `FullScreenMovieGui...get_Valid`.
    - So a Movie component can be pointed at a resource and played from Lua [inferred: methods exist]; still open:
      where its picture is shown (a GUI or render target), and making one for our own `.mov`.
    - **The crash [inferred, strong]:** the user's REFramework was **v1.5.9 + 7 (built 2025-03-05)** on a game exe
      of 2026-04-08; its log: `[IntegrityCheckBypass]: Could not find sussy_constant usage!` (and 6 more patterns not
      found), so the game's anti-tamper check stays on, and code hooks (WATCH_CALLS hooked ~60 methods, 5 failed with
      "unknown exception") can trip it. No crash entry or dump; the log just stops. **The user stays on REFramework's
      latest release, v1.5.9 (user, 2026-10-07: "I dont want a nightly, just use the most recent release for re4"),
      so our scripts and spikes don't hook game methods (`sdk.hook`) unless a feature can't work without it;
      REFramework's own callbacks (`re.on_frame`, `re.on_pre_application_entry`, which ran fine that session) are the
      way in.** Write with Claude's instructions say so too. WATCH_CALLS is off by default again (not needed again).
      Not yet confirmed: New Game without the probe on v1.5.9 (if that crashes too, REFramework itself is the cause).
      **Later the same day the user updated REFramework anyway** (dinput8.dll 2026-10-07 13:27, 23 MB vs v1.5.9's
      12.8 MB: a newer build; its version and integrity bypass show in the log at the next game start). Our scripts
      stay hook-free all the same: players of the user's mods may be on v1.5.9.
  - **Route 3: scripted real-time cutscenes (started 2026-10-07; user: "long term id like this to extend to re9").**
    - **Survey (2026-10-07):**
      | Candidate | Covers | Gap | Licence | Maintained |
      |---|---|---|---|---|
      | EMV Engine, SILVER fork (RE4R) | Any animation on any character (`layer:changeMotion(bank, motion, frame, blend, InterpolationMode, InterpolationCurve)`, `set_Frame`), the game's cutscene camera (`EventCameraController`, `via.motion.ActorMotionCamera`), spawning; RE4R player = `chainsaw.CharacterManager:getPlayerContextRef():get_BodyGameObject()` | No timeline: its "Sequencer" chains BehaviorTree moves on hotkeys | MIT | Pushed 2026-09 |
      | REFramework's free camera | Free movement, FOV, pause, HUD off | No recording or paths | MIT | Nightly 2026-09 |
      | Otis_Inf photomode (RE4R) | Camera paths with keyframes | Paid, closed: can't be built on | Commercial | Yes |
      | Custom Fixed Cameras (Nexus 6231, RE4R) | Saved camera angles switched on area triggers: Lua camera override works in RE4R | Fixed shots, no timing | Unknown (page not fetched) | ? |
      | Noclip / trainer scripts (e.g. VIPO777's, MIT) | Camera driven each frame from `re.on_pre_application_entry` | **RE9 scripts** (user caught it): the pattern is engine-wide, the hook, class names (`app.*` vs `chainsaw.*`) and overloads per game | MIT | 2026 |
      Nothing data-driven for RE4R: ours to build, on EMV's proven calls (reference; credit if code is copied).
    - **Decided:** a cutscene is data (`schemas/cutscene.v0.example.json`), played by one generic runtime
      (`runtime/remod_cutscene.lua`); everything per game is in the runtime's `GAMES` table keyed by
      `reframework:get_game_name()` (RE9 later = an entry there, the format unchanged). Sound (Wwise) last.
    - **Format v0:** `schema_version`, `name`, `length` (s), `start.key` (F1-F12 but F10), `letterbox` (bar height,
      fraction of the screen), `camera` [{`t`, `position` [x,y,z], `rotation` [x,y,z,w], `fov`, `ease`: smooth
      (default) / linear / cut, how the camera arrives at that key}], `subtitles` [{`t`, `until`, `text`}], `fades`
      [{`t`, `until`, `from`, `to`: black's opacity}], `motions` [{`t`, `actor` ("player" only), `bank`, `motion`,
      `frame`, `blend`}].
    - **Runtime (built 2026-10-07):** loads `reframework\data\remod_cutscenes\*.json` (`fs.glob`, `json.load_file`);
      REFramework menu: Play / Stop each, Reload, recording; a cutscene's start key toggles it; camera set in
      `GAMES[g].camera_hooks` (RE4R: see the probe run below), position lerped, rotation
      slerped (`Quaternion.new(w, x, y, z)` [official, book]), FOV lerped and restored after; letterbox, subtitles
      (`draw.text`, fixed font, left at 10%), fades (`draw.filled_rect`, colour 0xAABBGGRR) in `re.on_frame`; motions on
      the player through EMV's `changeMotion`; time from `os.clock` (else frames / 60). **F10 records** the camera as a
      key into `remod_cutscenes\recording.json` (time between presses = time between keys; frame shots with the free
      camera). Not yet: freezing the player, hiding the HUD, actors other than the player, triggers (M5), sound.
    - **Probe (`spikes/cutscene_probe.lua`, built 2026-10-07; the user's, once, in game):** F5 writes down the
      camera (object, position, rotation, FOV, components, parent), the player lookups, what `fs.glob` returns and
      whether `os.clock` exists; F6 holds the camera with each of 7 hooks in turn (UpdateBehavior, LateUpdateBehavior,
      UpdateMotion, PrepareRendering, BeforeLockSceneRendering, LockScene, BeginRendering; 3 s each, shown on screen)
      and reads back after BeginRendering how often something moved it; F7 = "this one held still"; F8 restarts the
      player's current animation through changeMotion. Results: `reframework\data\remod_cutscene_probe.json` (remod
      can read it from the Game folder). Guide: `spikes/cutscene_probe.md`.
    - Checked: tests (the runtime loaded in Lua 5.4 with stand-in REFramework tables: `camera_at` before / between
      (linear, smooth, cut) / after keys, missing FOV, no keys; both scripts' syntax; the example's shape). **Not
      checked: anything in game** (all four probe questions are open).
    - **Probe run 2026-10-07 (user) [game data, `remod_cutscene_probe.json`]:** camera `MainCamera` (no parent;
      `sdk.get_primary_camera()`), Leon `ch0a0z0_body` by the EMV lookup. Hooks, frames the camera moved after our
      set: UpdateBehavior 408/477, LateUpdateBehavior 463/463, UpdateMotion 457/457 (they run before the game's camera:
      overwritten), PrepareRendering 2/473, BeforeLockSceneRendering, LockScene, BeginRendering 0 (the user saw the
      view hold from the 4th on, no jitter or drift). **But playing the test cutscene with BeginRendering alone, the
      view followed the mouse** (Leon also moved, expected), while a readback after BeginRendering each second showed
      position and rotation exactly ours, cut included: the view is taken before that. Now `camera_hooks` sets it
      after LateUpdateBehavior and before PrepareRendering, BeforeLockSceneRendering, LockScene, BeginRendering:
      **in game the view holds (mouse ignored) and follows the keys, cut included** (user, 2026-10-07; which step
      is the one that matters wasn't narrowed down). A camera error shows in the menu and is logged once per play
      (`log.*` reaches the log without "Log Lua Errors to Disk"). `changeMotion`
      (bank 1000, motion 160) visibly restarted Leon's animation. `fs.glob` returns paths relative to
      `reframework\data` (the runtime's fallback handles it); `os.clock` exists. Not yet tried: a recorded cutscene
      played in game. `spikes/cutscene_test.json` (steps in the probe's guide) is one at the probe's spot, from its
      camera pose only (push in towards Leon, cut, rise; no invented rotation); a test checks it. The schema example's
      positions are invented (format only, not for playing).
    - **Cutscene block (built 2026-10-07; core `cutscene.*`):** `Cutscene` (Source): Cutscene file (.json; needn't exist
      yet), advanced Open with; list output "cutscene files" = `cutscene_files`: `runtime_dir()` (`<tool>/runtime`,
      beside profiles; installed into the zip) `/remod_cutscene.lua` at `reframework/autorun/remod_cutscene.lua`, then
      the file at `reframework/data/remod_cutscenes/<name>.json`. Its run checks the file (`check_cutscene`: every
      problem named, e.g. "camera key 3: t 9 is past the length (5)"; schema 0, length, start key not F10, letterbox,
      keys in time order, position 3 / rotation 4 numbers and a unit quaternion, fov, ease, spans' until after t,
      fades 0-1, motions' whole-number bank / motion, actor "player") and fails on any. Preview lists the files only.
      `build_package` now packages the same file listed twice at one game path once (each cutscene brings the
      runtime); two different files there are still refused. Test in game with no file yet installs the runtime
      alone (`cutscene_runtime`), to record with. App (Use layout): **Use recording** (`use_recording`:
      the game's `reframework\data\remod_cutscenes\recording.json` camera keys into the file, length at least the
      last key + 1 s, the rest kept, the old file as `.bak`; a new file named after its stem if none), **Open in
      editor**, and the Lua script block's **Test in game** / **Remove from game** / **Game errors** (errors of
      `remod_cutscene.lua`; no Fix with Claude: a cutscene is data). The AI note says: camera positions come from a
      recording, never invented. Checked: tests (files and refusals, every check message, Use recording new / merge /
      broken file, two cutscenes into one package with the runtime once, a bad file failing the run, a conflicting
      duplicate refused). **Not checked by eye or in game.** Not done: Write with Claude for cutscene files, an example
      graph (needs recorded positions), a timeline view.
  - **Research (2026-10-03, read-only look at the extracted game files).**
    - **Pre-rendered movies are plain MP4s [game data].** `natives/STM/streaming/_chainsaw/movie/<group>/<id>/<id>.mov.1.x64`
      is an MP4 (`ftyp mp42`, H.264 `avc1`); the non-streaming `.mov.1.x64` beside it is 38 bytes (`REMV`, names
      `dummy.mp4`). A movie is a prefab (`<id>.pfb`: the `.mov`, a render texture `cs_MovieRTT_4K.rtex`, and a sound
      container `_Chainsaw/Sound/Resource/Container/mv/snd_cont_<id>.user`), listed in
      `movie/resource/movieloadtable.user`. Groups: `logo` (mv7000/7001, 1080p / 4K, with AAC audio), `mv` (mva000-202:
      4K plus a `_fhd` 1080p copy, **no audio track**: sound from the container; mva300/301: 1440x1080 / 1080p with
      AAC; mva402-417: 328x408, silent), `richtutorial` (1388x780, silent). Which in-game moment each plays: unknown.
    - **Real-time cutscenes are data [game data]:** `_chainsaw/event/cs/<id>/` holds `.scn` (1,771), `.motlist`
      (1,543), `.user` (534), `.mcamlist` camera motion (126), `.tmlfsm2` (124), `.tml` timelines. Formats not read by
      this tool; REE-Lib / REE Content Editor (MIT) read many of them.
    - **Existing tools:** EMV Engine (MIT; RE4R via the SILVER fork): animation play / seek, free camera that detaches
      from a cutscene, a cutscene viewer, prefab spawning. REFramework's Lua can't play video itself; the old M4 planned a C++
      plugin for that (now route 2's fallback).
    - **Routes, cheapest first:**
      1. *Replace a game movie* (tier 1, asset only): the user's video encoded H.264 at that movie's size, written as
         `<id>.mov.1.x64` (and the `_fhd` copy, like a streaming copy), packaged. Windows' Media Foundation has an H.264
         encoder (no ffmpeg dependency, like WIC for images). [TBD-spike] a different length; the silent ones' sound
         (Wwise container: silence, or replace the bank); which movie plays where.
      2. *Play a movie at a moment of our choosing* (tier 2/3): spawn the game's own movie prefab (or fullscreen movie
         GUI) on a new `.mov` from REFramework Lua; else a C++ REFramework plugin (the old M4). [TBD-spike]
      3. *Real-time scripted cutscene* (tier 2, the M5 runtime): a sequence as data (shots: camera keys, character
         motions from the game's motlists, subtitles, letterbox, timing) played by the one generic Lua runtime; camera
         keys recorded in game with a hotkey into a file the tool reads. Sound is the hard part (Wwise).
      4. *New native cutscene files* (`.tml` / `.scn` / `.mcamlist`): not realistic now; editing existing ones is
         Content Editor's ground.


- **Priority order (user, 2026-10-02). See also the M4 "Order" below.**
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
  - **Story movies' sound. Built 2026-10-06 (user: "do all of it so we can wrap up sound as a restriction"); was a
    future consideration.** 25 of the 29 movies have no sound in the file. Only 3 have a sound container
    (`snd_cont_<id>.user`) and packages: mva000 (music, dialogue in 9 languages, effects), mva201 (effects),
    mva202 (dialogue, effects). The other 22 (radio calls, tutorials, the rest) have none: silent, or their sound
    comes from elsewhere [game data].
    - **What's built:** Replace movie's **Replace its sound** (advanced checkbox, on by default) and its list output
      **sound packages (if any)**, into Package's other file.
      - The packages are found by the profile's optional `sound_dir` and `movie_sound` (RE4R:
        `_chainsaw/sound/wwise`, `ch_{id}_`); every sound in them is replaced through core `replace_sounds` (§10
        "Game sounds" below): the packages' both copies, and the banks holding their first parts and sizes.
      - **Fixed the same day:** the first version rewrote the packages only. Every movie sound also has its first
        part (prefetch) in a bank (`ch_mva000_bgm.sbnk` etc.), and a bank records its size: the game would have read
        the old sound's header and opening before the new stream. Now replaced in step (checked on example 12: each
        bank's first part is a prefix of the new streamed sound, every recorded size matches).
      - Each new WEM keeps its original's codec, channels, rate and (Same length on, or a music track) sample count:
        core `encode_wem` (Vorbis: libvorbis at the quality whose setup is the original's, checked against it with the
        codebook library; Opus: libopus multistream at the original's average bit rate, channels reordered from a
        WAV's to Vorbis's order). Same length off: the new movie's length (in-game question).
      - The music (or, with no music, the effects) gets the video's sound (core `read_sound`: Windows decodes it to
        mono or stereo; more channels get it in front left / right), a test card's a beep each second; every other
        sound, dialogue in every language too, silence.
      - Run cache: one set per movie (`cached_sound_files`: `<key>_<n>.wwise` and a `<key>.sounds` list of game
        paths), keyed on the packages, the video (or "card"), the length rule and "snd2".
      - Core `decode_wem` (both codecs) for checks and the Browser's playback.
    - **Checked:**
      - tests:
        - packages round trip;
        - Opus 1 / 2 / 3 / 6 channels encoded and decoded;
        - Wwise's own reference WEMs decode to their WAVs with the channels in place (the spike files, when
          present);
        - with REMOD_GAME: the game's packages rewritten byte-identical (and their header-only copies), the music's
          setup found and a tone through it, the dialogue's 3 channels;
        - the block on a fake game folder: effects beep, dialogue silent, another movie's package left alone, an
          unchanged re-run, off;
      - example 12 on the real mva000: 11 packages (22 files) and 10 banks (mva000's effects sound has no bank entry
        anywhere); every WEM read by vgmstream r2117 with the original's id, codec, channels and sample count; the
        music decodes to a beep each second over 61.5 s.
    - **Not checked:** in game (§9's list).
    - **The game's side [game data, read-only, 2026-10-06]:** `snd_cont_mva000.user` names three triggers
      (`snd_trgr_mva000_bgm` / `_dialogue` / `_se`) and a package (`snd_pack_mva000`). The audio is in
      `streaming/_chainsaw/sound/wwise/ch_mva000_bgm.spck.1.x64` (AKPK, one WEM: Wwise Vorbis 0xFFFF, stereo 48 kHz,
      1.4 MB) and `ch_mva000_dialogue.spck.1.x64.<lang>` (9 languages; English: one WEM, Wwise Opus 0x3041, 3
      channels, 48 kHz). Every `.sbnk` checked (300) is bank version 140 = Wwise 2021.1; RE4R audio guides on Nexus
      use Wwise 2021.1.14 to make WEMs.
    - **Package layout [game data, all 1,266 packages]:** "AKPK", u32 header size (from after this field to the
      data), u32 version 1, u32 sizes of the language map, bank table, stream table and externals table; the language
      map (u32 count, {u32 offset from the map's start, u32 id}, UTF-16 names); each table a u32 count then entries
      {id (u32; u64 for externals), u32 block size, u32 size, u32 start block, u32 language id}; then the files.
      Every RE4R package: block size 1, the files back to back in table order with no gaps; the copy outside
      `streaming/` is the header alone, the streaming copy header and files.
  - **Game sounds: any sound replaced. Built 2026-10-06 (user: "I would like to be able to replace any game
    sound").** Core `sound.*` over `wwise.*`; block **Replace sounds**; the Browser lists, plays and saves sounds.
    - **Where RE4R's sounds are [game data, every file read 2026-10-06]:**
      - 3,367 banks (`.sbnk`, outside streaming/ only) and 1,266 packages. 111,554 WEMs sit in banks' media
        (`DIDX` + `DATA`), 41,565 in packages; 491 bank media aren't sounds (counted 2026-10-07; first estimate ~700) (start `00 04 02 00`, 80-740 KB;
        inferred: reverb impulse responses), 8 are empty.
      - Codecs: Opus 1-12 channels (mapping family 0 for 1-2, 1 for 3-8, 255 for some 1, 4, 7 and 12-channel
        ones; 417 sounds), Vorbis 1-12 channels. The writer does all three (255: each channel its own stream, in a WAV's
        order: real 1- and 4-channel ones decode as vgmstream r2117 does, within 1, channels in place; vgmstream
        refuses the 12-channel ones, so those aren't cross-checked).
      - **Banks:** chunks BKHD, DIDX (12-byte entries: id, offset in DATA, size), DATA (each media 16-byte aligned,
        zero padding, nothing after the last), HIRC; some have only BKHD and HIRC, or BKHD with media. Every bank
        rewritten unchanged is byte-identical (tested on 4, by REMOD_GAME).
      - **Streamed sounds have a first part in a bank (Wwise's prefetch):** 13,493 bank media are exact byte
        prefixes of a package's WEM, 804 whole copies (a short sound fits). 14,273 of 40,214 package ids.
      - **The event data records sizes:** HIRC objects (u8 type, u32 size, body), a Sound (type 2: u32 id, then a
        source) or a music track (11: u32 id, u8 flags, u32 count, sources); a source is u32 plugin, u8 stream type
        (0 in a bank, 1 prefetched, 2 streamed only), u32 media id, u32 in-memory size, u8 bits. For 0 the size is
        the media's (137,749 of 137,749); for 1 the prefetch's (15,073 of 15,073); for 1 and 2 it always ends on a
        packet boundary: the WEM's header and its first k packets (k under 20 mostly; 66,000 checked), recorded even
        when no prefetch is kept. Music tracks also record their sounds' lengths (not read).
    - **The replacement (`replace_sounds`):** every package whose table names the id (both copies written), every
      bank whose media or event data does. The original comes from its package (streamed) or a bank (whole); each
      language's file its own. The new WEM: `encode_wem` from the `SoundSource`. A bank's first part is the new
      WEM's header and as many packets as the original's first part had (`wem_prefix`, `wem_packets_within`); each
      recorded size becomes the new media size (0) or that prefix size (1, 2). A music track's sound keeps its
      original's length (its track records it). Scanning reads banks' DIDX and HIRC only (seeking past DATA): 1.4 s
      for example 14 on the whole folder.
    - **Game sound block (2026-10-07; user: "dragging individual lines from a sound bank into the graph"; the
      example "should not require exporting to wav outside the window"):** `GameSound`, Source, pure. Fields: Game
      sound (`PathKind::GameSound`: `<bank or package>#<id>`, `game_sound_text` / `parse_game_sound`; no picker,
      filled by a drop) and Your audio (empty: silence, the original's length). Its output is the audio with the
      game path `sound:<id>`; Package refuses one ("goes into Replace sounds").
    - **Replace sounds block:** `sounds` (multiple: Game sound blocks, or audio files whose names hold the id,
      `sound_id_in`: the last run of 4-10 digits), Same length (off: the audio's own length), Game files (advanced,
      `{game}`); list output `sound files` into Package. One Replace sounds per graph's sounds: two would each write
      their own copy of a shared bank. An id in no file fails the run naming it; two sounds for one id too. Run cache
      as Replace movie's. Audio: anything Media Foundation decodes (wav, mp3, m4a, aac, wma, flac, videos).
    - **Drag and drop (app):** a line of the Sounds list is a drag source (payload `remod_sound`, "<absolute
      file>#<id>"). Over the graph it shows a Game sound block's see-through copy (as the Nodes panel's blocks);
      dropped there it adds a Game sound block holding it (the file as `{game}\...`) and links it into the layout's
      Replace sounds block if there's exactly one (else the status line says to link it). **This works in Use layout
      too, the one block Use layout adds** (the Sounds list is only shown there). Dropped on a Game sound field it
      replaces that block's sound; other fields say where it goes.
    - **Browser filter (user, 2026-10-07: folders without sound banks and other languages' files filled the
      Browser):** "Show: Everything / Sound files" under the search box. Sound files: Game files lists only the banks
      and packages (a tree of `AssetIndex::sounds`), search too, and hides the `streaming` folder (a package's sounds
      list from its copy outside it); Language: All, or one (`en`, `de`, ...: files without a language stay).
      **Changing it keeps your place (user, 2026-10-07):** the folder you're on stays selected (or its nearest
      folder the filter still shows; switching back finds the folder itself), and tree folders are keyed by path, so
      open ones stay open.
      ponytail: for the session, not saved in the settings.
    - **Browser:** banks and packages are in the Game files index (`AssetIndex::sounds`, `FileKind::Sound`).
      Clicking one shows **Sounds###viewer**: id (click copies it), length, channels, codec, where ("in this bank",
      "streamed: its first part", "in this package", "not a sound"); **Play** (decoded in the background, a
      streamed sound's whole from its package, `sound_wem`; Windows' `PlaySound`, winmm) / Stop; **Save as WAV**
      (named `<id>.wav`, decoded and written in the background).
      **No lag on big files (user, 2026-10-07: clicking the music froze the app):** listing and `sound_wem` seek to
      each sound and read only its first 4 KB (the whole sound only when an Opus packet table is longer), never the
      whole file. `ch_bgm_castle.spck` (837 MB): `remod sounds` 51 s -> 0.18 s. All 4,633 banks and packages list
      the same 153,119 entries, 499 "not a sound" (491 non-RIFF media, 8 empty). A listing replaced while it runs is
      kept until done (`retired_lists_`): a `std::async` future waits for its task when destroyed. CLI: `remod sounds --file <bank|package>` prints the same list, `remod sound-wav --file
      <...> --id <id> --out <wav>` saves one.
    - Example `14_replace_a_sound`: a Game sound block (the intro's English narration, 880852580) with `beep.wav`
      (made by `make_examples.py`), into Replace sounds.
    - **Checked:** tests (bank and event data round trip, prefetch arithmetic, a fake folder with an embedded, a
      music track's and a streamed sound with its first part: every file and size in step; the block, its cache,
      no id, an unknown id; listing and finding a streamed sound's whole; with REMOD_GAME: 4 real banks byte-identical,
      the intro's English line replaced with its bank's first part a prefix of the new sound and every size
      matching); example 14 from the CLI.
    - Checked (2026-10-07): a test of Game sound blocks (your audio, silence, into Package refused, two picks of one
      sound), example 14 in its new form through the every-example test.
    - **Not checked:** in game (§9); **by eye:** the Sounds viewer, Play / Save as WAV, dragging a line onto the graph
      or a field, the Show / Language filter.
    - **Not done:** bank media that aren't sounds; changing a music track's recorded
      length (so its sounds keep theirs); audio for 3 or more channels goes in front left / right only.
    - **Survey (2026-10-06; licences and activity from GitHub's API):**
      | Candidate | Covers | Gap | Licence | Maintained |
      |---|---|---|---|---|
      | Wwise 2021.1 (Audiokinetic), `WwiseConsole convert-external-source` | The only encoder for Wwise Vorbis / Opus WEMs; command line | Install with an account; can't be bundled | Proprietary; free for non-commercial use | Yes (2021.1 is old; must match the game) |
      | RingingBloom (Silvris) | BNK and PCK editors: replace WEMs; RE Engine sound files; the RE4R guides' tool | GUI only (C#); no licence | None (all rights reserved) | Release v2.1 2021-05; pushed 2025-03 |
      | bnkextr (eXpl0it3r) | Extracts WEMs from BNK and PCK (AKPK); C++ | Extract only | Public domain or alternative | Release 2.0 2021-03; pushed 2026-10 |
      | wwiser (bnnm) | Reads banks (events, timing, codecs); the reference for the format | Read only, by design; Python | None (issue #61 asking for one, unanswered) | Release 2026-08 |
      | vgmstream | Decodes every Wwise codec (Vorbis, Opus) to PCM; C library and CLI | Decode only | ISC-style, permissive | Release r2117 2026-05; pushed 2026-09 |
      | sound2wem (EternalLeo) | A script driving WwiseConsole for any audio file | Needs Wwise | MPL-2.0 | Release v6 2026-01 |
      | wwiseutil / wwvorbis-go (hpxro7) | Unpack / repack BNK and PCK; Wwise Vorbis bindings | Abandoned; no usable encoder | GPL-3.0 | 2018 |
      | REE Content Editor / REE-Lib | Edits the RSZ sound data (`.user` containers, `WwiseRszFileLoader`) | No BNK / PCK / WEM code | MIT | Yes, weekly |
    - **Recommendation: build on them.** Write the AKPK package read / write ourselves in core (a table of ids,
      offsets and sizes; bnkextr's public-domain code as the reference). Encode WEMs by driving Wwise 2021.1's
      WwiseConsole as a managed dependency (detected or installed by the user, like Noesis; never bundled), with a
      conversion setting per codec (Vorbis for music, Opus for dialogue). Decode WEMs for previews with vgmstream
      (permissive). Banks: read them for lengths and codecs, own code with wwiser as the format reference (no
      licence: nothing copied); write them only if a length or codec change proves to need it (in-game test first).
      RingingBloom stays the user's own tool (no licence). AI's place: making the replacement audio (voice,
      music: M4) that this pipeline then encodes and packs.
    - **Open before building [TBD-spike]:** does a WEM of another length play fully without a bank edit; does the
      bank's codec id have to match (Vorbis -> PCM shortcut); why 3 channels in the dialogue WEM.
    - **Decided (user, 2026-10-06): no Wwise dependency.** The tool would write WEMs itself: Wwise Opus with libopus
      (vcpkg, BSD) plus Wwise's header and packet-size table (vgmstream's reader documents the fields), Wwise Vorbis
      with libvorbis (BSD) rewritten to Wwise's packet form and codebook ids (**proven 2026-10-06**, below), PCM as
      is.
    - **Spike prepared (2026-10-06): `spikes/wwise/`.** 7 known WAVs (`make_samples.sh`), `samples.wsources` (each
      as Vorbis, Opus and PCM: ShareSets `remod_vorbis` / `remod_opus` / `remod_pcm`), `run.cmd` (WwiseConsole
      `convert-external-source`), README with the steps. The user runs Wwise 2021.1.14 once to make the reference
      WEMs; the WAVs, the WEMs, the Wwise project and the log are git-ignored (user).
      **Run 2026-10-06:** all 21 WEMs made. `Root` in the list resolves against neither the file nor the working
      folder, so `run.cmd` writes `run.wsources` with an absolute Root (git-ignored).
    - **Wwise Opus WEM layout [spike 2026-10-06, 7 files, all fields checked against the inputs]:** RIFF, no
      padding, chunks `fmt `, `seek`, `data` in that order.
      - `fmt ` is 36 bytes: tag 0x3041, channels, rate 48000, avg bytes/s = data size x rate / total samples
        (floor), block align 0, bits 0, cbSize 16 (but **18** extra bytes follow: Wwise's quirk). Extra, little
        endian: u16 960 (frame, 20 ms); **u32 channel config** (Wwise's AkChannelConfig: channels in bits 0-7,
        type 1 = standard in bits 8-11, speaker mask from bit 12: mono 0x4101 = centre, stereo 0x3102 = L R, 3 ch
        0x7103 = L R C [vgmstream's reader]); u32 total samples; u32 packet count; u16 pre-skip 312 (libopus's
        lookahead, `OPUS_GET_LOOKAHEAD`); u8 1 (OpusHead version); u8 mapping family (0 mono / stereo, 1 for 3
        channels).
      - Packet count = ceil((samples + 312) / 960), i.e. the packets written. (Not ceil(samples / 960) + 1: the
        game's English mva000 dialogue, 2,916,800 samples, has 3,039 packets.)
      - `seek` = one u16 per packet: its byte size. `data` = the raw Opus packets back to back, no length prefixes
        (their sizes sum to the data size exactly). Silence packets are 3 bytes (`fc ff fe`).
      - Packets are plain 20 ms CELT fullband (TOC 0xf8 mono, 0xfc stereo). 3 channels are 2 streams: a stereo
        packet in self-delimited form (TOC, length, frame), then a mono one, as libopus's multistream encoder packs
        them. The framing fits every packet of the 3-channel sample and of the game's dialogue (de, en, es); not
        decoded.
      - **The game's own (mva000 dialogue, 3 ch) has the same layout, field for field.** So a writer is libopus's
        multistream encoder plus this header; every field is computed, nothing copied.
      - Not yet: whether the game needs anything the references lack.
    - **Wwise PCM WEM layout [spike 2026-10-06, 7 files, all fields checked]:** a 64-byte header then the samples;
      the easiest of the three. RIFF, `fmt ` (24), `JUNK` (4 zero bytes), `data`; RIFF size = file length - 8.
      - `fmt `: tag 0xFFFE (extensible, but cbSize 6 and only 6 extra bytes, not the usual 22), channels, 48000,
        avg = rate x align, align = 2 x channels, bits 16, cbSize 6. Extra: u16 0 (16 for the 3-channel file, as
        in the Vorbis one: WAVEFORMATEXTENSIBLE's valid-bits slot; why only 3 ch is unknown, copy it), u32 channel
        config (as Opus).
      - `data`: 16-bit little-endian interleaved samples, at file offset 64.
      - The samples equal the WAV's in 4 of 7 files; in the other 3 (two tones and the sweep) 1-2% of samples differ
        by exactly 1. Not a plain rounding or scaling of the 16-bit values (tried x*32767/32768 rounded, truncated,
        floored); likely a float stage inside Wwise. Inaudible, so a writer needn't reproduce it.
    - **Wwise Vorbis WEM layout [spike 2026-10-06, 7 files; the parts below hold on all 7, the rest is marked]:**
      RIFF with `fmt ` (66 bytes) then `data`, nothing else.
      - `fmt `: tag 0xFFFF, channels, 48000, avg bytes/s = (data size - seek table size) x rate / total samples
        (floor), align 0, bits 0, cbSize 48 (and 48 extra bytes, unlike Opus). Extra, little endian, offsets from
        the extra's start:
        | At | Field | Writer |
        |---|---|---|
        | 0 | u16 0 (16 for the 3-channel file, as PCM) | copy |
        | 2 | u32 channel config, as Opus | computed |
        | 6 | u32 total samples | computed |
        | 10 | u32 loop start: the first audio packet, as an offset from the setup block's start (= setup block size: 203 mono and 3 ch, 217 stereo) | computed |
        | 14 | u32 loop end: the audio's end, from the same start (= data size minus the seek table) | computed |
        | 18, 20 | u16 loop start extra (0), u16 loop end extra (= @32 when not looping) | computed |
        | 22 | u32 seek table size in bytes (= 4 x entries) | computed |
        | 26 | u32 audio offset in `data` (= seek table + setup block) | computed |
        | 30 | u16 largest audio packet payload, in bytes | computed |
        | 32 | u16 **end padding**: decoded samples past the total (960, 64, 0, 8 here; game music 384) | computed |
        | 34, 38 | two u32 that go with the setup block (stereo 0x3ed0, 0x40b0; mono and 3 ch 0x4704, 0x48cc; game music 0x4740, 0x4930); inferred: decoder memory sizes | copy with the setup |
        | 42 | u32 the setup's hash (one per setup block, see below) | copy with the setup |
        | 46 | u8 short block power (8 = 256), u8 long block power (11 = 2048) | from the setup |

        Field names at 10-20 and 32 follow the Wwise SDK's Vorbis info struct as vgmstream's reader describes it
        [recalled, not re-read]; the values are checked here: @32 equals the decoded length minus the total in all 7
        and in the game's music.
      - `data`: seek table, then the setup block (u16 size, payload), then audio packets, each u16 payload size +
        payload, ending exactly at the chunk's end.
      - **Audio packets:** Vorbis audio packets without the packet-type bit; the first bit is the mode, and mode 1
        is the long block. Checked by counting: each packet gives (previous block + this block) / 4 samples, and
        the sum minus the total is exactly @32's padding, in all 7 and in the game's music.
      - **Seek table, solved:** entries of u16 samples + u16 bytes, each a step from the entry before. An entry is
        a packet: the first packet whose end (in decoded samples) is at least the previous entry's end + G (from 0),
        as many as fit. Samples = that packet's end, bytes = its offset from the setup block's start. G is the
        conversion's seek granularity: 16384 in the samples (Wwise's default), **2048 in the game's music**. Holds
        for every entry of all 7 and of the game's 1,439. (The silent file's bytes "exceeding its audio" was only
        the 217-byte setup counted in.)
      - **The setup block is a fixed blob per encoder setting and channel layout.** All 5 stereo samples have the
        same bytes (215), mono and 3 channels share theirs (201); the game's music has another (228, higher
        quality). Its payload starts with the codebook count - 1 (8 bits), then a 10-bit id per codebook, an index
        into Wwise's built-in codebook library (aoTuV 6.03): stereo 38-49, 62-76, 416-430; mono 38-49, 62-76,
        213-224; game music 50-76, 463-479. So a writer can ship one reference setup (with its hash and the two
        u32s) per layout and quality; the remaining question is encoding audio that uses exactly those codebooks.
      - **Writing Wwise Vorbis with libvorbis works [spike 2026-10-06, Python in the session's scratchpad; to be
        redone in C++].**
        - **The codebook library:** ww2ogg's `packed_codebooks_aoTuV_603.bin` (BSD: Xiph and Adam Gashlin; ships
          with its notice). 598 codebooks, all readable; 26 codebooks sit under several ids. Every codebook libvorbis
          uses, at every quality from -1 to 10, mono and stereo, is in it.
        - **The game's setups are libvorbis's.** RE4R's sound packages and banks hold ~134,400 Opus WEMs (dialogue,
          effects; 1-12 channels) and 894 Vorbis ones, all 48 kHz but one. The Vorbis ones use 6 setups. Expanded to standard Vorbis, 5 are bit-identical to ffmpeg's libvorbis at 48 kHz:
          - music (`ch_bgm_*`, `ch_mva000_bgm`): stereo, and the uncoupled form for 1 / 4 / 12 channels: q 7.0-7.9;
          - cutscene sound effects (`ch_csa*_se`, 1 to 8 channels, one stereo): q 8.0-8.9;
          - 4 weapon sounds in banks: q -1 (or 3).

          The sixth is one 6 kHz file, not tried. Within a band the setup is the same, so the exact quality is
          unknown (any 7.x makes the music's). More than 2 channels use the mono-style setup: no coupling.
        - Because the expanded setups match, the writer takes the replaced file's own setup block, hash (@42) and the
          two u32s (@34, @38) and checks libvorbis's setup expands to the same (the library is needed only for that
          check).
        - **Audio packets:** libvorbis's packets with the packet-type bit and, on long blocks, the two window bits
          removed.
        - **Channel order:** Wwise keeps the WAV's order (3 ch: L R C), not Vorbis's L C R. ffmpeg reorders on
          decode; a writer feeds libvorbis the channels as they come.
        - **Checked:**
          - the 7 Wwise samples, turned into Ogg, decode with ffmpeg at their exact lengths (tones 32-36 dB per
            channel; a lossy codec);
          - the game's mva000 music, rebuilt from its own packets with the computed header and seek table, is
            **byte-identical** (1,425,446 bytes);
          - new WEMs from libvorbis q7 (sweep and noise in the music's stereo form, a tone in its mono form) are read
            by vgmstream r2117 as Wwise Vorbis with the right sample count, and decode to libvorbis's own output
            within 1.
        - **Not exact:** from an Ogg packet the real bit count is lost, so a repacked packet can be one zero byte
          longer than Wwise's (decoders ignore it). In C++ the writer could take the bit count from libvorbis.
          Not tried: more than 2 channels through the writer, and in game.
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

- **M4 (was M2, renumbered 2026-10-03): AI.** AI backend as a separate local process (not in the app). GPU/VRAM detection at install.
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
    tools below move from "before M4" to "before the generation block".
  - **Tools the AI's results must pass through, to have before the generation block (user, 2026-10-02: "give the AI as
    many polished tools as we can"):**
    1. **Channel tools** (split / merge, §10 Future work): an AI result is plain RGB and would overwrite data packed
       in a channel. Spike done (§9): sRGB = colour, UNORM = data; `albd` alpha is data; `nrrc`'s normal is in G/A.
       **Built 2026-10-02** (§4 Channel tools: Merge channels with "Colour from" keeps the original's alpha).
    2. **Streaming copies** (§9): character textures are the ones with `streaming/` copies. **Decided 2026-10-03: if a
       texture has a streaming copy, replace both** (edit at the streaming size, write both). **Built 2026-10-03**
       (§4 Conditions: Streaming copy).
    3. **Masks / regions:** "change only the jacket". **Built 2026-10-02** (§4 Masks: Mesh mask, Blend in mask).
    4. **Batch** (§10 Future work): worth it once there's real work per texture, which AI is.
    Done: an unchanged run writes nothing (the run cache, §4); AI iteration means many runs.
- **M5 (was M3, renumbered 2026-10-03):** REFramework Lua runtime reading manifests (triggers → actions): the one
  generic runtime M2's trigger -> action blocks and M3's scripted cutscenes run on. (The old M4, a C++ plugin for
  video playback, is now M3 route 2's fallback.)
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
    Game files shows it as a `streaming` folder browsed on disk (user, 2026-10-06: "some level of access"; read when
    opened, not searchable from Game files).
  - Core: `core/browse.*` (index, tree, search, `mesh_textures`, `preview_file`) and `read_tex_pixels`. The app
    uploads the stored mip to D3D11 as is: RE Engine's format numbers are DXGI's, so the GPU decodes BC1-7 itself.
    **With its smaller stored mips (2026-10-06, `read_tex_mips`; user: streaming copies "seem lower quality, even at
    high resolution"):** one 2048 px mip drawn in a ~400 px viewer skipped pixels (grainy). The data was fine: Leon's
    `lowerbody_albd` is BC7 sRGB in both copies (base 1024, 2 mips; streaming 4096, 4 mips), the streaming copy
    scaled to 1024 matches the base (PSNR 33.9 dB), and a crop of it holds real detail the base enlarged doesn't
    (images looked at). The chain stops at a mip not half the one above (a padded texture). Images (PNG) still
    upload one level (ponytail: GenerateMips needs a render-target format).
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
