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

**Time:** about 10 minutes. **You need:**
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

## Take it out

8. In remod, click **Remove from game** on the block.

## Tell Claude

What F3 hid, what the copy looked like after F5, and what F7, F4 and F6 did to it. The JSON records what the copy is
made of.

Claude reads the rest from `<game folder>\reframework\data\remod_character_probe.json`: the list of characters
found, and every press.

**If something goes wrong:**
- **A character stays hidden:** press F3 on it again, or Reset Scripts. Reloading a checkpoint also restores it.
- **The game crashes:** note what you pressed. The JSON and the log (`re2_framework_log.txt`) show how far it got.
