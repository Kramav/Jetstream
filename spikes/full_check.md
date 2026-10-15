# Full check

Everything remod has built that only you can check: in the app by eye, and in the game. It's the RE4R release pass
(CLAUDE.md §9's lists) in one go, plus the cutscene items parked since 2026-10-09.

**Each check has an id** (A3, T2, M5 ...). Tell me the id and what you saw, e.g. `T2 yes`, or
`M2 no: the movie stopped after 10 s`. You can reply in the chat, or type into
`spikes\out\full_check\results.txt` and tell me it's there. Skip anything you like: a skipped check stays open, never
counts as passed.

**I read the rest myself:**
- the game's REFramework log (saved after each session, below);
- what's in the game folder (installed and removed files);
- the run outputs.

So you only report what's on screen, heard, or felt (a crash, a lag).

**Time:** about 4 hours in all, in as many sittings as you like, in any order. The parts don't depend on each other.

**Ready-made (by `spikes\make_full_check.py`):**
- `spikes\out\full_check\mods\`: every mod to install, named by game session (`S1 - My shirt.zip`).
- `spikes\out\full_check\graphs\`: the examples plus the check's own graphs (`x1` to `x10`). Open graphs from
  here, not from `examples\`: you can save these, and their runs stay here.
- `build-release\remod-1.0.0-win64.zip`: the release zip, built today.

---

## Part R: the release zip (10 minutes)

1. Unzip `build-release\remod-1.0.0-win64.zip` into a new empty folder, e.g. `Desktop\remod-zip-test`.
2. Start `remod-app.exe` from there.
   - **R1** Does it start without an error?
   - **R2** Does the game picker (top bar, left) list **RE4R only**?
3. In the Pipeline panel, set **Game files** if it's empty. Open `examples\re4r\01_edit_a_texture_by_hand.json` from
   that folder and click **Run**. It waits at *Your edit*: click **Done editing**, then **Run** again.
   - **R3** Does `examples\re4r\mods\My first texture mod.zip` appear in the unzipped folder?
4. **R4** (optional) Do the same on a PC without Visual Studio. Copy the zip over, plus the REtool `natives\stm`
   folder or one texture.
5. Close it, and delete the folder.

---

## Part A: the app, by eye (1-1.5 hours)

Use the remod you normally run (`build\app\remod-app.exe`). For each check, say whether it works and whether anything
looks wrong or ugly. "Looks bad" is a useful answer; say what.

### The graph's look (open `graphs\07_edit_a_character_texture.json`, Build layout)
- **A1** Blocks: a coloured band and glyph per family (sources steel blue, transforms violet ...), drop shadows. Pins:
  texture square, image circle, text or path diamond. Small blocks (Split a dot; Text `{ }`; Value `=`) are short
  symbols. Readable at 100%?
- **A2** Drag a new link from an output. Is every pin it fits ringed in blue? Is the line drawn right-angled in its
  kind's colour, with red dashes over a pin that refuses it? Drop it on nothing to cancel.
- **A3** Open `graphs\03_your_picture_in_a_photo_frame.json`. *Replace photo* has a grip with "+N" on its bottom
  edge. Drag it down: do the hidden settings (ageing, scratches ...) appear one by one? Up: do they fold again? A click
  shows all or none. Save, reopen: are as many showing?
- **A4** Open `graphs\05_many_textures_at_once.json`. Are the links after *Files in folder* hollow double lines
  (lists)?
- **A5** Zoom out past about 55%, or click **Far**. Do blocks fold, then become small symbols? Does every line keep the
  same side of each block as it had close up (none suddenly passing under a block)? Select a block and click
  **Near**: does it zoom to that block?
- **A6** On `graphs\20_a_picture_in_every_photo_and_painting.json`, click **Tidy up**. Is the layout clear: no line
  hugging a block's side, no line looping round a block? The line above the graph counts links without a clean
  route: what does it say?
- **A7** The **Nodes** panel (bottom, Build layout):
  - Are the folders closed until opened, and does a search open them?
  - Is there a **Built from blocks** folder (Convert with streaming copy, Recolour part)?
  - Drag a block onto the graph: does a see-through copy follow, and land exactly where you drop it?
- **A8** Uncheck, then check, **Descriptions** above the graph. Do the block descriptions hide and come back?

### Blocks and buttons
- **A9** **A custom node.** In `graphs\02_recolour_a_texture.json`:
  1. Select *Adjust colour* and *Convert image to texture*, then right-click and choose **Make custom node...**. Title it
     `My recolour`.
     - Does one block replace them, with the links kept?
     - **Run**: does it still package?
  2. Right-click it, **Edit custom node**. Does its inside open? Change the hue, **Save**, then **Back to the
     layout**.
  3. Is `My recolour` in the Nodes panel's **Custom nodes** folder?
  4. Add it to `graphs\x1_stamp.json` and save that graph. Edit `My recolour` again from `02`, change the hue once
     more and save it.
  5. Reopen `x1_stamp`: does the Pipeline panel offer **Use the library's**?
- **A10** In `graphs\24_a_new_material_on_a_character.json`, *Part texture*'s **Materials** field has a ▼. Does it
  list Leon's materials (`Pants_Mat`, `Shirts_Mat` ...)?
- **A11** Run `graphs\08_only_the_colour_textures.json`.
  - Are the blocks for the textures that aren't colour faded, with a faint outline and **NOT NEEDED**?
  - Are the links into them sparse dots?
- **A12** In `08`, add a **Value** block (Nodes > Values) and link it into an *If* block's **Condition**. Does the
  Value become a checkbox?
- **A13** **A list of hand edits:** `graphs\x10_batch_edits.json`.
  1. **Run**. It exports several documents and waits.
     - Does the **YOUR STEP** card list each document, with **Open** and **Done** / **Undo** on each, and **Done
       editing all**?
     - Do *Files in folder*'s ◀ ▶ switch which document the thumbnails show ("Previews: ... (n of N)")?
  2. Mark two Done, **Run**: do those two go on while *Package* still waits?
  3. **Done editing all**, **Run**: does it package?
- **A14** `graphs\01_edit_a_texture_by_hand.json`: **Run**, then on the *Your edit* card, **Open in editor**.
  - Does your image editor open `edits\file_008.png`? It already has a red "REMOD CHECK 01" on it: that's my edit,
    for the game test.
  - Close the editor, **Done editing**, **Run**.
  - Then **Edit again**: does the card come back?
- **A15** Thumbnails.
  - In `03`, does *Replace photo* show a thumbnail, and its Frame width say "auto (~91 px)"?
  - Click a thumbnail (Use layout): does it show in the Browser's viewer? Does **Pop out** open a window you can zoom
    (wheel) and pan (drag)?
- **A16** `graphs\20`: does *Name pictures* look right?
  - After a **Run**, does `graphs\edits\named pictures\matches.txt` say which picture went where?
  - Is the run unchanged on a second **Run**?

### The Browser
- **A17** The Browser:
  - Pin a folder (right-click > **Pin**).
  - Browse This PC.
  - Type a path in the address bar.
  - Right-click a game file > **Name...**: does the nickname show beside it, and is it found by search?
- **A18** Drag a texture from the Browser onto *Original texture*'s field in any graph. Does it fill, written as
  `{game}\...`?
- **A19** Click `_chainsaw\character\ch\cha0\cha000\00\cha000_00.mesh` in Game files.
  - Does Leon show in 3D with his textures, with the group checkboxes working?
  - Click one of his textures below: are the parts not using it dimmed?
- **A20** **Sounds.** Under the Browser's search box choose **Show: Sound files** and **Language: en**.
  - Does the tree shrink to sound banks, and keep the folder you were in?
  - Click `ch_mva000_dialogue.sbnk.1.x64.en`. Does the Sounds list show it? **Play** / **Stop** on a line.
  - **Save as WAV** on a line: does a `.wav` appear and play in Windows?
  - Open `graphs\14_replace_a_sound.json` and drag a line from the Sounds list onto the graph. Does a *Game sound*
    block appear, linked into *Replace sounds*? Dropped on the existing *Game sound*'s field instead, does it replace
    that block's sound?
- **A21** **Movies.** Game files > `streaming\_chainsaw\movie\mv\mva402`, click `mva402.mov.1.x64`. Does it play in
  the viewer, with Pause and seeking on the time bar? Then open `graphs\x4_short_intro.json` (already run). Does
  *Replace movie*'s **Preview movie** play its result (a test pattern)?
- **A22** **Editing a movie by hand:** `graphs\13_edit_a_movie_by_hand.json`.
  1. **Run**. It copies `mva402` to `edits` and waits at *Edit video*.
  2. On its YOUR STEP card, **Open in editor**.
  3. Save any short video as `graphs\edits\mva402_edited.mp4`. Rendered from your video editor is best; a copy of
     `graphs\short_intro.mp4` renamed is fine.
  4. **Done editing**, **Run**. Does it encode and package? Does **Preview movie** show your video?

### AI
- **A23** Pipeline panel, **AI images (optional)**: are the AI tool, model and VAE fields filled with yours? Hover
  **Download stable-diffusion.cpp**: does its tooltip make sense? Optional: click it, pick a new empty folder, and
  say whether it downloads and fills the AI tool field (then set your own back).
- **A24** `graphs\23_an_ai_photo_in_a_frame.json`. Its AI steps are cached, so **Run** takes seconds.
  - Thumbnails: *Generate image* shows an old photo. *AI cleanup* with **Show the area** ticked shows a red band
    round the photo's edge.
  - *Keep big changes*: does **Show the heat map** switch its thumbnail between the result and a red-yellow map
    along the photo's edge?
- **A25** `graphs\24_a_new_material_on_a_character.json`.
  - Does *Make material* show the khaki swatch, and *Lay material on* the trousers in khaki?
  - Drag **Scale** to 200%: does the pattern in the thumbnail get bigger?

### Scripts and cutscenes (no game needed)
- **A26** Pipeline panel: are **Game folder** and **SDK dump** filled? Is **Scripts in the game** there (empty now) in
  the Use, Build and Cutscene layouts?
- **A27** The Cutscene layout: open `graphs\x8_my_own_cutscene.json` and click **Edit cutscene** on the block. **Is
  its look good enough for a release? If not, what's bad?** (In October you said "looks bad but good enough for now";
  I'd like the specifics before redesigning it.)
- **A28** Undo and save:
  - Ctrl+Z / Ctrl+Y undo and redo a block move and a field change.
  - A `*` shows in the title when there are unsaved changes, and closing asks.
  - Open `examples\re4r\02_recolour_a_texture.json` itself (the shipped one): does **Save** ask where, instead of
    saving over it?

---

## Part G: in the game (5 sessions, 2-2.5 hours)

**Before each session:**
1. In Fluffy, uninstall the previous session's mods, and any old remod test mods (probes, examples, `remod ...`).
   Your own other mods can stay unless they change Leon's clothes, documents, menus or movies.
2. Install that session's zips from `spikes\out\full_check\mods\` (the names start with `S1 - `, `S2 - ` ...).
   Fluffy's order doesn't matter: nothing in a session overlaps.

**After each session:** quit the game, then in a terminal in the repo run
`spikes\full_check\save_log.cmd <session number>`. The game empties REFramework's log at every start, so this keeps
it for me. If you forget, tell me before starting the game again and I'll copy it.

REFramework must be installed for sessions 3 and 5. In-game texture and sound checks don't need it.

### Session 1: textures, built-in converter (30 minutes)
Mods: `S1 - My first texture mod`, `S1 - My shirt`, `S1 - My pictures`, `S1 - Khaki trousers`, `S1 - Stamp check`.

- **T1** Inventory > Files: the page headed **"Petición oficial"** with a red wax seal (an early file).
  - Does it have a red **REMOD CHECK 01** across its middle?
  - Is the rest of the page as before: sharp, not shifted or smeared?
- **T2** Leon's shirt has bright **red bands** all over. Look at him close up, then from 10+ m away, e.g. from the
  far side of a room.
  - Are the bands there at both distances? That needs both copies replaced: the streaming copy close up, the base
    far away.
  - Any shimmering or flicker as you move?
- **T3** His trousers are **khaki canvas**:
  - Do the folds, dirt and wear still show through?
  - Where pieces of cloth meet (side seams, crotch, knees), does the weave jump or change size? Say where.
  - Does the canvas look the same size on both legs and at the waist?
- **T4** Photos and paintings (example 20). The 10 photos in Files and the paintings in houses (village, chapter 1-2)
  and the castle show **numbered sample cards**.
  - Is each card upright, whole, and in its own frame (no card cut off or showing part of another)?
  - Note any painting still the original.
- **T5** The **Completed** stamp on a finished merchant request (the blue notes) should be **magenta**, not green. Is
  its shape intact, not shifted, skewed or striped? Skip it if you can't finish a request soon.

### Session 2: more textures (20 minutes)
Mods: `S2 - Stamped document`, `S2 - Green trousers`, `S2 - My AI photo`.

- **T6** The "Petición oficial" page has a small **red no-entry sign** (a red ring with a bar) stamped on it (example 4).
- **T7** Leon's trousers are **green**, and nothing else on him changed (example 6: Recolour part).
- **T8** The old framed photo in Files (the one examples 3 and 23 use) holds an **AI-made sepia castle photo** in the
  original frame.
  - Is the frame's edge clean all round: no light gap, no bits of the old photo, no smear?
  - Does the photo look like part of the document?

### Session 3: movies, the cutscene runtime, channels (40 minutes, REFramework)
Mods: `S3 - Movie cards`, `S3 - Short intro`, `S3 - Movie moment`, `S3 - New movie`, `S3 - New sound`,
`S3 - After the opening`, `S3 - Channel check`.

`Movie cards` puts a test card in place of every game movie except `mva000`. The card shows the movie's name and the
seconds, and beeps each second where the movie has sound. `Short intro` makes `mva000` a 15-second test pattern
with a steady tone.

- **M1** Start the game. Do the logos at start show **test cards** (`mv7000`, `mv7001`)? Does `mv7001` beep each
  second? Does the title screen follow normally?
- **M2** **New Game.** Does the opening movie show a **15-second test pattern** with a **steady tone**, played to the
  end (not cut at 15 s, not held at 61 s, no freeze), then the game going on?
- **M3** Right after that movie, does the subtitle **"remod check: this started after the opening movie"** show for a
  few seconds?
- **M4** **Which movie plays where:** any time a test card appears (now or later this session), write down the name on
  it and where it played (a tutorial, a screen in the world, a story moment).
- **M5** Once you control Leon, press **F9** (example 15).
  - A fade to black, then the opening's test pattern full screen with the game paused, Leon held and no HUD.
  - Then a fade back. Does play resume exactly where it was: position, enemies, timers?
- **M6** **F8** (example 16): our own movie `rmd001`, a 12-second test card that beeps each second, then back to play.
- **M7** **F7** (example 17): three beeps.
- **M8** **Channel check:**
  - Leon's shirt has its colour as normal but its **alpha all 0**: does it look different? Shinier, metal-like,
    darker, or nothing?
  - His trousers' normal map is **flat in red and green**: do their folds look flatter, with light no longer catching
    the creases?

  This tests what those channels mean (CLAUDE.md §9's table). "No visible difference" is a real answer.

### Session 4: sounds (15 minutes)
Mods: `S4 - Sound test`, `S4 - Menu sounds`.

- **S1** **New Game** with the English voice language: the original opening movie plays. Where Leon's narration was,
  do you hear **beeps** instead? Are the music and effects unchanged?
- **S2** `Menu sounds` gives every one of the game's 211 menu sounds (those held in its bank, `ch_ui_ingame_media`)
  **three rising beeps over 3 seconds**: longer than nearly all of them. Open the inventory, move the cursor, open a
  file, close it.
  - Do you hear the beeps, and **all three each time** (a sound longer than the original, played fully)?
  - Does any menu sound still sound like the original? Any crackle, or a cut-off?
  - Leave quickly: it's loud.

### Session 5: cutscenes, talk topics, scripts (45 minutes, REFramework)
Mods: `S5 - Talk topics`, `S5 - Trigger test`, `S5 - New character`, `S5 - Event animation`.
You need a save with Ashley following Leon and a merchant near.

**Talk topics** (example 21; this is also the first run of the cutscene script since it was split into a shared
part and an RE4-only part):
- **C1** Walk up to Ashley. Does the game's Interact icon show over her head? Press Interact: does a **Talk** list
  open with only **How are you holding up?**, Leon standing still?
- **C2** Press back (Escape or the pad's back). Does the list close and Leon move again?
- **C3** Open it again and pick the topic (Enter or the pad's confirm). Does its short cutscene play (a subtitle and
  fades)?
- **C4** At the merchant: is **Talk** in his menu (after Buy / Sell / Upgrade / Trade), with no other new entry yet?
  Pick it: does the shop close and its cutscene play?
- **C5** Back in his menu: is there now **Ask about Ashley** under **Talk**? Pick it.
  - Does the cursor skip the hidden spare entries (no empty rows to land on)?
  - Does the cutscene play?
- **C6** Back at Ashley: is the list now **How are you holding up?** then **About the merchant**? Move with the arrow
  keys, W / S and your pad if you use one. Pick the second.

**Characters and animations:**
- **C7** **F6** (example 19).
  - Do Luis (on Leon's **right**) and a **purple** Ashley (on his **left**) appear about 2 m ahead, facing him?
  - Is the real Ashley hidden while it plays, and back after?
- **C8** **F11** (`Event animation`): Luis appears and he and Leon play the game's own cutscene animations (csa012,
  cut 3, then cut 5 from 6 s), then it fades out.
  - Do they move like a cutscene: real actions, not idles?
  - Do Leon and the camera come back to you afterwards?
  - They may walk off or through walls: that's known.
- **C9** REFramework's menu (Insert) > Script Generated UI > **remod cutscenes** > **Animations**.
  - **Who: Leon**: pick a bank and click an animation. Does it loop? Do the frame slider and Pause work? Does
    **Stop previewing** give him back?
  - **Who: luis**: does he come out with banks to pick from?

**Started by itself:**
- **C10** (optional) Example 18 starts by itself near where you stood for the cutscene probe in October (chapter 1's
  village, the same spot). If you go there: does "remod: this cutscene started by itself" appear on its own?

**Your own cutscene: your camera and your spot** (the app plus the game):
- **C11**
  1. In remod open `graphs\x8_my_own_cutscene.json` and click **Test in game** on the Cutscene block.
  2. In game, REFramework's menu > **Reset Scripts**.
  3. Frame three shots (REFramework's free camera, or just the game's), pressing **F10** at each, about 2 s apart.
  4. Then stand somewhere memorable and click **Make a trigger here** in the remod cutscenes menu. Walk 5 m away.
- **C12**
  1. In remod, click **Use recording**, then **Use trigger**, then **Test in game** again.
  2. In game, **Reset Scripts**.
  3. Press **F3**. Does it play with your three camera shots, moving between them?
  4. Then walk back to your spot. Does it start by itself (once per save)?

**Scripts:**
- **L1**
  1. In remod open `graphs\x9_broken_script.json` and click **Test in game** on the Lua script block.
  2. In game, **Reset Scripts**, then wait 5 s. REFramework's menu should show "remod full check script: loaded".
     The script has a mistake on purpose: it errors once after 2 s.
  3. In remod, click **Game errors**. Does an amber error show under the block, naming line 9?
- **L2** Click **Fix with Claude**. That's one Claude call on your plan, as with any click.
  - Does a note come back, and `broken_check.lua` get fixed (the old one kept as `.bak`)?
  - **Test in game**, Reset Scripts, wait, **Game errors**: are there no new errors?
- **L3** Type a change in the block's box, e.g. "also show the time since the script started", and click **Ask
  Claude**. **Open in editor**: is the change there, readable?
- **L4** Pipeline panel > **Scripts in the game**: are `remod_cutscene.lua` and `broken_check.lua` listed? Click
  **Remove** on `broken_check.lua`. Is it gone from the list, and in the Recycle Bin?
- **L5** On the x8 Cutscene block click **Remove from game**.

**Fluffy's uninstall:**
- **F1** Quit the game, uninstall all four `S5` mods in Fluffy, and tell me. I'll check that their files are gone from
  the game folder (`reframework\autorun`, `reframework\data\remod_*`, the merchant's menu layout under `natives`).

---

## Taking it all out
- Uninstall every `S*` mod in Fluffy.
- **Scripts in the game** in remod should be empty.
- The game's `reframework\data\remod_cutscenes\` holds remod's flags and your recording. It's harmless; delete it if
  you like.

## If something goes wrong
- **The game crashes:** note what you'd just done, then run `save_log.cmd` for that session. The log shows how far it
  got.
- **A key does nothing:** REFramework's menu > remod cutscenes: its **Problem:** line names what failed. The log has it
  too.
- **A mod's change doesn't show:** check in Fluffy that it's installed and enabled, and that no other mod replaces the
  same file.

## Rebuilding the pack
`python spikes\make_full_check.py`, with `REMOD_GAME` set to RE4R's extracted `natives\stm`. Unchanged steps come from
the run cache, so a rebuild is quick except for new movies.

## The checklist
```
R1 R2 R3 R4
A1 A2 A3 A4 A5 A6 A7 A8 A9 A10 A11 A12 A13 A14 A15 A16 A17 A18 A19 A20 A21 A22 A23 A24 A25 A26 A27 A28
T1 T2 T3 T4 T5 T6 T7 T8
M1 M2 M3 M4 M5 M6 M7 M8
S1 S2
C1 C2 C3 C4 C5 C6 C7 C8 C9 C10 C11 C12
L1 L2 L3 L4 L5
F1
```
