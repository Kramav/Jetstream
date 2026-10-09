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
3. Open one of the **examples** (the `...` button next to *Graph file*, in the `examples` folder): eleven ready-made
   mods, from a hand-edited document to recolouring part of a character. `examples\README.md` says what each shows.
   Their game paths start with `{game}`, your Game files folder. They're read-only: **Save** asks where to put your copy.

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

## The layouts

The switch in the bar at the top of the window changes between (it stays in the same place in every layout):

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
| Streaming copy | Finds a texture's high-resolution copy, so the mod replaces both (below). |
| Export image | Writes the texture as an image to edit. |
| Edit image | Your step: the run waits until you click Done editing. |
| Use existing image | An image you already have. |
| Convert image to texture | An image → a texture like the original. |
| Adjust colour, Resize image, Overlay image | Change an image. *Save to* keeps the result as a file. |
| Replace photo | Puts your picture into a photo frame texture, keeping the frame and the photo's ageing. |
| Pick channel, Merge channels | Work on one channel (e.g. the colour) without touching data in another. |
| Part texture | The texture some parts of a mesh use, by material name (e.g. Leon's `Pants_Mat`): no hunting in the Browser. The ▼ beside Materials lists the mesh's materials. *Material file* picks a costume variant (e.g. `cha000_00b.mdf2`). |
| Mesh mask, Blend in mask | Change only one part of a texture, e.g. a character's trousers. |
| Recolour part | Built from blocks: a mesh, its parts' material names and a colour change give both new textures (the texture and its streaming copy), changed on those parts only. |
| Preview | Shows a picture on the graph. |
| Export movie | Copies a game movie to a file for you to edit (below). |
| Edit video | **Your step:** edit the movie and render your edit; the run waits until you click Done editing. |
| Replace movie | A new version of one of the game's movies: your video, or a test card (below), and its sound packages. |
| Replace sounds | Your audio in place of game sounds, each found by the sound id in its file's name (below). |
| Lua script | A REFramework script for the mod, with Test in game (below). |
| Package for Fluffy | Builds the mod and its zip. |
| Copy / Move / Rename / Delete file, Make folder | File housekeeping. Delete uses the Recycle Bin. |
| Run program | Runs a program or script as a step (below). |

**Utilities** (small grey blocks that only compute): Value (a setting kept in the graph, e.g. your output folder),
Text (fills `{1}`, `{2}`… from what's linked in), Split, Join path, Path parts, Change extension, Cut text, Require
file, and the conditions: If, First of, File exists, Text matches, Not (below).

- **One output feeds one input.** To use a value in several places, put a **Split** on its link; it gets a new
  output for each link.
- **Linked fields show their values.** A linked field shows what it will hold, or *known after a run*. Hover a field
  for its whole value.
- **Paths in a graph** are relative to the graph file's folder. A path starting with `{game}` is inside your Game
  files folder (`{game}\_chainsaw\...`), so a graph works on another PC whose game files are elsewhere. Picking or
  dragging a file from your Game files folder stores it that way by itself.
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

## Conditions: steps that only sometimes run

- **If** passes its value on only when its condition is yes. When it's no, it passes on *nothing*: the steps after
  it don't run, write nothing, and show **Not needed** (faded, with the reason). Put it at the start of the branch,
  before any step that writes.
- **Conditions** are their own kind of pin (white triangles): **File exists** (is a file there), **Text matches** (a
  name or path against a pattern such as `*_albd*`; `;` between several) and **Not** (flips one). A condition can
  also tick a checkbox field, such as Package's Replace existing.
- **By hand:** with nothing linked into its condition, an If has a checkbox: a switch for that branch.
- **If / else:** an If on the condition and another on Not of it, both into **First of**, which passes on whichever
  ran.
- **With a list:** an If on a Files in folder chain is a filter. Only the matching files go on, and Package collects
  just those.

Before a run, conditions that can be worked out already (File exists, Text matches) decide which branch the previews
show and which files the run will write.

## Streaming copies

Many RE4 textures (most characters and props) exist twice: the base copy at its own path, and a sharper
**streaming copy** at `natives\STM\streaming\<same path>` that the game swaps in up close. **If a texture has a
streaming copy, the mod must replace both**, or the edit comes and goes with distance. A texture without one (most
UI) is replaced alone.

Two blocks do this:
1. **Streaming copy:** feed it the base texture (from the REtool folder, through a Split). Export and edit its
   **full size** output: the streaming copy if there is one, else the texture itself.
2. **Convert with streaming copy** (in place of Convert image to texture): link in the edited image, the base
   texture, and Streaming copy's **streaming copy (if any)**. Link both of its outputs into Package.

Without a streaming copy, the second output gives nothing: Package gets the one texture, and the block's streaming
conversion shows Not needed. With Files in folder, each texture gets two or one.

## Movies

The game's pre-rendered movies are video files under `natives\STM\streaming\_chainsaw\movie\`, named
`<id>.mov.1.x64`. Movies in `mv` have a 1080p copy beside them, `<id>_fhd.mov.1.x64`. **Replace movie** makes both:
1. **Game movie:** pick either copy in your REtool folder (the one under `streaming`; the file of the same name
   outside it is only a stub).
2. **Your video:** any video Windows plays (mp4, mov, wmv...). It's scaled to the movie's size, with black bars if
   its shape differs, and retimed to the movie's frame rate. **Leave it empty for a test card:** the movie's name
   and the seconds counting, as long as the original. Use that to find out where the game plays a movie.
3. **Same length as the original** (on by default): a longer video is cut, a shorter one holds its last frame, so
   the movie lasts as long as the original. Some movies' sound isn't in the file: the game plays it from its sound
   packages, timed to the original, so a different length would put picture and sound out of step.
4. **Sound in the file:** a movie with sound in its file (the logos `mv7000` / `mv7001`, and `mva300` / `mva301`)
   gets your video's sound, re-encoded to match (AAC, 48 kHz stereo), cut or padded with silence to the picture's
   length. Your video has none: silence. A test card for one of them beeps each second, so you can hear whether the
   game plays the file's sound.
5. **Sound in the game's sound packages** (the story movies `mva000`, `mva201`, `mva202`): **Replace its sound** (on
   by default; it shows under "settings at their defaults") makes new packages. The music, or for a movie without
   music its effects, gets your video's sound (a test card's: a beep each second); every other sound, the dialogue
   in every language too, becomes silence, so the original's sound doesn't play over your movie. Each new sound keeps
   its original's format and length, so the game's sound bank still fits. Turn it off to keep the game's own sound.
   The other movies have no sound packages (and no sound track): nothing to do.
