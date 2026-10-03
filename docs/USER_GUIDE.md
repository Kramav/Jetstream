# remod — RE Engine texture modding

remod makes texture mods for **Resident Evil 4 (2023)** that you install with **Fluffy Mod Manager**. You lay out the
steps as blocks in a graph (take a game texture, export it as an image, edit it, turn it back into a texture,
package the mod). Then you run the graph and edit the images when it asks.

## What you need

- Windows 10 (1803 or later) or Windows 11, 64-bit.
- The game's files extracted with **REtool** (the `re_chunk_000.pak` extraction). remod reads textures and meshes
  from there and never changes them.
- **Fluffy Mod Manager**, to install the mods it builds.
- An image editor (GIMP, Photoshop, Krita, Paint.NET…).
- Optional: **Noesis** with the **fmt_RE_MESH** plugin. remod converts textures and reads RE4 meshes itself, so you
  only need Noesis if you prefer its texture conversion or want 3D views of other games' meshes.

Nothing is installed. Unzip the folder somewhere with a **short path**, e.g. `C:\remod`. Windows' 260-character path
limit is a known problem for RE Engine mods, so keep your mod and output folders short too (e.g. `E:\mods`).
Keep the `profiles` folder next to `remod-app.exe`.

## First start

1. Run **`remod-app.exe`**.
2. In the **Pipeline** panel on the right, set **Game files** to your REtool folder, e.g.
   `...\REtool\RE4\re_chunk_000\natives\stm`. The **Browser** on the left then lists the game's textures and
   meshes. Reading the folder takes a few seconds each time the app starts.
3. Open `examples\texture_mod.json` (the `...` button next to *Graph file*) to see a complete mod. Save a copy
   (**Save As...**) before you change it.

The app remembers its settings in `%APPDATA%\remod`. It keeps converted textures in `%LOCALAPPDATA%\remod\run_cache`
(at most 512 MB) so a run that changed nothing writes nothing.

## Making a texture mod

A basic mod is this chain: **Original texture → Export image → Edit image → Convert image to texture → Package for
Fluffy**, plus a link from Original texture into Convert's *original texture* input (the new texture copies its
size, format and mip levels).

1. **Pick the texture:** find it in the Browser, then **Use in graph** (or double-click it). It goes into the
   Original texture block. Or use the block's `...` button, or drag the file from the Browser onto the field.
2. **Fill in the fields:** where Export image writes the image to edit, and Package for Fluffy's mod name, author,
   version, description and output folder.
3. **Run.** The image is exported and the run stops at **Edit image**, which is marked **YOUR STEP**.
4. Click **Open in editor**, make your change and save it. Keep the same width and height. To always open a
   particular program (e.g. GIMP), set the block's *Open with* field.
5. Click **Done editing**, then **Run** again. Your image becomes a texture and the mod is packaged.

The result is `<output folder>\<ModName>.zip`, with the mod folder at its root (`modinfo.ini`, the preview picture and
`natives\STM\...`). Add that zip to Fluffy Mod Manager.

After each run every block shows how far it got: **green** = done, **amber** = waiting for you, **red** = failed
(hover it for the reason), **grey** = not reached. Running again only redoes what changed.

**Things worth knowing:**
- **Edit format:** Export image writes PNG, TGA or JPG, whichever its file name ends in. TGA suits GIMP. JPG loses
  some quality and all transparency.
- **Transparency in game textures:** many textures keep data, not transparency, in the alpha channel (for example
  the `albd` colour textures). The image blocks leave alpha untouched. Keep it as it is when you edit, unless you
  know what it holds.
- **Your edit is safe:** Export image never overwrites an image that goes to an Edit image step. If you pick a
  different texture, it exports the new one and asks you to edit again.
- **Preview picture for Fluffy:** link images into Package's *preview*; several are tiled into one picture.
- **Several textures in one mod:** give each its own chain and link every new texture into Package. Each link adds
  an input row.
- **Replace existing** on Package rebuilds over the previous build of the same mod, and never deletes anything else.
- **The in-game path** is worked out from where the texture sits in the REtool folder. For a texture from elsewhere,
  fill in Original texture's *In-game path* (the path after `natives/STM/`).
