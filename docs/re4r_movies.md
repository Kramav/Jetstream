# RE4R movies: how the game plays them, and adding new ones

**For:** whoever works on movies in remod next (people or Claude), so none of this has to be worked out again.
**Status (2026-10-07):** proven in game. A movie of our own, under a **new movie id**, plays full screen at a moment we
choose, inside a "cinematic state", without replacing any game file. Run 3 of `spikes/new_movie_probe.lua` played
`rmd001` twice, end to end. The game's own movies play the same way; that path is built into remod's cutscene runtime
(example `15_play_a_game_movie`).

Labels: **[game data]** read from the game's files or its running objects; **[dump]** from REFramework's SDK dump;
**[official]** REFramework's documentation; **[inferred]** a conclusion not tested directly.

## 1. How the game finds and plays a movie

```
MovieDefine.ID (an enum: mva000 = 10000)
  -> a movie catalog entry with that id            (chainsaw.MovieCatalog, registered with AppEventManager)
     -> a MovieResource                            (which prefab, display type, sound trigger)
        -> a prefab (.pfb)                         (a via.movie.Movie component, a render texture, a sound container)
           -> the movie file (.mov = an MP4)
Played by chainsaw.MovieMediator: load(id), then play(id).
```

- **Ids [game data, dump]:**
  - `chainsaw.MovieDefine.ID` holds about 300 names: mva000-mva924, mvb, mvc, mve, mvf.
  - **The enum's value is the catalog id:** mva000 = 10000; mva201 = 10201; mva300 = 10300. Catalog ids are UInt16.
