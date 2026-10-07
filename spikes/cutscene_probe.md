# Cutscene probe

Part of M3 route 3: cutscenes the game renders in real time, played by remod's cutscene script.

**Status:** done for RE4R on 2026-10-07. You only need to run it again for another game. The
[test cutscene](#play-the-test-cutscene) below also played the same day: the camera held and cut on time.

## What it found

| Question | Answer |
|---|---|
| Can a script find the camera and Leon? | Yes. The camera is `MainCamera`; Leon is `ch0a0z0_body`. |
| Can a script hold the camera in place, without the game moving it back? | Yes, from the 4th of the 7 timings tried. The first three run before the game moves its own camera, so the game overrides them; that's why the view didn't freeze at first. But set at the last one (`BeginRendering`) alone, a cutscene's view still followed the mouse, so the cutscene script now sets it at each of them. |
| Can a script play one of Leon's animations? | Yes. F8 visibly restarted his idle animation. |
| Can the script list its cutscene files and keep time? | Yes. |

The raw results are in `<game folder>\reframework\data\remod_cutscene_probe.json`.

## Play the test cutscene

[cutscene_test.json](cutscene_test.json) is a 10-second cutscene built from the camera the probe recorded when you
pressed F5. Its positions are fixed points on the map, so it only looks right at that spot.

**Time:** about 5 minutes.

### Install it

1. In remod, on the probe's **Lua script** block, click **Remove from game** (if you haven't already).
2. Switch to **Build layout**. In the **Nodes** panel at the bottom, open **Source** and add a **Cutscene** block.
3. Switch to **Use layout**. Set the block's **Cutscene file** to
   `E:\Company\Github\Jetstream\spikes\cutscene_test.json`.
4. Click **Test in game**. That's all the installing there is: the Cutscene block brings its own script. It copies
   two files into the game's folder:
   - `reframework\autorun\remod_cutscene.lua`: remod's cutscene player (from remod's `runtime` folder);
   - `reframework\data\remod_cutscenes\cutscene_test.json`: the cutscene.

   You don't need a Lua script block or the probe for this.

### Watch it

5. Start the game and load **the same save you used for the probe**. Stand roughly where you were when you pressed F5.
   If the game is already running, open REFramework's menu (**Insert**), then **ScriptRunner** > **Reset Scripts**.
6. Press **F9**: this cutscene's start key (set in the file; F5-F8 were the probe's keys and do nothing now). Or, in
   REFramework's menu: **Script Generated UI** > **remod cutscenes** > **Play**.
7. You should see:
   - the screen fades in from black, with black bars at the top and bottom;
   - the camera pushes in smoothly towards Leon and zooms a little, with a subtitle;
   - a cut to a wider shot that slowly rises, with a second subtitle;
   - a fade to black after 10 seconds, then the normal camera returns.

   Leon also restarts his idle animation at the start. He's frozen and the HUD is hidden until it ends.
8. Press **F9** again to stop it early, or to play it again.

### Take it out

9. In remod, click **Remove from game** on the Cutscene block.

### Tell Claude

What you saw: did it play, was the camera movement smooth, did the cut happen, did the subtitles, bars and fades
show?

**If something goes wrong:**
- **The camera shows somewhere else or looks at nothing:** you're not where the probe ran. Press F9 to stop.
- **F9 does nothing:** check that REFramework's menu shows **remod cutscenes** under Script Generated UI. If it says
  "No cutscenes", click **Reload cutscenes**.
- **A message box names `remod_cutscene.lua`:** note what it says, then click **Game errors** on the block in remod.

## Running the probe again (another game)

**What it checks:** the four questions above. While it runs it changes only the camera (during the hold test) and
the player's animation (F8). Nothing is saved to the game. It doesn't hook game methods, only REFramework's own
per-frame callbacks, so it's safe on REFramework's stable release.

**Time:** about 10 minutes. **You need:** the game with REFramework, and remod with **Game folder** set in its
Pipeline panel.

### Install it

1. In remod, switch to **Build layout** and click **New** in the Pipeline panel. In the **Nodes** panel, open
   **Source** and add a **Lua script** block.
2. Switch to **Use layout**. Set the block's **Script** to `E:\Company\Github\Jetstream\spikes\cutscene_probe.lua`.
3. Click **Test in game**.

### Run it

4. Start the game and **load a save**, so you're in the world as the player. If the game is already running, open
   REFramework's menu (**Insert**) > **ScriptRunner** > **Reset Scripts**.
5. Press **F5**. This writes down the camera and the player. Nothing visible happens.
6. Press **F6**. Text at the top left names the timing being tried: 7 in turn, 3 seconds each. Keep **moving the player
   and the mouse** the whole time.
   - With the first few, the view keeps moving: that's expected.
   - When the view stays **perfectly still** (no jitter, no drift) while you move, press **F7** while that name is
     shown. Press F7 for each one that holds.
   - When the text disappears, the test is over and the camera is yours again. Pressing F6 again stops it early.
7. Stand still and press **F8**. The player should visibly restart what they're doing (an idle, for example). Note
   whether they did.
8. Quit the game.

### Take it out

9. In remod, click **Remove from game** on the block.

### Tell Claude

Say that it's done and whether F8 visibly restarted the animation. Claude reads the results from
`<game folder>\reframework\data\remod_cutscene_probe.json`.

**If something goes wrong:**
- **The camera stays frozen after the test:** press F6, or Reset Scripts.
- **A message box names `cutscene_probe.lua`:** note what it says and tell Claude.
- **The result file is missing:** check that `reframework\autorun\cutscene_probe.lua` is in the game's folder, then
  Reset Scripts.
