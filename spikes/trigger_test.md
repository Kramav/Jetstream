# Trigger test

Cutscenes that start by themselves. A cutscene file's `trigger` names where (a spot Leon walks into) and optionally
when (the game's chapter, stage, area or location). The cutscene script checks it five times a second. It's built
into remod's cutscene script, so this is a test of the real feature, not a separate probe.

**What it answers:**
1. Does the script read where Leon is? The menu's **Now** line shows the game's names for the chapter, location,
   area and stage.
2. Does a cutscene start by itself at a spot (example 18)?
3. Does the whole loop work with your own spot: **Make a trigger here** in game, then **Use trigger** in remod?

**Status:** built 2026-10-08, not run yet.

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

## Take it out

8. In remod, click **Remove from game** on the block, and uninstall Trigger test in Fluffy.

## Tell Claude

- What the **Now** line showed at a few places (it's what triggers can name), and whether it ever said
  "(busy: triggers wait)" when nothing was going on.
- Whether example 18 started by itself, and whether your own spot did.

Triggers write "<name> triggered" to `<game folder>\re2_framework_log.txt`.

**If something goes wrong:**
- **Nothing starts:** check the Now line isn't "unknown", and that the file's `trigger` has your spot. Remove
  `chapter` and `stage` from it to try with the spot alone.
- **It starts again and again:** it shouldn't. Note what you were doing.
