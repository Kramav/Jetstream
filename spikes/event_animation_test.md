# Event animation test

Can a character play an animation from one of the game's own cutscenes? RE4's cutscenes have actions that aren't in
gameplay: they live in each cutscene's own animation files (`_chainsaw/event/cs/<id>/.../chara/<character>/*.motlist`,
1,543 of them). This test loads one of those files onto Luis (a puppet) or Leon, under a bank number of our own, and
plays its animations in remod's Animations previewer. No bank file is written and nothing of the game's is replaced.

**What it answers:**
1. Does an animation file load onto a character this way? The note says how many animations bank 9000 has.
2. Do its animations play on Luis's puppet and on Leon? Are they real cutscene actions, and does the body look right
   (no twisted limbs)?
3. Does the character stay where it is, or does the animation move it somewhere else (event animations may be placed
   relative to the cutscene's own origin)?
4. Does the face move? Expected: no. Faces are a separate file (`cha300_10.motlist`) on the head, not tried yet.

If 1 and 2 work, cutscenes can use any of the game's cutscene animations, and the same route should later take
animations made in Blender (CLAUDE.md §10, "Brand-new animations made in Blender", step 1).

**Status:**
- **Run 1 (2026-10-08): picking luis crashed the game**, before any animation file. The crash was on one of the
  game's worker threads, with nothing logged by remod. Luis was built from inside the menu's drawing code (a cutscene
  builds him from its key and works). Now the menu only asks; the game's own update builds him (and loads animation
  files), and his banks are read half a second later. The log gets a line ("bringing out luis") before each such step.
- **Run 2 (2026-10-08): no crash.** Luis's csa012 file loaded as bank 9000 with 14 animations, Leon's as 9001 with
  21; both needed the game's `setupMotionBank` first. **Works:** most look like the game's cutscene actions, bodies
  right, some move only the arms; they move the character and can walk it through walls.

**Time:** about 10 minutes. **You need:**
- RE4R with REFramework;
- remod with **Game folder** set;
- a save loaded (anywhere).

It hooks no game methods.

## Install it

1. In remod, open `spikes\actor_test.json` (Browser > Cutscenes, or the actor test's Cutscene block) and click
   **Test in game**. It copies remod's cutscene runtime and Luis's definition into the game. A cutscene without Luis
   as an actor doesn't bring his definition.
2. In game: REFramework's menu (Insert) > **Reset Scripts**.

## Run it

3. REFramework's menu > Script Generated UI > **remod cutscenes** > **Animations**.
4. **Who: luis.** "Bringing luis out..." shows for a moment, then Luis stands 2 m in front of Leon.
   - **Animation file** already holds Luis's file from cutscene csa012:
     `_Chainsaw/Event/cs/csa012/csa012_s00/chara/cha300_00/cha300_00.motlist`. Leave **As bank** at 9000.
   - Click **Add this animation file**. About a second later, a line under it says what happened, e.g.
     "bank 9000: 37 animations", or "failed: ...".
5. In **Bank**, pick **9000**. Click a few animations in the list. For each one:
   - Does Luis do something you'd see in a cutscene?
   - Does his body look right?
   - Does he stay in front of Leon, or jump somewhere else?
   - Use the **Frame** slider and **Pause** to look closely.
6. **Who: Leon.** The file box now holds Leon's file from the same cutscene (`.../cha000_00/cha000_00.motlist`).
   Set **As bank** to **9001**, click **Add this animation file**, pick bank 9001 and click a few. Same questions.
   Click **Stop previewing** after.
7. Optional: another cutscene's file. Folders are under `natives\STM\_chainsaw\event\cs\` in the extracted game
   files. A file's path is written from `_Chainsaw/Event/...`, without `natives/STM` and without the `.663` at the
   end. The folders under `chara\` are named after the characters' meshes: Luis is `cha300_00`, Leon `cha000_00`. Use a
   new bank number for each file.

## Take it out

8. In remod, **Remove from game** on the Cutscene block, or leave it there for the next test.

## Tell Claude

- The line(s) under **Add this animation file**, exactly as shown.
- What the animations looked like on Luis and on Leon, and where they ended up standing.
- Anything in the "Problem:" line.

**If something goes wrong:**
- **"failed: ..." or 0 animations:** copy the line. The second number in the note says whether the game had to set
  its banks up again first.
- **Bank 9000 isn't in the Bank list:** pick another Who, then the same one again (the list is read when you pick).
- **The game crashes:** note which step. The log (`re2_framework_log.txt`) has the note lines up to that point.
- **"Use in a cutscene" with bank 9000:** a cutscene can't load the file yet, so that animation only plays while the
  file is still loaded from this session. Cutscene files don't say which animation files to load yet. That comes
  after this test.
