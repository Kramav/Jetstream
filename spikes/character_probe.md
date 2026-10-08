# Character probe

Other characters in cutscenes (Ashley, Luis, villagers, enemies): can a script find them, take them over the way the
game's own cutscenes do, play their animations, and put them where a shot needs them?

**What it answers:**
1. Which characters the script can find, and what each is called.
2. Whether the game's cutscene lock takes one over: does an enemy stop attacking, does Ashley stop following?
3. Whether its animation can be restarted, as cutscenes already do for Leon.
4. Whether it can be moved into place.

**Status:** built 2026-10-07, not run yet.

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
4. **F3** locks it for a cutscene. Watch it for a few seconds: does it stop what it was doing? Does an enemy stop
   coming at you, or Ashley stop following? Does "locked" read true? Press **F3** again to unlock it: does it go
   back to normal?
5. **F4** restarts its current animation, once while unlocked and once while locked. Does it visibly restart?
6. **F6** brings it 1.5 m in front of Leon, facing him. Does it appear there and stay, or walk away or snap back?
   Try it locked and unlocked.

## Take it out

7. In remod, click **Remove from game** on the block. Reset Scripts also unlocks anything still locked.

## Tell Claude

For each of F3, F4 and F6: what you saw, and for which character.

Claude reads the rest from `<game folder>\reframework\data\remod_character_probe.json`: the list of characters
found, and every press.

**If something goes wrong:**
- **A character stays frozen:** press F3 on it again, or Reset Scripts.
- **The game crashes:** note what you pressed. The JSON and the log (`re2_framework_log.txt`) show how far it got.
