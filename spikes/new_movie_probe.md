# New movie probe

Part of M3 route 2: playing **our own movie, under a new movie id**, at a moment we choose, without replacing any of the
game's movies. It also tests the **cinematic state** that goes with it:
- When the movie starts, normal play stops: the world is paused, Leon can't be controlled and the HUD is hidden.
- While it plays, nothing about Leon changes: position, facing, health, money, kills, animation.
- When it ends, play resumes exactly where you were.

**What it answers:**
1. Can a movie with new files and a new id be registered and played from a script?
2. Does the game's own event-movie pause freeze everything around Leon?
3. Does anything about him change between before and after?

**Status: done (2026-10-07, run 3): F5 works.** Our movie played under the new id 10950 for its full 12 s, twice,
and Leon was unchanged during it and on restore. Everything learned, and how to add a movie, is in
[docs/re4r_movies.md](../docs/re4r_movies.md). The runs:

**Run 1:**
- **F7 (the game's `mva000`) works.** It played full screen, with the world paused, Leon held and the HUD hidden.
  While it played, nothing about Leon changed. On restore, his position, facing, health and animation were as before.
- **The game's clocks kept running:** game time and play time went up by the movie's length. The time counted as
  "demo" and "pause" didn't move.
- **One second after resuming,** he had moved about 1 m and lost 346 health (784 to 438). That's what an enemy
  attack resuming would do; not yet confirmed.
- **F5 (the new id) failed.** `create_userdata` returned a MovieResource with no prefab (the `.user` didn't load
  from that path), and creating a catalog entry from the game's entry type gave nothing (an attempt to index a nil
  value). To fix before the next run.
- **What the run showed about ids:** the catalogs are Movie_1st (10000) and Movie_2nd (10201, 10202, 10300, 10301),
  both kind 0. The enum value is the catalog id (mva000 = 10000).

The F7 path is now in remod's cutscene runtime (`movies` in a cutscene file, example `15_play_a_game_movie`).

**Run 2 (the zip rebuilt 2026-10-07; the text at the top left says "run 2"):** F5 is fixed in two ways.
- **Every new object is made a different way:** REFramework's simplified create (its book says to use it when the
  plain one gives nil), else a copy of the game's own mva000 objects.
- **No settings file is loaded any more:** the movie's settings are copied from mva000's in the script, then pointed
  at our prefabs.

Each step's note says how it was made, and whether the game can see our prefab files ("exists"). The new id is now
10950, beside the game's (mva000 is 10000).

**Run 2 result:** the new id registered; the game found our movie under 10950. But it never finished loading, and
the screen stayed solid blue (the game's movie window with nothing in it [inferred]). The game couldn't see the 4K
prefab (`exists false`); it could see the 1080p one. The game's own 4K path starts with `@`
(`@_Chainsaw/Movie/mv/mva000/mva000.pfb`), which marks a file with a platform version: on disk it's `.pfb.17.x64`.
The probe had typed the path without the `@`, so the game looked for `rmd001.pfb.17`. The 1080p path has no `@`
(file `.pfb.17`), so it was found.

**Run 3 (the zip rebuilt; the text says "run 3"):** the prefab paths are taken from mva000's own, with only the name
changed, so the `@` stays. To run it: in Fluffy, **uninstall the old one first**, add the rebuilt zip and install
it. Then do steps 2 to 5 below, pressing **F5**. You should see a moving colour test pattern with a timer for 12
seconds. Solid blue means it still didn't load.

**Time:** about 10 minutes. **You need:**
- RE4R with REFramework;
- Fluffy Mod Manager;
- a save to load, ideally somewhere quiet with an enemy in view, to see that it freezes too.

It hooks no game methods.

## What's in the mod

`spikes\out\remod new movie probe.zip`. It's already built; to build it again, run
`spikes\make_new_movie.ps1`. It holds:
- **The movie `rmd001`**, at new paths beside the game's movies:
  - a 12 s test pattern at the game's 4K size, plus its 1080p copy;
  - the prefabs and settings the game needs, copied from the intro movie's (`mva000`) with only the paths changed.

  None of the game's files is replaced.
- **The probe script** (`reframework\autorun\remod_new_movie_probe.lua`). It registers `rmd001` under a new id with the
  game's movie system, then plays it the way the game plays its own.

## Run it

1. In Fluffy, add `remod new movie probe.zip` and install it.
2. Start the game and load a save. White text at the top left shows the keys and the state. If it's missing, check
   that REFramework's menu (Insert) > ScriptRunner lists `remod_new_movie_probe.lua`.
3. Note Leon's health, and take a little damage first if it's full, so a change would show. Note your money, and
   where he stands and faces.
4. **Run (hold a direction) and press F5.** What should happen:
   - the HUD goes and Leon stops at once;
   - the game pauses (enemies freeze);
   - the test pattern plays full screen for 12 seconds.
5. When it ends, play should resume where you were. Read the **last result** line. It lists how far Leon moved and
   turned, his health, money, kills and animation if any changed, and how far each of the game's clocks moved. A
   second line follows one second later.
6. If F5 didn't play the movie, press **F7**. It plays the game's own intro movie (`mva000`) the same way, so we can
   tell whether the new id or the playing is the problem. Press **F6** to stop it early.

**F6** stops a movie at any time and restores normal play.

## Take it out

7. Uninstall the mod in Fluffy.

## Tell Claude

- Did F5 play the test pattern full screen? Did Leon stop, did enemies freeze, did the HUD go?
- Did any sound play during it? The intro's music would mean its sound comes from somewhere the probe doesn't clear.
- Did play resume where you were? Read out the "last result" lines.

Claude reads the rest from `<game folder>\reframework\data\remod_new_movie_probe.json`. That file records:
- every step and what the game answered;
- the movie catalogs and ids it found;
- Leon's state before, during and after.

**If something goes wrong:**
- **Stuck paused, or with no control:** press F6, or REFramework's menu > ScriptRunner > Reset Scripts.
- **The HUD stays hidden:** set Display HUD back in the game's options. The probe puts it back itself, but a crash
  can't.
- **The game crashes:** note what you pressed, then uninstall the mod in Fluffy. The log
  (`<game folder>\re2_framework_log.txt`) and the JSON show how far it got.
