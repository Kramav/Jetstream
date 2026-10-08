# Sound probe

Sound for new movies, and playing sounds from a script. One game session answers three questions:

1. **Can a script play one of the game's own sounds on command?** (F7)
2. **Can it play a brand-new sound?** It's in a sound bank of our own, beside the game's: nothing replaced. (F5)
3. **Does the game play sound that's inside a new movie's file?** The game's logo movies keep their sound there. If
   it does, new movies get sound just by encoding it into the movie.

**Status: run 1, 2026-10-07: F5 and F7 work.** The user heard both.
- **F5:** the brand-new bank loaded, and its sound played on the first press, every press.
- **F7:** Leon's container has 727 sounds; the first played.
- **Not tried yet:** the movie with sound in its file (`rmd002`).

The details are in [docs/re4r_movies.md](../docs/re4r_movies.md) §4.

**Time:** about 10 minutes. **You need:** RE4R with REFramework, Fluffy, and a save to load. It hooks no game
methods.

## What's in the mod

`spikes\out\remod sound probe.zip`, built by `spikes\make_sound_probe.ps1` from your
extracted game files. It holds:
- **`remod_snd001`, a new sound bank.** It's a copy of the game's `ch_csa404_se` bank, which has one event and one
  sound. Every id inside it is new, and its sound is 3 seconds of beeps (made by `remod new-sound`). A small note
  says its path and event id.
- **`rmd002`, a new movie** (a 12-second test card) with a beep each second in its file, plus remod's cutscene
  script, which plays it.
- **The probe script** (`remod_sound_probe.lua`).

## Run it

1. In Fluffy, add the zip and install it. If the new movie probe mod is still installed, it can stay.
2. Start the game and load a save. White text at the top left shows the keys and what happened last.
3. **F7: a game sound.** It plays one of the sounds from Leon's own sound container. Do you hear anything? The
   "last" line says whether the game is playing it ("Wwise playing id" above 0). Press **F8** for the next sound,
   then **F7** again. Try a few.
4. **F5: the brand-new sound.** Do you hear about 3 seconds of beeps? Read the "last" line. If it says NOT
   playing, press F5 once more after a second: the bank may still have been loading the first time.
5. **The movie with sound:** open REFramework's menu (Insert) > Script Generated UI > remod cutscenes, and click
   **Play** beside "New movie rmd002". Do you hear a beep each second over the test card?

## Take it out

6. Uninstall the mod in Fluffy.

## Tell Claude

- F7: heard anything? For which sounds (F8 count)?
- F5: heard the beeps? On the first press or the second?
- rmd002: heard the beeps over the movie?

Claude reads the rest from `<game folder>\reframework\data\remod_sound_probe.json`: every step, the request ids and
whether Wwise played them, and how the bank was loaded.

**If something goes wrong:**
- **A sound won't stop:** load another save, or restart the game.
- **The game crashes:** note what you pressed, then uninstall the mod in Fluffy. The JSON and the log
  (`re2_framework_log.txt`) show how far it got.
