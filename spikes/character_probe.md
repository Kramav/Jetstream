# Character probe

Other characters in cutscenes (Ashley, Luis, villagers, enemies): can a script find them, take them over the way the
game's own cutscenes do, play their animations, and put them where a shot needs them?

**What it answers:**
1. Which characters the script can find, and what each is called.
2. Whether the game's cutscene lock takes one over: does an enemy stop attacking, does Ashley stop following?
3. Whether its animation can be restarted, as cutscenes already do for Leon.
4. Whether it can be moved into place.

**Status:**
- **Run 2 (2026-10-08):**
  - F2 finds Leon and Ashley.
  - F4 restarts Ashley's animation, with a jitter if she was running.
  - F6 puts her in front of Leon, but she keeps following him.
  - F3, the game's cutscene lock, didn't stop her, and unlocking crashed the game.
- **Run 3 (2026-10-08):** the game's "think" switch, held off, didn't stop her following either.
- **Run 4 (2026-10-08):**
  - F3 hides all of Ashley (13 objects) and brings her back.
  - The game's cutscene body prefab spawned only a mesh-swap settings object, so nothing showed.
- **Run 5 (2026-10-08):** F5 built nothing. Ashley's top object has no mesh: her visible parts (body, head, hair,
  an accessory) are objects under it.
- **Run 6 (2026-10-08):** F5 builds a visible copy in front of Leon, and F6 moves it. But it has no animation
  layer (F4 and F7 do nothing) and no hair.
- **Run 7 (2026-10-08): works.**
  - The copy stands idle (F7), restarts its animation (F4), moves its head and clothes with its body, and has her
    hair.
  - Her hair's shade is slightly off.
  - Hide the real one (F3), drive the copy: that's the way for other characters in cutscenes.
- **Run 8 (ready): puppets from files, no live character needed.**
  - Part 1: F8 builds Luis anywhere, from the files the game's own event `csa012` builds him from. F9 writes down
    the files a live character really uses.
  - **Part 1 done (2026-10-08):** Luis is built from files, animates and is placed, and his hair works. F9's
    written-down Ashley built from files too, but without hair: hair drawn as Strands doesn't work from files. The
    new character now uses her plain hair mesh, the kind Luis's is.
  - **Part 2's try (2026-10-08) crashed the game.** The log says the first F8 built Luis fine. After several
    Reset Scripts (which remove the puppets), F8 built him again and the game crashed 18 ms later.
- **Run 9 (ready):** F11 removes the puppets without a Reset Scripts, to find which of the two causes it (steps
  below).
  - Part 2: a new character mod. "rmc001" is Ashley in blue at new paths, with nothing of the game's replaced,
    installed through Fluffy and built by F8 from the definition the mod ships.

**Time:** about 10 minutes, plus 10 for run 8. **You need:**
- RE4R with REFramework;
- remod with **Game folder** set;
- a save with someone else nearby: Ashley is best, or a villager.

It hooks no game methods.

## Install it

1. In remod, add a **Lua script** block (Build layout, Nodes panel > Source), set its **Script** to
   `E:\Company\Github\Jetstream\spikes\character_probe.lua`, switch to Use layout and click **Test in game**.
2. In game: REFramework's menu (Insert) > ScriptRunner > **Reset Scripts**. White text at the top left shows the
   keys and the selected character.

## Run it

3. **F2** picks the next character. The line says:
   - its name, and what it is (player, partner, doll NPC, enemy);
   - how far it is;
   - whether it's locked;
   - its animation.

   Press it until you reach Ashley, or someone near you.
4. Pick Ashley (F2), then press **F3**: does she disappear completely (head, hair and clothes too)? Press F3
   again: is she back?
5. With Ashley selected, press **F5**: a copy of her should appear 1.5 m in front of Leon. What does it look like:
   - whole, or only some parts (body, head, hair, clothes)?
   - standing, in a T-pose (arms out), or with parts floating apart?
   - nothing at all?
6. Press **F2** until the line says **puppet**. Then:
   - **F7** (stand idle): does it start moving like a person?
   - **F4** (restart its animation);
   - walk away, then **F6**: does it come to you and stay there?
7. Reset Scripts removes the copy and shows anything hidden.

## Run 8, part 1: Luis from files

The probe changed, so click **Test in game** again first, then Reset Scripts. The top line should say "run 8".

1. Load any save; Luis doesn't need to be anywhere near.
2. Press **F8**. It builds Luis 1.5 m in front of Leon and selects him; the "last:" line lists what loaded.
   - Is he there? All of him (body, head, hair), a T-pose, floating parts, or nothing?
3. **F7** (stand idle), **F4** (restart), then walk away and press **F6**.
4. If Ashley is with you: press **F2** until she's selected (partner), then **F9**. That writes down the files she
   really uses, which I'll compare with the definitions. Pressing **F8** again builds the next definition: after
   Luis, her written-down one.

## Run 9: what crashes a second F8

Click **Test in game** again (the top line should say "run 9"). In remod's Pipeline panel, open **Scripts in the
game** and remove anything you don't want running; the probe should be the only one. Don't press Reset Scripts
during these steps unless one says to.

1. **F8** (Luis appears), then **F11** (he's removed), then **F8** again. Does the game crash?
2. If it didn't: **F8** twice more without F11 (two Luises, then rmc001 if the mod is installed). Crash?
3. If it still didn't: Reset Scripts once, then **F8**. Crash?

Tell me the first step that crashed, if any.

## Run 8, part 2: a new character, as a mod

5. Build the mod. In PowerShell, in the remod folder:
   `powershell -ExecutionPolicy Bypass -File spikes\make_new_character.ps1`
   It takes about a minute and makes `spikes\out\remod new character rmc001.zip`. It's built from your
   extracted game files each time.
6. Install that zip with Fluffy as usual. Keep the probe installed: the mod is data only (the character's files and
   its definition, `reframework\data\remod_puppets\rmc001.json`), and the probe is what builds it.
7. Start the game (or Reset Scripts if it's running) and load a save.
8. Press **F8** until the "last:" line says **rmc001** (Luis comes first). Is it Ashley in a blue outfit? Is her
   hair there? Then **F7**, **F4**, **F6** as before.
9. Uninstall the mod in Fluffy when done.

## Take it out

10. In remod, click **Remove from game** on the block.

## Tell Claude

What F3 hid, what the copy looked like after F5, and what F7, F4 and F6 did to it. The JSON records what the copy is
made of.

For run 8: what Luis and rmc001 looked like after F8, and what F7, F4 and F6 did. The JSON records what loaded
(`from_files`) and F9's written-down files (`captured`).

Claude reads the rest from `<game folder>\reframework\data\remod_character_probe.json`: the list of characters
found, and every press.

**If something goes wrong:**
- **A character stays hidden:** press F3 on it again, or Reset Scripts. Reloading a checkpoint also restores it.
- **F8 builds nothing visible:** the "last:" line names what didn't load. Tell me what it says.
- **rmc001 isn't in F8's list:** the mod's `reframework\data\remod_puppets\rmc001.json` didn't reach the game folder.
  Check it's there.
- **The game crashes:** note what you pressed. The JSON and the log (`re2_framework_log.txt`) show how far it got.