- **Texture names don't matter:** `x.tex`, `x.tex.143221013` or `x.tex.re2remake` all work; textures are recognised
  by their contents and named the way the game expects in the mod.

## The two layouts

The switch above the graph changes between:

- **Use layout:** run a finished graph. Fill in fields, Run, edit, Done editing. Blocks and links are locked. The
  Browser, a large viewer and the textures of what you picked are shown.
- **Build layout:** make or change a graph.
  - **Add a block:** right-click empty canvas, or drag one from the **Nodes** panel at the bottom.
  - **Add a block already linked:** drag from a pin and let go on empty canvas.
  - **Put a block between two others:** right-click a line → Insert node here.
  - **Remove a link:** right-click it → Delete link.
  - **Change a block:** right-click it → Rename..., Duplicate, Disconnect all, Delete.
  - **Tidy up** arranges the blocks.

Undo / redo: **Ctrl+Z** / **Ctrl+Y**. Save: **Ctrl+S**. A `*` in the title means unsaved changes.

## Blocks

**Steps** (they read or write files):

| Block | What it does |
|---|---|
| Original texture | A game texture. |
| Files in folder | Every matching file in a folder; the blocks it feeds run once per file (below). |
| Export image | Writes the texture as an image to edit. |
| Edit image | Your step: the run waits until you click Done editing. |
| Use existing image | An image you already have. |
| Convert image to texture | An image → a texture like the original. |
| Adjust colour, Resize image, Overlay image | Change an image. *Save to* keeps the result as a file. |
| Replace photo | Puts your picture into a photo frame texture, keeping the frame and the photo's ageing. |
| Pick channel, Merge channels | Work on one channel (e.g. the colour) without touching data in another. |
| Mesh mask, Blend in mask | Change only one part of a texture, e.g. a character's trousers. |
| Preview | Shows a picture on the graph. |
| Package for Fluffy | Builds the mod and its zip. |
| Copy / Move / Rename / Delete file, Make folder | File housekeeping. Delete uses the Recycle Bin. |
| Run program | Runs a program or script as a step (below). |

