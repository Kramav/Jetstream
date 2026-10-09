# Trigger test

Cutscenes that start by themselves. A cutscene file's `trigger` names where (a spot Leon walks into) and optionally
when (the game's chapter, stage, area or location). The cutscene script checks it five times a second. It's built
into remod's cutscene script, so this is a test of the real feature, not a separate probe.

**What it answers:**
1. Does the script read where Leon is? The menu's **Now** line shows the game's names for the chapter, location,
   area and stage.
2. Does a cutscene start by itself at a spot (example 18)?
3. Does the whole loop work with your own spot: **Make a trigger here** in game, then **Use trigger** in remod?

**Status:**
- **Part 1 (2026-10-08): example 18 started by itself.** Part 2 (your own spot) not tried yet.
- **Part 3, run 1 (user, 2026-10-09):** Ashley's prompt and H worked; no prompt at the merchant; no difficulty
  cutscene. The log said nothing about why, so the script now writes into the log what it sees (characters within 8 m
  of Leon with their kind and list, now enemies too; the difficulty setting and the Difficulty flags read two ways),
  and talk triggers play every time (user). **Run 2:** Ashley twice, fine. No merchant prompt; G was the free camera.
  From the log: the merchant isn't a listed character (he's a gimmick, now found as `merchant`), and the Difficulty
  flags read off. **Run 3:** the merchant's key is now **T**; story flag changes are logged as you play.
- **Run 3 (user + log, 2026-10-09): talking works.** T at the merchant (found 3.7 m away as a gimmick) and his
  Trade, H at Ashley, each played. The two prompts drew on top of each other (noted, not fixed). Story flags have no
  readable names: 17,586 numbered flags in 242 groups (`Location47/Location47_002`, `Chapter0101/...`); 753 came on
  when the save loaded. **Run 4:** flag tests from those, a control that must never play, once per save.
- **Run 4 (user + log, 2026-10-09):** the flag test played, the control never: **flag triggers work**. The game's
  csa038 turned on `EventTimelineStart00_038` / `EventTimelineEnd00_038` (cutscenes have flags). **Once per save
  failed:** after a reload no J prompt; the play time had kept counting. The script now logs the game's clocks at
  each load. **Run 5:** step 10 again, loading the older save from a save slot (Load from the menu).
- **Runs 5-7 (2026-10-09):** prompts stack now (J showed); an in-game load of an *older* slot restores the play time,
  but reloading the *same* save doesn't, so once per save can't rest on it. Next: remod's own flags, kept per save
  slot. **Run 8:** see "Saves and loads" below.

## Run 8: saves and loads

The installed mod (rebuild of `spikes\out\remod trigger test 3.zip`) logs every save and load the game does. Do
these, in any order, then quit the game (the log is only complete once it closes):
1. Save to a slot (a typewriter).
2. Load a different slot.
3. Load the same save you're in.
4. Die, then retry from the checkpoint.
5. Walk into a new area (an autosave).

Tell Claude only which of these you did; the log has the rest.

**Time:** about 10 minutes. **You need:** RE4R with REFramework, Fluffy, and the save you used for the earlier
probes (example 18's spot is where you stood then, in the village). It hooks no game methods.

## 1. Example 18: a spot from before

1. In Fluffy, uninstall the earlier test mods (probes and examples), then install
   `examples\mods\Trigger test.zip` (already built).
2. Load the save. Open REFramework's menu (Insert) > Script Generated UI > **remod cutscenes**. Read the **Now:**
   line: it should show names like `chap01_01` and `st40_100`, not "unknown". "Started by itself" should be listed
   with "starts by itself".
3. Walk to where you stood for the earlier probes. Within 3 m of it, letterbox bars and "remod: this cutscene
   started by itself" should appear on their own. It starts once per game session; reload the game to try again.
   (If you can't find the spot, go on to part 2: it uses your own.)

## 2. Your own spot

4. In remod, add a **Cutscene** block (Build layout, Nodes panel > Source). Type a file name for it, e.g.
   `cutscenes\here.json`. Click **Test in game**: with no file yet, it installs just the cutscene script.
5. In game: Reset Scripts. Stand somewhere memorable and click **Make a trigger here** in the remod cutscenes menu.
   Walk at least 5 m away.
6. In remod, click **Use trigger** on the block, then **Open in editor** and add a subtitle so you'll see it, e.g.
   `"subtitles": [{"t": 0, "until": 3, "text": "here"}]`. Click **Test in game** again.
7. In game: Reset Scripts, then walk back to the spot. Does the subtitle appear by itself?

## 3. Talking, story flags, once per save: a ready-made mod

Nothing to make: install one mod and play. Each test cutscene is a few seconds of letterbox with a "remod test: ..."
subtitle. The script logs what it sees (characters near Leon, story flags as they change, prompts, the game's own
cutscenes), so you only say what you saw.

- **Merchant:** "[T] Talk (remod test)" near him; T plays it, every time. (Runs 3: works.)
- **Ashley:** "[H] Talk to Ashley (remod test)"; H plays it, every time. (Runs 1-3: works.)
- **A story flag:** `Location47_002`, which your save turned on in run 3: plays once, about 2 s after the save loads.
- **The control:** needs that flag both on and off, so it must **never** play.
- **Once per save:** "[J] Once per save (remod test)" near Ashley.

The mod is `spikes\out\remod trigger test 3.zip` (rebuilt by `remod run --graph spikes\trigger_test3.json`; the
cutscenes are in `spikes\trigger_test3\`).

8. In Fluffy, uninstall the old `remod trigger test 3` and install the new one.
9. **Load the same save as run 3.** About 2 s after you're in control, does "story flag Location47_002 is on" play?
   Does the CONTROL one ever play?
10. **Once per save:** near Ashley, press J; it plays. Walk away and back: J's prompt shouldn't come back. Save in a
    **new** slot. Load an **older** save (from before you pressed J): J's prompt should be back near Ashley. Then load
    the save you just made: no J prompt.
11. **Story flags:** play a little: open a door, pick up an item, finish a fight, read a file. Note roughly what you
    did, in order (the log has the flags; your list tells me which moment set which).
12. **The game's own cutscenes:** if one plays, nothing to do (the log notes it).

## Take it out

13. Uninstall **remod trigger test 3** in Fluffy (and Trigger test, if you installed it for parts 1 and 2).

## Tell Claude

Only what you saw; the log (`<game folder>\re2_framework_log.txt`) has the rest.
- Part 3: whether the flag test played after loading, and whether the CONTROL ever did (9); J's prompt at each step
  of 10; what you did in 11, in order.

**If something goes wrong:**
- **Nothing starts:** check the Now line isn't "unknown". The "Problem:" line in the menu names anything that failed.
- **The CONTROL plays:** flag reading is wrong; say when.
- **It starts again and again:** it shouldn't. Note what you were doing.
- **The game crashes:** note the step; the log has a line before each change the script makes.