- **Catalogs [game data]:**
  - `chainsaw.MovieCatalog` is a userdata whose `_ResourceArray` holds
    `chainsaw.AppEventCatalogBase`1.Resource<chainsaw.MovieResource>` entries: {UInt16 `_ID`, `_Data` (the
    MovieResource), `_FollowData`}.
  - The game registers two at start, both kind 0:
    - `Movie_1st` = {10000} (`_chainsaw/appsystem/catalog/maincontents/moviecatalog_1st.user.2`);
    - `Movie_2nd` = {10201, 10202, 10300, 10301}.
  - Registered with `chainsaw.AppEventManager.registerCatalog(catalog, Kind)`. The manager builds its controllers
    then [inferred from `_EventControllerCatalogArray`]: an entry added to a catalog after registering isn't seen. A
    whole new catalog registered later is seen.
  - Read back with `AppEventManager.getMovieResource(id)`.
- **MovieResource [dump, game data]** (file `<id>.user.2`):
  - `_MoviePrefab` (4K);
  - `_ExceptionalMoviePrefabs` (an array; mva000's holds the `_FHD` prefab);
  - `_DisplayType`: 0 for mva000; the enum has FullScreen, GUI, SetObject, UpperHalf, UnderHalf;
  - `_Option`, a `MovieResource.OptionParam`: subtitle GUIDs and `_WwiseTriggerID`.
- **Prefabs [game data]:**
  - **4K:** `mva000.pfb.17.x64`, path `@_Chainsaw/Movie/mv/mva000/mva000.pfb`. It names:
    - `@_Chainsaw/Movie/mv/mva000/mva000.mov`;
    - the render texture `_Chainsaw/UI/ui_system/Movie/tex/cs_MovieRTT_4K.rtex`;
    - the sound container `_Chainsaw/Sound/Resource/Container/mv/snd_cont_mva000.user`.
  - **1080p:** `mva000_fhd.pfb.17`, path `_Chainsaw/Movie/mv/mva000/mva000_FHD.pfb`. It names `.../mva000_FHD.mov` and
    `cs_MovieRTT_1080p.rtex`.
  - mva300's prefab shows on an in-world texture (`RTT_sm61_568.rtex`) instead.
- **Movie files [game data]:**
  - `natives/STM/streaming/_chainsaw/movie/mv/<id>/<id>.mov.1.x64` and `<id>_fhd.mov.1.x64` are the MP4s.
  - The same names outside `streaming/` are a 38-byte stub (`REMV`, naming `dummy.mp4`), identical for every movie.
- **The load table [dump]:** `MovieMediator.getLoadInfo(id)` reads `movieloadtable.user`. That holds per-id flags
  (IsOccupiedPlayer, IsChapterStart / End, IsEnding, IsGameOver, HasNextMovie) and stage and character lists.
- **Playing [dump, game data]:**
  - `chainsaw.MovieMediator`, an AppSingleton: `get_Instance()`, a static method on its parent class, found
    through it.
  - Calls: `load(id)`, `IsLoaded(id)`, `play(id, Action justPlay, Action justEnd)` (nil for both is fine),
    `isPlaying()`, `requestSkip(id)`, `unload(id)`, `getWork(id)` (its `_MoviePhase`).
  - Loading takes about 0.1 s.

## 2. The cinematic state

All of these are the game's own mechanisms; none hooks a game method.

| What | How | Seen |
|---|---|---|
| World paused (enemies too) | `share.PauseManager` (`get_Instance`) `requestStartPause(share.PauseManager.PauseType, System.String, System.Action)` with `EventMovie`, an owner name (`sdk.create_managed_string`), nil; `requestEndPause` with the same arguments | Leon, enemies and his animation stood still |
| Player can't be controlled | `chainsaw.CharacterManager.requestOperationStop(CharacterControlIndex.Player_1, character.PauseLayer.Self)`, **every frame** before UpdateBehavior | Works even with a direction held (`spikes/hud_freeze_probe.md`) |
| HUD hidden | `chainsaw.OptionManager` `setCurrentOptionValue(option.OptionID.DisplayUI, 0)`, the previous value put back after | It's the player's saved option: a crash mid-movie leaves it at 0 |

**Measured** (runs 1-3; position, rotation, HP, PTAS, kill count and animation read before the movie, every 0.5 s
during it, on restore and one second after):
- **During the movie and on restore:** unchanged, every time, for both the game's mva000 and our rmd001.
- **One second after:** changes come from play resuming: your own input, or an enemy attack in progress (run 1:
  -346 HP).
- **Not frozen:** `share.GameClock`'s Game and ActualPlaying times count the movie's length. DemoSpending and
  PauseSpending don't move. Freezing play time would mean changing the clock; not done.

## 3. Adding a new movie (what works)

### Files, all at new paths

The name must be the same length as `mva000` (6 characters, e.g. `rmd001`). That lets the prefabs be byte-patched:
replacing a string with one of another length would move everything after it.

| File (under the mod's `natives/STM/`) | What | Made from |
|---|---|---|
| `streaming/_chainsaw/movie/mv/rmd001/rmd001.mov.1.x64` | The 4K MP4 | remod's Replace movie on mva000 with the video (Same length off, Replace its sound off): H.264 at mva000's size, frame rate and bit rate |
| `streaming/_chainsaw/movie/mv/rmd001/rmd001_fhd.mov.1.x64` | The 1080p MP4 | Replace movie's 1080p copy |
| `_chainsaw/movie/mv/rmd001/rmd001.mov.1.x64`, `rmd001_fhd.mov.1.x64` | The 38-byte stubs | Copies of mva000's |
| `_chainsaw/movie/mv/rmd001/rmd001.pfb.17.x64` | The 4K prefab | mva000's, with the UTF-16 string `mv/mva000/mva000` replaced by `mv/rmd001/rmd001` (2 places) |
| `_chainsaw/movie/mv/rmd001/rmd001_fhd.pfb.17` | The 1080p prefab | The same on `mva000_fhd.pfb.17` (2 places) |

Notes on these files:
- **Sound container:** the patch leaves it as mva000's. The script clears the movie's sound trigger instead. No sound
  was heard; see the open questions.
- **Case:** the paths inside say `_FHD` while the files are `_fhd`, as in the game's own: the game ignores case.
- **The builder:** `spikes/make_new_movie.ps1` builds all of these from the user's extracted game files, plus the
  probe and `modinfo.ini`, as a Fluffy zip. Game files are never committed.
- **Installing:** Fluffy installs them as loose files, and the game finds new paths.

### The `@` rule (run 2's failure)

A resource path starting with `@` names a file that has a platform version. The game adds the platform suffix:
`@_Chainsaw/.../mva000.pfb` is the file `mva000.pfb.17.x64`. Without the `@` it looks for `.pfb.17`.

- In RE4R, mva000's 4K prefab and both `.mov` paths have an `@`; the 1080p prefab path doesn't (its file has no
  `.x64`).
- **Always take a path from the game's own object and change only the name**, never type it.
- `via.Prefab.get_Exist()` says at once whether the game can see the file: run 2's 4K prefab read `false`, which was
  the whole problem.

### Registering it at runtime (Lua, REFramework)

What run 3 did; each step is in `spikes/new_movie_probe.lua` (`register`):

1. **Pick the id.** The first id from 10950 up that no catalog entry or enum value uses (10950 in run 3). It stays
   within mva's range: mva924 = 10924 is the last used, so 10925-10999 are free.
2. **The MovieResource:** `sdk.create_instance("chainsaw.MovieResource", true)`, then fill it from mva000's (the
   entry `Movie_1st:get_ResourceArray():get_element(0):get_Data()`):
   - `set_MoviePrefab`: a new `via.Prefab` (`sdk.create_instance("via.Prefab", true)`, `set_Path`) on mva000's prefab
     path with `mva000/mva000` -> `rmd001/rmd001`;
   - `set_ExceptionalMoviePrefabs`: a new `via.Prefab` array (`sdk.create_managed_array("via.Prefab", 1)`), holding
     the 1080p prefab made the same way;
   - `set_DisplayType`: mva000's;
   - `set_Option`: an OptionParam (created, or a copy of mva000's) with `set_WwiseTriggerID(0)`.
3. **The catalog entry:** a `MemberwiseClone` of mva000's entry. `sdk.create_instance` of its generic type gives
   nil, even with simplify. Then `set_ID(id)`, `set_Data(resource)`; `_FollowData` stays shared with mva000's.
4. **The entry array:** `Clone()` of `Movie_1st`'s one-element array, then `SetValue(System.Object, System.Int32)`
   at 0. Reading it back with `GetValue` confirms the set took.
5. **The catalog:** `sdk.create_instance("chainsaw.MovieCatalog", true)`, then:
   - `set_KeyName("remod_movie")`;
   - `_KeyNameHash` set to a constant (0x7e3d0001);
   - `set_Kind` to Movie_1st's;
   - `set_ResourceArray`.

   Then `AppEventManager:registerCatalog(catalog, kind)`. `getMovieResource(id)` returning our resource confirms it.
6. **The load table:** a `MemberwiseClone` of mva000's LoadInfo (`getLoadInfo(10000)`) with `set_MovieID(id)` and the
   chapter flags false, added with `get_MovieLoadTable():get_LoadInfoList():Add(info)`. Untested whether this is
   needed.
7. **Every object made:** `:add_ref()`, so it isn't collected. Do it once per game session: nothing is saved.

Then play it as the game's own (section 1), inside the cinematic state (section 2).

### What didn't work (so it isn't tried again)

- **`sdk.create_userdata("chainsaw.MovieResource", "<our .user path>")`** gave an object with no prefab: the file's
  content wasn't loaded (run 1). So no `.user` is shipped; the resource is built in Lua.
- **`sdk.create_instance(type)` without `simplify = true`** gave nil for the catalog entry type (run 1). REFramework's
  book: "set simplify to true if the function returns nil" [official]. Even with simplify, the generic entry type
  gives nil: copy one with `MemberwiseClone` instead.
- **A typed prefab path without its `@`** (run 2): `get_Exist` false, `load` never finished (MovieWork phase 1 for 20
  s), and a solid blue screen (the movie window with nothing to show [inferred]).
- **An id one past every enum value** (run 1 picked 60913, past some high enum value). Not wrong as far as we know,
  but ids beside the game's are the safer guess.

## 4. Open questions

- **Sound.** Story movies' sound comes from Wwise (the prefab's sound container and the resource's trigger), not from
  the MP4. Our movies are silent so far.
  **Sound probe run 1 (user, 2026-10-07: "both work") [game data, `remod_sound_probe.json`]:**
  - **A brand-new sound plays from a script (F5).** Our bank `remod_snd001` loaded with
    `create_resource("via.simplewwise.BankResource", "@_Chainsaw/Sound/Wwise/remod_snd001.sbnk")`, then a holder
    kept by Leon's container. Its event, posted through a cloned trigger info, played **on the first press**
    (Wwise playing id 290, then 291, 292...) and was heard. Six presses, all played.
  - **The game's own sounds play on command (F7).** Leon's `chainsaw.SoundContainerApp` has 727 triggers; trigger
    937148 (event 2648637722) played and was heard.
  - **Not tried yet:** `rmd002`, the movie with sound in its file (nothing in the log).

  The two ways being tried (`spikes/sound_probe.md`):
  1. **Sound in the movie file.** The logo movies (mv7000 / 7001) have AAC in theirs, and `via.movie.Movie` has
     audio getters (`get_CurrentAudioTime`, `get_AudioRequestId`). Wwise also has an audio-input event for it
     (`via.simplewwise.Driver.get_PlayAudioInputEventId`) [dump]. New movie's advanced "Sound in the movie file"
     writes AAC 48 kHz stereo; probe movie `rmd002`.
  2. **A brand-new bank.** `remod new-sound` / core `new_sound_bank`:
     - **Source:** a copy of `ch_csa404_se.sbnk.1.x64` (35 KB). It has one Event (4), Action (3) and Sound (2),
       three actor-mixers (7) and an attenuation (14), and one in-bank mono Vorbis sound. 15 such banks exist, the
       smallest found by a scan.
     - **New ids:** every id it defines (BKHD's bank id, each HIRC object's id, the media id) becomes
       FNV-1("<name>/<old id>"), replaced wherever it's written as 4 bytes. Ids it only references (parents
       outside the bank) stay.
     - **Its sound:** our audio, as `encode_wem` in the original's form, with the recorded in-memory size updated.
     - **Checked:** the event still points at its action and the action at its sound, no old id is left, and the
       sound decodes.
     - **Playing it:** the probe loads it with `sdk.create_resource("via.simplewwise.BankResource",
       "@_Chainsaw/Sound/Wwise/remod_snd001.sbnk")`, then `create_holder("via.simplewwise.BankResourceHolder")`,
       added to a sound container's `BankResourceList`. Banks are named with a leading `@`, as in the game's trigger
       files: `@_Chainsaw/Sound/Wwise/ch_csa404_se.sbnk`. It then posts the event through a `MemberwiseClone` of
       one of the container's `soundlib.SoundTriggerInfo` with `_EventId` set:
       `soundlib.SoundContainer.trigger(SoundTriggerInfo)` returns a request id, and
       `via.simplewwise.Driver.getPlayingIdByRequestId` reads non-zero once Wwise plays it.
  - **Game sounds on command (F7):** the same trigger call with a container's own trigger infos (`_TriggerInfoList`:
    `_TriggerId`, `_EventId`). Containers: `chainsaw.SoundContainerApp` (a `soundlib.SoundContainer`), Leon's or
    the scene's (`findComponents`). Containers load trigger lists from `.user` files
    (`_Chainsaw/Sound/Resource/Trigger/.../snd_trgr_<x>.user`), which name banks.
- **Many movies.** 50 or more should be one catalog with many entries, `rmd001`-`rmd999` [inferred]. Not tried with
  more than one.
- **1080p only:** pointing both prefabs at one file, to halve the mod's size. Untried.
- **Play time:** the game's clocks count a movie (section 2).
- **Subtitles:** the OptionParam's subtitle GUIDs (the game's message system). Untried.

## 5. In remod (built 2026-10-07)

- **The New movie block** (`NewMovie`, core `nodes.cpp`) makes section 3's files from a video. Its fields are a name
  (`new_movie_name_problem`: 6 lowercase letters, digits or `_`, not `mv...`) and Your video (empty: a test card).
  It makes:
  - the MP4s, with Replace movie's encoder (`encode_movie_copies`), at the video's own length;
  - the stubs (mva000's, under the new name);
  - the prefabs (`rename_movie_paths`);
  - the note `reframework/data/remod_movies/<name>.json`;
  - the cutscene runtime.

  It reads mva000's files from `{game}`, and an unchanged run reuses everything from the run cache.
- **The cutscene runtime** (`runtime/remod_cutscene.lua`) loads the notes. A cutscene's `movies` entry naming one
  registers it the first time it plays (`register_movie`: section 3's steps, from 10950 up, one catalog per movie).
  It then plays like a game movie. The menu lists each new movie with a Play button.
- **Example `16_insert_a_new_movie`:** New movie (rmd001, a test card) and a Cutscene
  (`examples/cutscenes/play_new_movie.json`: fade out, rmd001, fade in; F8), both into Package.
- **Checked:**
  - a test on a fake game folder: every file at its path, the prefabs renamed, the sound container left, an
    unchanged run, bad names;
  - example 16 built from the real game files: 9 files, the prefabs naming `@_Chainsaw/Movie/mv/rmd001/rmd001.mov`.
- **Not yet seen in game:** the runtime's registration (the same calls as the probe's run 3), and example 16 itself.

## 6. Where things are

- **The probe:** `spikes/new_movie_probe.lua`. F5 plays the new id, F7 the game's mva000, F6 stops. It writes
  `reframework/data/remod_new_movie_probe.json`.
- **Its guide:** `spikes/new_movie_probe.md`, with runs 1-3.
- **The mod builder:** `spikes/make_new_movie.ps1`.
- **Game movies in cutscenes:** `runtime/remod_cutscene.lua`, `GAMES.re4.movie_start` / `movie_update` /
  `movie_stop`; the `movies` list in cutscene files; example `15_play_a_game_movie`.
- **Probe lessons:** CLAUDE.md §9, "Finding a game's switch from the SDK dump".