**Utilities** (small grey blocks that only compute): Value (a setting kept in the graph, e.g. your output folder),
Text (fills `{1}`, `{2}`… from what's linked in), Split, Join path, Path parts, Change extension, Cut text, Require
file.

- **One output feeds one input.** To use a value in several places, put a **Split** on its link; it gets a new
  output for each link.
- **Linked fields show their values.** A linked field shows what it will hold, or *known after a run*. Hover a field
  for its whole value.
- **Paths in a graph** are relative to the graph file's folder.
- **Settings at their defaults** fold into one "+ N settings" row; click it to open them.

Image blocks show a live thumbnail. Click it for a larger view, or pop it out into its own window. The wheel zooms,
drag to pan, double-click to fit.

## Many textures at once

Start with **Files in folder** instead of Original texture:
- **Folder and Files:** pick the folder and, optionally, which files (`*_iam.tex*`; empty = every texture).
- **Repeating:** every block it feeds runs once per file. Put **`{name}`** in the file names they write, e.g. Export
  image's `edits\{name}.png`, so each file gets its own.
- **Collecting:** Package for Fluffy takes them all and builds one mod.
- **Editing:** a run exports every image. The **YOUR STEP** card lists them, each with Open and Done, plus **Done
  editing all**. Package waits until all are done.
- **If a file fails:** stop the run (default) or skip that file.
- **Previews:** the ◀ ▶ arrows on the block pick which file the thumbnails show.

## Masks and channels

- **Pick channel** shows one channel as a grey picture. **Merge channels** puts channels back together, e.g. your
  new colour with the original's alpha.
- **Mesh mask** makes a mask from a mesh: white where the named materials (`*jacket*`) sit on the texture. If no
  name matches, the run lists the mesh's materials.
- **Blend in mask** shows the edit where the mask is white and the original where it's black. *Feather* softens the
  edge. Any grey picture works as a mask.

## Custom nodes

Turn blocks you use together into one block of your own:
1. In Build layout, select the blocks.
2. Right-click one → **Make custom node...**, and name it.

The block is saved to your library (`%APPDATA%\remod\nodes`) and listed under *Custom nodes* in the Nodes panel.
A graph that uses one keeps its own copy, so it opens on another PC. Right-click it → **Edit custom node** to change
it. **Input** and **Output** blocks inside are its pins. **Save** updates it; **Back to the layout** returns.

## Browser

- **Tree:** *Pinned* (Game files and folders you pin: right-click a folder → Pin) and *This PC*.
- **Search:** several words narrow it down (`leon albd`).
- **Nicknames:** in the game files, right-click a folder or file and type a readable nickname for a cryptic name
  (`cha000` → Leon). Nicknames are searchable and only label things.
- **Viewer** (bottom left, Use layout): the selected texture large. Picking a mesh shows it in 3D with its textures:
  drag to turn, right-drag to move, wheel to zoom, double-click to reset. Click one of its textures to highlight
  the parts that use it.
- **Transparency** is off by default, because alpha often holds data.

## Run program

Runs an `.exe`, `.bat` / `.cmd`, `.ps1` or `.py` as one step.
- **Program:** a full path, a script next to the graph, or a name on PATH.
- **Arguments:** `{in}` is what's linked into its input, `{out}` its *Output file*, `{name}` the file's name in a
  Files in folder list.
- **Outputs:** what the program printed, and its output file.
- **When it runs again:** only when the program, its arguments or its input changed, or its output file was changed
  or deleted. Tick *Always run* if the result depends on anything else.

The program runs as you and can change any of your files.

## Command line

`remod.exe` runs the same graphs without the app (PowerShell, in the remod folder):

```powershell
.\remod.exe run --graph C:\work\my_mod.json                 # stops at Edit image steps
.\remod.exe run --graph C:\work\my_mod.json --edited true   # after you've edited the images
```

Single steps:

```powershell
.\remod.exe tex2png --profile profiles\re4r.toml --tex C:\work\x.tex.143221013 --out C:\work\x.png
.\remod.exe png2tex --profile profiles\re4r.toml --png C:\work\x.png --original C:\work\x.tex.143221013 --out C:\work\new.tex.143221013
.\remod.exe package --profile profiles\re4r.toml --tex C:\work\new.tex.143221013 `
  --game-path _chainsaw/ui/ui3200/tex/x.tex.143221013 --name MyMod --out E:\mods `
  --author Me --version 1.0 --description "What it does" --screenshot C:\work\preview.png
```

Add `--noesis <path to Noesis64.exe>` to convert with Noesis. `package` refuses to overwrite an existing mod unless
you add `--replace true`. Run `.\remod.exe` alone for every option.

## Letting an AI build graphs (optional)

`remod.exe mcp` is an MCP server. An AI client such as Claude Code can use it to build, check and run graphs with the
same blocks and rules as the app. For Claude Code:

```powershell
claude mcp add remod -- "C:\remod\remod.exe" mcp
```

- **Approval:** a run that would remove a file, write outside the graph's folder or start a program asks **you** in
  a Windows dialog first.
- **Your steps:** Edit image steps wait for you.
- **Game files:** never changed.

## Noesis (optional)

- **Setup:** install Noesis (`winget install -e --id RichWhitehouse.Noesis`) and put the fmt_RE_MESH plugin in
  `Noesis\plugins\python`. The Pipeline panel finds Noesis and says whether the plugin is installed.
- **Textures:** tick **Convert textures with Noesis**. Noesis writes mip levels down to 8×8 whatever the original
  has, so the tool warns you. The built-in converter keeps the original's mip levels exactly.

## Troubleshooting

- **"no 'profiles' folder found":** keep `profiles` next to `remod-app.exe` / `remod.exe`.
- **Path too long:** move the remod, mod and output folders closer to the drive root.
- **A block is red:** hover it for the reason. The Pipeline panel's log has the details.
- **The texture's version isn't known:** only RE4 (2023) is supported so far. Other games need a profile in
  `profiles`.
