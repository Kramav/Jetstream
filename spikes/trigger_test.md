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
- **Part 3 (built 2026-10-09, not run):** a ready-made mod (`spikes\out\remod trigger test 3.zip`): talking to the
  merchant and Ashley, story flags (one test cutscene per difficulty), once per save, watching flags, after the
  game's own cutscene.

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

## 3. Talking to people, story flags, once per save: a ready-made mod

Nothing to make: install one mod, play, and read the menu. It holds remod's cutscene script and six short test
cutscenes (each is a few seconds of letterbox with a "remod test: ..." subtitle):
- **Talk to the merchant:** near him, "[G] Talk (remod test)"; G plays it. Once per save.
- **Talk to Ashley:** near her, "[H] Talk to Ashley (remod test)"; H plays it. A control: her kind (`ch2_a1z0`) is
  certain, the merchant's (`ch3_a8z0`) is a guess.
- **One per difficulty:** each needs its story flag (`DifficultyStandard` and so on), so only your save's difficulty
  should play, 2 s after the save loads.

The mod is `spikes\out\remod trigger test 3.zip` (rebuilt by `remod run --graph spikes\trigger_test3.json`; the
cutscenes are in `spikes\trigger_test3\`).

8. **Clear the way.** In Fluffy, uninstall the other remod test mods (Trigger test, the examples, the probes): they
   carry their own copy of the cutscene script. In remod's Pipeline panel, under **Scripts in the game**, remove
   `remod_cutscene.lua` if it's listed (a Test in game copy). Then install `remod trigger test 3.zip` with Fluffy.
9. **Start the game and load a save.** About 2 s after you're in control, one "story flag Difficulty... is on" should
   play: is it your save's difficulty? (None, or the wrong one: note it.)
10. Open REFramework's menu (Insert) > Script Generated UI > **remod cutscenes** > **Story flags**. Read the
    **Difficulty flags** line (one should say ON) and the number of story flags under it.
11. **Ashley** (if she's with you): walk up to her. Does "[H] Talk to Ashley (remod test)" show at the bottom of the
    screen, and does H play its cutscene? Walk away and back: the prompt shouldn't show again until you restart the game (this
    one is once per game run).
12. **The merchant:** walk up to him. Does "[G] Talk (remod test)" show, and does G play it? Does his own "Trade"
    still work beside it? If no prompt shows, open **Characters near Leon (talk triggers)** next to him and note the
    names listed and their distances.
13. **Once per save:** after the merchant's has played, walk away and back: no prompt. Save in a new slot. Then load
    a save from **before** you talked to him: the prompt should be back. Load the save you just made: no prompt.
14. **Watching flags:** in **Story flags**, tick **Watch**. Do something the story counts (open a door the story
    needs, pick up a key item, finish a fight, read a file). Do flags appear in the list as they change, with
    readable names? Type part of one into **Find a flag**: are they listed with on / off?
15. **The game's own cutscenes:** if one of the game's cutscenes or movies plays, does the menu then say "The game's
    last cutscene: csa..., ended N s ago" (or movie)?

## Take it out

16. Uninstall **remod trigger test 3** in Fluffy (and Trigger test, if you installed it for parts 1 and 2).

## Tell Claude

- What the **Now** line showed at a few places (it's what triggers can name), and whether it ever said
  "(busy: triggers wait)" when nothing was going on.
- Parts 1 and 2: whether example 18 started by itself, and whether your own spot did.
- Part 3, by step: which difficulty played (9) and the Difficulty flags line and flag count (10); Ashley's prompt and
  H (11); the merchant's prompt, G and Trade, or the names Characters near Leon listed (12); once per save (13);
  a few flag names the watch showed and what you'd just done (14); the game's cutscene line, if one played (15).

Triggers write "<name> triggered" to `<game folder>\re2_framework_log.txt`.

**If something goes wrong:**
- **Nothing starts:** check the Now line isn't "unknown". The "Problem:" line in the menu names anything that failed.
- **A difficulty test plays for the wrong difficulty, or several play:** note which; flag reading is then wrong.
- **It starts again and again:** it shouldn't. Note what you were doing.
- **The game crashes:** note the step; the log has a line before each change the script makes.
