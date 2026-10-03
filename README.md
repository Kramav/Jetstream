# Jetstream — RE Engine Modding Tool

Node-based modding tool for Capcom RE Engine games. First target: Resident Evil 4 (2023).

**Status:** Milestone 1 complete. A node graph (texture → image → edited texture → Fluffy-ready zip)
runs from the desktop app and from the CLI.

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

## Two modes

The switch centred above the graph changes between:

- **Use layout:** run a finished layout. Fill in the fields, Run, edit the images, click Done editing. Blocks and
  links are locked, and blocks can't be moved.
- **Build layout:** make or change a layout.
  - **Add a block:** right-click empty canvas.
  - **Add a block already linked:** drag from a pin and let go on empty canvas.
  - **Put a block between two others:** right-click a line → Insert node here.
  - **Remove a link:** right-click its line → Delete link.
  - **Change a block:** right-click it → Duplicate, Disconnect all or Delete.

The mode is remembered. New starts in Build layout.

Lines run at right angles around the blocks, never underneath them. One output feeding several inputs is drawn
as one line that branches, with a dot where it splits.

## Graph features

- **Every field has a pin.** Link a node's output into a field instead of typing it. A linked field shows
  `<- source`.
- **Text node:** type a value once and link it into several fields. `{1}`, `{2}`… in its text are filled from
  whatever is connected to its numbered inputs, e.g. `{1} v2`.
- **Several textures per mod:** give each texture its own Original texture → Export → Edit → Convert chain and connect
  every new texture to Package for Fluffy. A new input line appears each time. One run exports all the images.
- **Preview for Fluffy:** connect edited images, or any image via Use existing image, to Package's `preview`.
  Several are combined into one picture.
- **Edit format:** Export image writes PNG, TGA or JPG, whichever its file name ends in. In the `...` picker,
  pick the format under "Save as type". TGA suits GIMP. JPG loses some quality and all transparency.
  BMP isn't offered (Noesis writes it without transparency).
- **Replace existing** on Package rebuilds over the previous build of the same mod, so no manual deleting.
  It never deletes anything else.
- **Noesis (optional):** the Pipeline panel finds it automatically (remembered path, `REMOD_NOESIS`, PATH, winget)
  and says whether the RE Engine plugin is installed. Tick **Convert textures with Noesis** to use it for textures.
- **Game files (REtool):** set your REtool folder, e.g. `...\REtool\RE4\re_chunk_000\natives\stm`, and the
  texture picker opens there. It's pre-filled from the RE plugin's own setting if you've set it in Noesis.

**Mip levels and padded rows:** the built-in converter writes the new texture over a copy of the original, so
its mip count, row padding and header stay the original's. The Noesis converter writes mip levels down to 8×8
whatever the original has (most RE4R UI textures have one); the tool builds the mod anyway and shows a warning (a
popup in the app, `WARNING:` from the CLI). See CLAUDE.md §9.

**Any texture name:** textures are recognised by their header, not their name: `.tex`, `.tex.143221013` and
`.tex.re2remake` all work, and a package names each one the way the game expects (`.tex.143221013` for RE4R).

## Browser

The **Browser** panel on the left browses your Game files (REtool) folder, once it's set in the Pipeline panel.
Two panels along the bottom show what you pick there:

- **Browser:** a folder tree, a search box (several words narrow it down, e.g. `wood albd`) and the folder's file
  paths, meshes first. Hover a name for its full path.
- **Textures** (bottom): thumbnails of the picked mesh's textures, or of the textures in the folder (or the search).
  Above them, the selected texture's size and format. **Use in graph** (or a double-click) puts it into the
  selected Original texture block, or the layout's only one. In Build layout a block is added if there's none.
  **Transparency** is off by default: many textures keep other data in the alpha channel (e.g. metalness), which
  would hide the picture.
