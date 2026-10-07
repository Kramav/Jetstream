# Cutscene probe (M3 route 3: real-time cutscenes)

**Status: run 2026-10-07; not needed again for RE4R** (run it again for another game). Results:
- Camera: `MainCamera`, no parent; found via `sdk.get_primary_camera()`. Leon: `ch0a0z0_body`, found via
  `chainsaw.CharacterManager:getPlayerContextRef():get_BodyGameObject()`.
- Hooks: UpdateBehavior, LateUpdateBehavior and UpdateMotion don't hold (the game moves the camera after them, almost
  every frame). PrepareRendering held (2 of 473 frames moved), BeforeLockSceneRendering, LockScene and BeginRendering
  held perfectly (0 moved). The runtime uses BeginRendering.
- `changeMotion` works: Leon visibly restarted his animation (bank 1000, motion 160).
- `fs.glob` returns paths relative to `reframework\data`; `os.clock` exists.

**Afterwards: a test cutscene at the probe's spot** ([cutscene_test.json](cutscene_test.json)). Its camera is the
probe's own (where you pressed F5), pushed in towards Leon, then a cut to a shot pulled back that rises. It only makes
sense at that spot: load the same save and stand where you were.
1. In remod, add a **Cutscene** block with Cutscene file `spikes\cutscene_test.json`, then click **Test in game**.
2. In game: **Reset Scripts**, then press **F9** (or REFramework's menu > Script Generated UI > remod cutscenes >
   Play).
3. Expect a fade in, letterbox bars, two subtitles, a smooth push in, a cut, a slow rise, a fade out, and Leon
   restarting his idle animation. Leon still takes your input and the HUD still shows (not built yet).

**What it answers:** four things the cutscene runtime depends on in RE4R: what the camera is and how to find Leon;
which engine hook lets us hold the camera still; whether Leon's animation can be restarted from a script; and how
REFramework lists files. It changes only the camera while you run the hook test and Leon's animation when you press
F8. Nothing is saved to the game. It hooks no game methods: it only uses REFramework's own per-frame callbacks
(v1.5.9 can't hide method hooks from the current game's anti-tamper check, which crashed the game during the movie
probe).

**Time:** about 10 minutes. **You need:** RE4R with REFramework, and
remod (`build\app\remod-app.exe`), with **Game folder** set in its Pipeline panel (see the movie probe's guide, steps
1-2).

## Install the probe

1. In remod: **Build layout**, then **New** in the Pipeline panel. From the **Nodes** panel (bottom), **Source**, add a
   **Lua script** block.
2. **Use layout**: set the block's **Script** to `E:\Company\Github\Jetstream\spikes\cutscene_probe.lua`.
3. Click **Test in game**.

(You can install it alongside the movie probe: a second Lua script block in the same graph, each with its own Test in
game.)

## Run it

4. Start the game and **load a save**, so you're standing in the world as Leon. (Already running: **Insert**,
   **ScriptRunner**, **Reset Scripts**.)
5. Press **F5**. This writes down the camera and how Leon is found. Nothing visible happens.
6. Press **F6**. The view freezes, and text at the top left names the hook being tried, one after another, 3 seconds
   each (7 in all). Meanwhile, **move Leon and the mouse**.
   - If, for one of them, the view stays **perfectly still** while you move (no jitter, no drift), press **F7** while
     that name is shown. More than one may hold: press F7 for each one that does.
   - When the text disappears, the test is over and the camera is yours again. F6 also stops it early.
7. Stand still and press **F8**. Leon should visibly restart what he's doing (his idle, for example). Note whether he
   did.
8. Quit the game.

## Take it out

9. In remod, click **Remove from game** on the block.

## Done

Tell Claude it's done, and whether Leon visibly restarted his animation at F8. The result is
`<game folder>\reframework\data\remod_cutscene_probe.json`, which Claude reads from there.

**If something goes wrong:**
- **The camera stays frozen after the test:** press F6, or Reset Scripts.
- **The game shows a message box naming `cutscene_probe.lua`:** note what it says and tell Claude.
- **The result file is missing:** check that `reframework\autorun\cutscene_probe.lua` is in the game's folder, and
  press Reset Scripts.