6. Link all three outputs (**new movie**, **1080p copy**, **sound packages**) into Package's **other file** inputs.

Encoding takes a while for a 4K movie (about 20 s for the 61 s intro on a PC with a hardware encoder). A run with
nothing changed reuses it. After a run, a movie block's **Preview movie** button (Use layout) plays its movie in the
viewer: Replace movie's new movie, Export movie's copy, Edit video's edit. Example `12_replace_a_movie` is the test
card for `mva000`, with its sound packages beeping, ready to run.

### Editing a movie by hand

1. **Export movie** copies the game's movie (the full-size one) to its **Video file**, e.g. `edits\mva000.mp4`.
2. **Edit video** is your step. Open the copy in your video editor (**Open with** can name it), edit, and render
   (export) the result to **Your edit**: by default `<video>_edited.mp4` beside the copy. Click **Done editing**.
3. **Replace movie** takes the edit as **Your video**.

Feed the same movie to Export movie and Replace movie from one Value through a Split. Your copy is kept between
runs; picking another movie exports again and the edit step waits again. Example `13_edit_a_movie_by_hand` is the
whole chain, for `mva402`.

Not known yet (needs a test in game): whether a movie of another length plays fully (turn off **Same length** to
try); whether the game plays a replaced file's sound (the logos) and a replaced sound package's (example 12's
beep); which movie plays where.

