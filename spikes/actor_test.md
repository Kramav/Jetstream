# Actor test

Other characters in a cutscene, played by remod's cutscene runtime: Luis and the blue Ashley (rmc001) appear in
front of Leon, the real Ashley is hidden while it plays, then they're put away when it ends.

**What it answers:**
1. Do actors appear when a cutscene starts (no keys pressed, no waiting)?
2. Are they placed side by side in front of Leon, facing him?
3. Does the real Ashley vanish while it plays and come back after?
4. Can it play again (actors reused, no crash)?

**Status:**
- **First try (2026-10-08): F11 crashed the game.** REFramework frees objects a script made when the script resets
  (or stops using them), unless the script marks them to keep. The runtime handed the game objects that weren't
  kept. Fixed: everything it hands the game is now kept.
- **Second try (2026-10-08): works.** The cutscene played with its actors, and nothing was logged as a problem. F11
  a second time played cleanly. Luis and Ashley were on the wrong sides; fixed.
- **Part 2 (ready):** the sides again, the animation previewer, the cutscene editor and example 19.

**Time:** about 10 minutes. **You need:**
- RE4R with REFramework;
- remod with **Game folder** set;
- the rmc001 mod installed through Fluffy (`spikes\out\remod new character rmc001.zip`; rebuild it with
  `spikes\make_new_character.ps1` if it's gone);
- a save with Ashley nearby is best (to see her hidden), but any save works.

It hooks no game methods.

## Install it

1. Close remod and build it again (it was open during the last build), or skip this if you rebuilt since.
2. In remod, Pipeline panel, **Scripts in the game**: remove `character_probe.lua`: it builds puppets of its own.
3. Add a **Cutscene** block (Build layout, Nodes panel > Source), set its **Cutscene file** to
   `E:\Company\Github\Jetstream\spikes\actor_test.json`, switch to Use layout and click **Test in game**.
   It copies remod's cutscene runtime, the cutscene and Luis's definition into the game.
4. In game: REFramework's menu (Insert) > **Reset Scripts**. Under Script Generated UI > **remod cutscenes**, the
   "Puppets" line should list `luis` and `rmc001`.

## Run it

5. Stand somewhere open, with room in front of Leon. Press **F11**.
   - The screen fades in, with black bars at top and bottom.
   - Do Luis (left) and the blue Ashley (right) stand about 2 m in front of Leon, facing him?
   - Is the real Ashley gone while it plays?
   - Two subtitles, then a fade to black at 9-10 s.
6. After it ends: are Luis and the blue Ashley gone, the real Ashley back, and Leon controllable?
7. Press **F11** again: does it play the same way a second time?
8. Optional: **Reset Scripts**, then **F11** once more.

## Part 2: sides, the previewer, the editor, example 19

Close remod and build it again first, then click **Test in game** on the Cutscene block again (the runtime changed).

10. **F11** again: Luis should now be on Leon's **left** and the blue Ashley on his **right** (it was the other way
    round).
11. REFramework's menu > remod cutscenes > **Animations**.
    - **Who: Leon.** Pick a bank, then click an animation. Does Leon play it, looping? Do the frame slider and Pause
      work? Click **Stop previewing**: does he move again?
    - **Who: luis.** Does Luis come out in front of Leon, with a list of banks and animations? Click a few.
    - Click **Use in a cutscene** on one you like.
12. In remod, open `actor_test` from the Browser's **Cutscenes** (top of the tree), or click **Edit cutscene** on the
    Cutscene block. It opens in the Cutscene layout.
    - On the **Stage** (top right), is Luis on Leon's left?
    - Move the playhead to 2 s (click the seconds), then click **Add picked animation**.
    - Drag a subtitle along the timeline, and stretch a fade by its end.
    - **Test in game** (top bar), Reset Scripts, **F11**: does that character play the animation at 2 s?
13. Example 19: open `examples\19_a_new_character_in_a_cutscene.json`, **Run**, and install `examples\mods\New
    character.zip` with Fluffy. In game, **F6**: Luis on the right, a purple Ashley on the left.

Tell me what happened at each step.

## Take it out

14. In remod, click **Remove from game** on the Cutscene block. Uninstall the rmc001 and New character mods in
    Fluffy if you like.

## Tell Claude

What you saw at steps 5-8, especially where each character stood and which way they faced. The "Problem:" line in
the remod cutscenes menu names anything that failed. The log (`re2_framework_log.txt`) has it too.

**If something goes wrong:**
- **An actor doesn't appear:** look at the "Problem:" line and the "Puppets" line (a missing definition or file
  is named there).
- **They stand on the wrong side, or face away:** tell me which. Side and facing come from Leon's rotation, and
  only "in front" has been seen in game before.
- **The real Ashley stays hidden:** Reset Scripts brings her back.
- **The game crashes:** note what you pressed. The log shows how far it got.