- **Texture / 3D view** (bottom left): the selected texture, large: the wheel zooms (about the mouse), drag to move
  it, double-click to fit it again. Picking a mesh shows it in 3D there instead,
  with its colour textures; close the 3D view to see the texture again, pick the mesh again to reopen it. The last
  mesh stays while you browse other textures. Drag to turn it, right-drag to move, the wheel zooms, double-click
  resets. Click one of its textures to highlight the parts that use it (click it again to show all). The Groups
  checkboxes show or hide the mesh's groups (e.g. a character's bare arms, a weapon). The tool reads RE4R meshes
  itself; for other games' meshes it uses Noesis, if you've set it.

Reading the folder takes a few seconds each time the app starts. Nothing is written to disk, except the
temporary file Noesis converts a mesh into when it's used (deleted straight away). A folder that
doesn't end in `natives\stm` is only read after you click **Index it anyway**.

## Making a texture mod with a graph

A graph chains the steps: **Original texture → Export image → Edit image → Convert image to texture → Package for
Fluffy**, plus a link from Original texture to Convert's `original texture` input.
`schemas\graph.v0.example.json` is a ready-made one; copy it and change the paths. Relative paths in a graph are
relative to the graph file's folder.

**Edit image** is your step, marked "YOUR STEP" with an amber border:
1. **Run:** Export image writes the image file, and the run stops at Edit image.
2. On Edit image, click **Open in editor**, make your change, save it (same width, height and format), and click
   **Done editing**.
3. **Run again:** your image is converted like the original and packaged.

After each run every block shows how far it got: green = done, amber = waiting for you, red = failed (with the
reason), grey = not reached. From the CLI, the second run is `remod run ... --edited true`.

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
   and press Delete to remove. Then click **Run**.
3. Every path field in a node has a **`...`** button that opens the Windows file or folder picker.
4. The panel lists any problems (missing fields, unconnected inputs) and shows the run log.
5. **Save** / **Save As...** write the graph, including node positions.

The app remembers the graph file and Noesis paths between sessions in `%APPDATA%\remod\settings.json`.
It writes that file only when you Load, Save or Run and a path changed.
It looks for the `profiles` folder in the current folder, then next to and above the exe; from
`build\app` it finds the repo's.

**From the CLI** (normal PowerShell, repo root):

```powershell
.\build\cli\remod.exe run --graph C:\work\my_mod.json
```

Add `--noesis "D:\path\to\Noesis64.exe"` to `run`, `tex2png` or `png2tex` to convert with Noesis instead.

## Many textures at once

Start the graph with a **Files in folder** block instead of picking one texture:
- **Folder and files:** pick the folder, and optionally which files (`*_iam.tex*`; empty means every texture).
- **Repeating:** every block it feeds runs once per file.
- **File names:** put `{name}` in the file names those blocks write, e.g. Export image's `edits\{name}.png`. It
  becomes each texture's name, so each one gets its own file.
- **Collecting:** Package takes them all and builds one mod.
- **Editing:** the run exports every image, then the **YOUR STEP** card lists them. Mark each one **Done** as you
  finish it (or **Done editing all**), then Run again. Finished images move on; Package waits until all are done.
- **If a file fails:** the block's **If a file fails** setting stops the run (the default) or skips that file and
  goes on with the rest.
- **Which file the previews show:** the ◀ ▶ arrows on the Files in folder block pick it. A run always does them all.

## Channels

RE textures often keep data, not transparency, in a channel. An `albd` texture's alpha is one example. To change a
picture's colour without disturbing that data:
- **Pick channel** shows one channel as a grey picture, to see or edit it on its own.
- **Merge channels** puts channels back: the colour from one image (for example an edited or AI-made picture), and
  any single channel from a grey image. Everything else stays as the base image has it.

## Masks: changing one area only

To change only part of a texture, such as a character's jacket:
- **Mesh mask** makes the mask. Give it the mesh, the texture, and the material names to include (`*jacket*`; the
  run lists the names if none match). It's white where those parts sit on the texture.