### Adding a movie of your own

**New movie** adds your video to the game as a movie of its own, beside the game's: none of the game's movies is
replaced. A cutscene then plays it whenever you choose (see Cutscenes below).
1. **Movie name:** 6 lowercase letters, digits or `_`, not starting with `mv`, e.g. `rmd001`. Each new movie in a
   mod needs its own name. (It has to be 6 characters: the new movie's files are copies of the game's `mva000`
   files with the name swapped in, letter for letter.)
2. **Your video:** any video Windows plays, at its own length. Empty: a test card with the name and the seconds.
3. Link **new movie files** into Package's **other file**, and add a **Cutscene** whose file lists the movie:
   `"movies": [{"t": 0.5, "id": "rmd001"}]`. In game the movie also has a **Play** button in REFramework's menu
   (Script Generated UI > remod cutscenes).

It needs **Game files** set (it copies files from `mva000`), and players need REFramework. The video's sound plays
with the movie, in mono, from a sound bank the block makes. The game doesn't play sound inside a new movie's own
file. Example `16_insert_a_new_movie` is the whole thing, with a test card and F8. How it works:
`docs\re4r_movies.md`.

### Adding a sound of your own

**New sound** adds your audio to the game as a sound of its own, beside the game's: none of its sounds is replaced.
1. **Sound name:** 1 to 32 lowercase letters, digits or `_`, e.g. `door_creak`. Each new sound in a mod needs its
   own name.
