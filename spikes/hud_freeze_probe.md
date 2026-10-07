# HUD and freeze probe

Part of M3 route 3: during a cutscene the HUD should be hidden, and Leon should stand still. Cutscenes will later
start by themselves (triggers), so this has to work **while you're moving him**. The game has its own switches for
these, found in the SDK dump; this probe tries them.

**Status:** done (2026-10-07). **Run 6 works: the operation stop with layer `Self`, sent every frame, stops Leon even
with a direction held**; the cutscene script now uses it. Earlier results:
- **HUD (run 1):** the Display HUD option's values are 0 nothing (not even the crosshair), 1 and 3 crosshair and the
  red damage edges, 2 everything (health bar too; your setting). Cutscenes use 0.
- **Nothing has stopped Leon with a direction held yet:** the game's event pause, switching off his head updater (he
  keeps his last action, so he walks on), his behaviour tree's `EnableOperation` flag (run 3: the game kept reading
  it as on), the control groups (runs 4-5: `Character` off, then all off), forcing his idle (run 5). REFramework's
  FreeCam "Disable Movement" has the same problem. Animations still play throughout (F8).
- **Why `EnableOperation` never changed:** it isn't a switch, it's a copy. The game sets it each frame from its
  *operation stop* system, which it uses while the inventory, map, files, tutorial or crafting is open.

**What run 6 answers:** does the game's operation stop (`CharacterManager.requestOperationStop`, sent every frame)
stop Leon, even with a direction held? Which of its layers (`Self`, `Inventory`, `Tutorial`, `File`, `Map`, `Craft`)
does it, and does it freeze anything else? Can a cutscene still play an animation on him then?

**Time:** about 5 minutes. **You need:** RE4R with REFramework, and remod with **Game folder** set. It turns off when
you press F5 again or on Reset Scripts, and hooks no game methods.

## Install it

If you still have the probe's Lua script block from earlier runs, skip to step 4.

1. In remod, take out the cutscene: **Remove from game** on the Cutscene block (so F9 does nothing meanwhile).
2. Switch to **Build layout**. In the **Nodes** panel, open **Source** and add a **Lua script** block.
3. Switch to **Use layout**. Set the block's **Script** to `E:\Company\Github\Jetstream\spikes\hud_freeze_probe.lua`.
4. Click **Test in game** on that block.

## Run it

5. In game (no restart needed): REFramework's menu (**Insert**) > **ScriptRunner** > **Reset Scripts**. White text at
   the top left shows the keys, the layer, and what the game reads (`OperationEnable`, `OperationStopState`,
   `EnableOperation`).
6. **Layer Self:** run (hold a direction) and press **F5** while still holding it. Does Leon stop? Does the text's
   `EnableOperation` turn false? Try aiming and the camera. Press **F8**: does his idle restart? Press **F5** to turn
   it off.
7. **The other layers:** press **F6** for the next layer, then repeat step 6. Do this for each (Inventory, Tutorial,
   File, Map, Craft), or stop at the first that works.
8. Check everything works normally with it off.

## Take it out

9. In remod, click **Remove from game** on the block.

## Tell Claude

- Which layer (if any) stopped Leon with the direction held? Did the camera or anything else freeze too?
- Did F8 restart his idle while stopped?

Claude reads the rest from `<game folder>\reframework\data\remod_hud_freeze_probe.json`.

**If something goes wrong:**
- **Leon or the game won't respond:** press F5, or Reset Scripts. If even that doesn't help, restart the game:
  nothing is saved.
- **A message box names `hud_freeze_probe.lua`:** note what it says and tell Claude.