- **Blend in mask** takes the original, the edited (or AI-made) picture and a mask. The edit shows where the mask is
  white and the original where it's black. **Feather** softens the edge.

Any grey picture works as a mask too, for example one you painted, or one made with Pick channel.

## Custom nodes

Turn a group of blocks you use often into one block of your own.

**Making one:**
1. In Build layout, select the blocks.
2. Right-click one and pick **Make custom node...**, then give it a name.

The blocks become a single block. Every link into or out of the group becomes one of its pins.

**Where it goes:**
- It's saved to your library (`%APPDATA%\remod\nodes`) and appears in the Nodes panel under **Custom nodes** in every
  layout.
- A layout that uses one keeps its own copy, so the layout still opens and runs on another PC.
- If your library's version is newer, the Pipeline panel offers to use it.

**Editing one:**
- Right-click it and pick **Edit custom node**. Its blocks open in the canvas.
- **Input** and **Output** blocks are its pins. Give an Input a default value to make that pin a field you can type
  into on the block.
- **Save** updates your library and the layout; **Back to the layout** returns.

Everything works inside a custom node. An Edit image inside one shows up on the YOUR STEP card under the custom
block's name.

## Running other programs

The **Run program** block runs a program or script as one step of the graph: an `.exe`, `.bat` / `.cmd`, `.ps1` or
`.py`.
- **Program:** a full path, a script next to the graph, or just a name found on PATH (`claude`, `python`).
- **Arguments:** typed as on a command line. Use `{in}` for whatever is linked into its input, `{out}` for its
  **Output file**, and `{name}` for each file's name in a Files in folder list.
- **What it passes on:** what the program printed (for example `claude -p "..."`'s answer), and the output file.
- **When it runs again:** only when the program, its arguments or its input (a file by its contents) changed, or
  its output file was changed or deleted. Otherwise a Run passes on the last result without starting it.
- **Always run:** tick this when the result depends on something else, like a file only the arguments name, the web
  or the time.

The program runs as you and can change any of your files; nothing checks what it does. Graphs run by the API (an
AI) need your approval before any program starts. Batch-file arguments can't contain `& | < > ^ % !` or quotes,
because Windows would run them as commands; use the program's `.exe`, or a `.ps1` / `.py` script.

## Letting an AI build mods (MCP)

`remod mcp` is an MCP server. An AI client such as Claude Code or Claude Desktop can use it to:
- build and change graphs with the same blocks and rules as the app;
- check them, and look at their preview pictures;
- run them.

**Claude Code:**

```powershell
claude mcp add remod -- "E:\path\to\Jetstream\build\cli\remod.exe" mcp
```

**Claude Desktop:** add this to `claude_desktop_config.json`:

```json
{ "mcpServers": { "remod": { "command": "E:\\path\\to\\Jetstream\\build\\cli\\remod.exe", "args": ["mcp"] } } }
```

**What stays yours:**
- **Approving changes:** when a run would remove a file, write outside the graph's folder or start a program, a
  Windows dialog asks **you**, listing the changes. The AI can't answer it, and nothing runs without your Yes.
- **Edit image steps:** these wait for you. The AI is told to mark one done only after you say you've finished.
- **Game files:** never changed.
- **Settings:** it uses the app's choice of texture converter and your Game files folder.

## Single steps with the CLI

The same steps are also available as individual commands:

Run from the repo root in a normal PowerShell window:

```powershell
# 1. Original texture -> PNG (or .tga / .jpg; also prints size, format and mip count)
.\build\cli\remod.exe tex2png --profile profiles\re4r.toml `
  --tex "C:\work\my_texture.tex.143221013" --out "C:\work\my_texture.png"

# 2. Edit the image in any image editor. Keep the same width and height.

# 3. Edited image -> new texture, using the original as the template (same format and mips)
.\build\cli\remod.exe png2tex --profile profiles\re4r.toml `
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