2. **Your audio:** anything Windows plays (wav, mp3, m4a, wma, flac, or a video's sound), at its own length.
   Empty: 3 seconds of beeps.
3. Link **new sound files** into Package's **other file**, and list the sound in a cutscene:
   `"sounds": [{"t": 0, "id": "door_creak"}]`. It starts at that time and stops when the cutscene ends, so make the
   cutscene at least as long as the sound. In game each new sound also has a **Play** button in REFramework's menu.

New sounds play in mono for now: they're made from one of the game's own one-sound banks, and no stereo one exists
to copy. Example `17_add_a_new_sound` plays `beep.wav` when you press F7.

## Sounds

The game's sounds are in Wwise sound banks (`.sbnk`) and sound packages (`.spck`) under
`natives\STM\_chainsaw\sound\wwise\`. Each sound has a number, its **id**.

1. **Find it.** Click a bank or package in the Browser (they're in Game files; **Show: Sound files** under the
   search box hides every other folder, and **Language** the other languages' files). The viewer lists its sounds:
   id, length, channels. **Play** plays one. A line that says *streamed: its first part* is a longer sound kept in a
   package; Play plays all of it. Click an id to copy it.
2. **Drag its line onto the graph.** That adds a **Game sound** block holding it, linked into your Replace sounds
   block if the graph has one (this works in Use layout too). Dropped on a Game sound block's field instead, it
   changes which sound that block replaces.
3. On the Game sound block, pick **Your audio**: any audio Windows plays (wav, mp3, m4a, wma, flac, even a video's
   sound). It's converted to the original's format, channels and sample rate. Leave it empty to silence the sound.
4. Link **Replace sounds**' **sound files** into Package's **other file**. Use one Replace sounds block for all your
   sounds.

For many sounds at once, Replace sounds also takes audio files named by the sound's id (**Save as WAV** names them
so; `leon_line_880852580.wav` is fine: the last run of 4 or more digits is the id), e.g. from **Files in folder**.

Everything that holds or describes a sound is rewritten to match: its package (both copies), its bank, the first
part of a streamed sound that a bank keeps so it starts at once, and the sizes the bank records. A sound can be
longer or shorter than the original (**Same length as the original** off, the default). A sound in one of the game's
music tracks always keeps the original's length, because the track records it.

Example `14_replace_a_sound` replaces the intro's English narration with `beep.wav`. From the command line,
`remod sounds --file <bank or package>` lists the ids and `remod sound-wav --file <...> --id <id> --out x.wav` saves
one.

Not known yet (needs a test in game): whether a sound of another length plays fully.

## Scripts (REFramework)

A mod can include a Lua script for [REFramework](https://github.com/praydog/REFramework), which players then need
installed.

1. Add a **Lua script** block and pick your `.lua` file, e.g. `scripts\my_mod.lua`. Modules it loads with
   `require` go in a folder named like it beside it (`scripts\my_mod\`); they come along.
2. To try it without packaging: set **Game folder** in the Pipeline panel (where the game is installed), click
   **Test in game**, then in game press **Reset Scripts** in REFramework's menu (Insert). REFramework doesn't reload
   a changed script by itself: after each change, Test in game and Reset Scripts again. **Remove from game** takes
   it out. **Scripts in the game**, under Game folder, lists every script the game runs, whoever installed it, with
   **Remove** (to the Recycle Bin, with its folder); a script a mod manager installed is better uninstalled there.
   REFramework's log is `re2_framework_log.txt` in the game's folder (for every game).
   **Game errors** reads your script's errors from that log since Test in game and shows them under the block; **Fix
   with Claude** asks Claude to fix them (or fix them yourself: **Open in editor**). REFramework only writes Lua errors
   to its log with **Log Lua Errors to Disk** on, in its ScriptRunner menu (off until you turn it on, once). The log
   starts empty each time the game starts.
3. Link **script files** into Package's **other file**. The script goes in the mod's `reframework\autorun\`, and the
   mod's description says "Needs REFramework."

**Letting Claude write it (optional).** With [Claude Code](https://claude.com/claude-code) installed and signed in
(the VS Code extension's copy is found too), the Lua script block in Use layout has a box: describe what the script
should do (or, once it exists, what to change) and click **Write it with Claude** / **Ask Claude**. Claude reads the
script first, looks up every game name in your SDK dump, checks the script, and answers in a minute or a few; its
note shows under the box and the status line gives the cost. The script stays an ordinary `.lua` file: **Open in
editor** opens it to read or change yourself (set **Open with**, under the block's other settings, to use e.g. VS
Code). Claude only reads; remod writes the file, keeping the previous version as `my_mod.lua.bak`. If you save the
file yourself while Claude is working, yours is kept and Claude's answer goes to `my_mod_claude.lua` instead.

**Checking a script.** Every Run checks the block's scripts. A syntax error stops the run with the file and line. With
an **SDK dump** set in the Pipeline panel, every game name the script passes as text is checked too
(`sdk.find_type_definition("chainsaw.PlayerManager")`, `:get_method("...")`, `:get_field("...")`, `:call("...")`):
one the game doesn't have is a warning, with the nearest real names. The SDK dump is the game's code as REFramework
sees it. Make it once per game update: with REFramework installed, in game open REFramework's menu, then
DeveloperTools > ObjectExplorer > **Dump SDK**. It writes `il2cpp_dump.json` to the game's folder (it may crash near the
end; the file is still written). The first check reads it, which takes a while; later ones use a copy of what's needed.
From the command line: `remod check-lua --file my_mod.lua`.

Not known yet (needs a test): whether Fluffy installs a mod's `reframework` files into the game's folder.

## Cutscenes (REFramework)

A **Cutscene** block makes a real-time cutscene: the game itself renders it, while remod's cutscene script moves the
camera, plays Leon's animations, and draws subtitles, letterbox bars and fades. A cutscene is a small JSON file you can
edit by hand (the format: `schemas\cutscene.v0.example.json`). Players need REFramework.

1. Add a **Cutscene** block and type a file name for it, e.g. `cutscenes\door.json` (it needn't exist yet).
2. **Record the camera shots in game:** click the block's **Test in game** (with no cutscene file yet, it installs
   just the cutscene script, which is all recording needs), then in game frame each shot with REFramework's free
   camera and press **F10** at each one. The time between presses becomes the time between shots.
3. Click **Use recording**: the shots go into your cutscene file (anything else in it stays; the previous version is
   kept as `.bak`). **Open in editor** to add subtitles, fades, a start key and so on.
4. **Test in game**, then in REFramework's menu: Script Generated UI > remod cutscenes > **Play** (or the cutscene's
   start key). **Game errors** shows the cutscene script's errors.
5. Link **cutscene files** into Package's **other file**. Several cutscenes can go in one mod.

A Run checks the cutscene file and names every problem in it.

**Other characters (actors).** A cutscene's `actors` list puts other characters in it, such as Luis, even where the
level hasn't loaded them. Each is a copy (a "puppet") built from the game's files; the real one isn't touched.
Each actor has:
- a `name`, which `motions` use to animate it (`"actor": "luis"`);
- a `puppet`: a definition in `reframework\data\remod_puppets`. remod ships `luis` and `ashley` and packages the
  ones a cutscene uses; a mod that adds a new character ships its own;
- where it stands: either `offset` [right, up, forward] metres from Leon, facing him (default 1.5 m in front), or
  `position` and `rotation` from **Write down Leon's spot** in the remod cutscenes menu
  (`remod_cutscenes\spot.json`);
- optionally `"hides": "partner"`, which hides the real partner while it plays.

The actors appear when the cutscene starts and are put away when it ends (seen in game, 2026-10-08).

**Where cutscene files are.** A cutscene file lives wherever you put it: the Cutscene block's **Cutscene file** names
it (the examples' are in `examples\cutscenes`). **Test in game** copies it into the game, to
`<game>\reframework\data\remod_cutscenes`. The runtime also writes what you note in game there: `recording.json`
(F10's camera keys), `trigger.json`, `spot.json` and `animation.json`. The Browser's **Cutscenes**, at the top of its
tree, lists both:
- **This graph's:** its Cutscene blocks' files.
- **In the game:** the copies there, and what was written down, with how long ago. Click **recording** or **trigger**
  to use it in the open cutscene.
- **New cutscene...** makes a new file and opens it.

Anywhere else in the Browser, a cutscene file is marked "(cutscene)"; click it to open it.

**The Cutscene layout.** The third layout beside Use and Build. **Cutscene layout** in the top bar shows the cutscene
you opened last, or a new one if none is open: start making it straight away, and **Save** asks where to keep it.
Opening a cutscene (from the Browser, or **Edit cutscene** on a Cutscene block) switches to it too. No JSON to type.
- **Preview** (top left): remod's part of the screen at the playhead (the letterbox bars, fades to black and the
  subtitle), as the runtime draws them. The game's picture, the camera and the characters only show in game.
- **Stage** (top right): who stands where, seen from above, Leon facing up. Drag an actor to move it (0.1 m steps;
  hold Shift for finer).
- **Play** / Space plays the playhead in real time; the line under it says what the camera and each character are
  doing then.
- **Timeline:** a lane each for camera keys, fades, subtitles, Leon's animations, each actor's, movies and sounds.
  - Click an item to select it.
  - Drag it to move it; drag a fade's or subtitle's end to stretch it. Times snap to 0.05 s; hold Shift for none.
  - Right-click a lane to add something there.
  - Delete removes the selected item.
  - Click or drag the seconds to move the playhead.
  - Camera keys can be moved, eased (smooth, linear, cut) or removed. Where the camera is still comes from recording
    in game.
- **Cutscene item** (right): the selected item's settings. With nothing selected, the cutscene's own: name, length,
  start key, letterbox, **Use recording**, **Use trigger** and the actors. Click an actor's lane name, or the actor on
  the stage, for its settings: puppet, near Leon or at a spot (**Use Leon's spot**), and whether it hides the partner.
- **Every item (tables):** everything as numbers, to type exact values.
- **Add picked animation** adds the animation you picked in game at the playhead.
- **Animation files** (the cutscene's panel, with nothing selected): animations from the game's own cutscenes,
  which aren't among a character's usual ones. Each is a file such as
  `_Chainsaw/Event/cs/csa012/csa012_s00/chara/cha300_00/cha300_00.motlist` (Luis in cutscene csa012), put on a
  character under a bank number of your own (9000, 9001, ...) when the cutscene plays. Easiest: in game, load the
  file in the Animations menu (**Add this animation file**), pick an animation, **Use in a cutscene**, then **Add
  picked animation** here: the file comes along. These animations move the character as the game's cutscene did,
  through walls too, so give them room.

Problems show at the top as you edit. Ctrl+Z / Ctrl+Y undo and redo. **Save** (Ctrl+S) writes the file and keeps the
previous one as `.bak`. **Test in game** saves and copies it into the game. **Add to graph** puts a Cutscene block for
it into your graph, linked into Package. **Use layout** or **Build layout** in the top bar returns to the graph;
**Cutscene layout** comes back to the cutscene, still open.

**Finding animations (in game).** REFramework's menu > remod cutscenes > **Animations**:
1. Pick **who**: Leon, or a puppet, which comes out in front of him.
2. Optional, for a game cutscene's own animations: **Animation file** (a `.motlist` path; the box suggests one for
   Leon and Luis), **As bank** (your own number, e.g. 9000), **Add this animation file**. A second later the line
   under it says how many animations it brought; the bank is then in the list. In the extracted game files they're
   under `natives\STM\_chainsaw\event\cs\<cutscene>\...\chara\<character's mesh>\`.
3. Pick one of its **banks**, then an animation from the list (numbers and the game's names; **Find** narrows it).
   It plays at once, looping, with a frame slider and Pause.
4. **Use in a cutscene** writes it down (with its animation file, if any) for the editor's **Add picked animation**.

While Leon previews one he's held still, as in a cutscene.

**New characters.** The **New character** block makes a character of your own from one of remod's (ashley, luis):
- one part (its body, by default) becomes new files, with its colour textures recoloured;
- **Leave as they are** keeps some, e.g. `*hand*` for bare skin;
- **Colour** and **Strength** set the new colour.

Nothing of the game's is replaced. Its **Name** must be as long as the game's folder it takes the place of (6 letters
or digits, e.g. `rmc002`). Link its files into Package; a cutscene's actor then uses it as its puppet. Example 19 does
both.

The cutscene probe (`spikes\cutscene_probe.md`, run 2026-10-07) showed the camera holds still and Leon's animations
play from the script in RE4R. While a cutscene plays, Leon stands still and the HUD is hidden; both come back when it
ends or you stop it. While it plays the game ignores your controls for Leon (as it does while its own menus are
open), so he stops by himself even if you're running when it starts.

**Animations in a cutscene** (`motions`: a time, then the animation's `bank` and `motion` numbers): to find an
animation's numbers, open REFramework's menu > Script Generated UI > remod cutscenes while playing. It shows "Leon's
animation now", e.g. `bank 1000, motion 160, frame 76 of 2433`: do the action in game and note the numbers.
The HUD is hidden through the game's own Display HUD option: if the game ever crashes during a cutscene and the HUD
stays hidden, set it back in the game's options.

**The game's movies in a cutscene** (`movies`: a time, then the movie's `id`, e.g. `{"t": 0.5, "id": "mva000"}`): the
movie plays full screen as the game plays its own, with the game paused (enemies too) and Leon held. The cutscene's
time waits while it plays, then goes on from that moment, so anything after it in the file happens after the movie.
Fades and subtitles aren't drawn over the movie itself; a fade that's black when the movie starts covers its loading.
Example `15_play_a_game_movie` fades out, plays the intro movie and fades back in. Movie ids are the game's movie names
(`mva000`, `mva201`, ...), or the name of a movie of your own from a **New movie** block (example
`16_insert_a_new_movie`). The game's play-time clock keeps running while a movie plays.

**Sounds in a cutscene** (`sounds`: a time and a **New sound** block's name, e.g. `{"t": 2, "id": "door_creak"}`):
the sound starts at that time, beside everything else, and stops when the cutscene ends.

**Cutscenes that start by themselves** (`trigger`): the cutscene starts when everything its trigger names is true.
You make the trigger in game (REFramework's menu > Script Generated UI > remod cutscenes); each button there adds a
condition to `trigger.json`, and the menu shows what it holds so far (**Start a new trigger** empties it):
- **Make a trigger here:** Leon's spot, plus the game's names for the chapter and stage he's in (this one starts the
  file afresh).
- **Characters near Leon > Talk trigger:** talking to that character. Near him, "[G] Talk" shows at the bottom of the
  screen, and G starts the cutscene: a new conversation with the merchant, say. The characters are listed by the
  game's names for their kind (the merchant is probably `ch3_a8z0`), nearest first.
- **Story flags:** the game's flags for what has happened in the story. Tick **Watch** and play: flags that change are
  listed, so you can see what a moment sets, and **Add** one (it must be on; one that went off must be off). **Find a
  flag** searches them by name.
- **Start after it:** right after the game's own movie or cutscene that just ended (shown by name).

Then in remod, **Use trigger** (the Cutscene block, or the Cutscene layout with nothing selected) puts it into the
cutscene file (the rest stays; the previous version is kept as `.bak`).

The trigger's parts, all optional but at least one needed:
- `near`: the spot and a `radius` in metres;
- `chapter`, `stage`, `area`, `location`: the game's names, as the menu's **Now** line shows them where you stand;
- `talk`: `npc` (the character's kind, or `merchant` for the merchant), and optionally `key` (A-Z or F1-F12 but F10; T by default), `prompt` (the
  words after the key, "Talk" by default) and `radius` (2.5 m by default);
- `flags`: story flag names that must be on; one starting with `!` must be off;
- `after`: `{"movie": "mva000"}` or `{"event": "csa012"}`, for the 10 seconds after that one ends;
- `delay`: seconds the conditions must hold first;
- `once`: `true` (the default) once per save: loading a save from before it played lets it play again; `"session"`
  once each time you start the game; `false` every time. A `talk` trigger plays every time by default (its prompt
  comes back when the cutscene ends); give it `once` to limit it.

It doesn't start while another cutscene plays, the game plays one of its own movies, or the game is paused. It
starts when the conditions become true (a talk trigger: when you press its key then), and not again until they've
stopped being true. Example
`18_start_a_cutscene_by_itself` starts at a spot in chapter 1's village.

## Built from blocks

Some blocks are made of other blocks, such as **Convert with streaming copy**. They're listed in the Nodes panel under
**Built from blocks**. Right-click one and pick **Edit custom node** to see the blocks inside and how they're linked.
Change it and **Save** to keep your own version in your library; yours is used from then on. They work exactly like
your own custom nodes (below).

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
  the parts that use it. Clicking a movie (a game movie, or any mp4, mov, wmv...) plays it there: Play / Pause, and
  drag the time bar to jump. The game's movies are under **Game files → streaming → _chainsaw → movie**.
- **Game files → streaming** holds the game's high-resolution texture copies and its movies. It's browsed as it is on
  disk: search doesn't reach into it (search inside it filters the folder you're in).
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
- **Scripts:** it can write REFramework scripts for you. It looks up the game's real names in your SDK dump and checks
  the script until it's clean (see Scripts above). Whether the script does the right thing in game is for you to test.

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
