# Movie probe (M3 route 2: play a movie when we choose)

**What it answers:** can a Lua script start the game's own movie player on our video, or does that need a C++
REFramework plugin? It lists the game's movie classes and records which of their methods the game calls while a movie
plays. It only looks: nothing in the game or your saves changes.

**Status: run on 2026-10-07; not needed again.** It captured the movie classes and the calls the game made while
the startup logos played. The game then crashed on New Game with the probe's call-watching hooks in, while running
**REFramework v1.5.9 (built March 2025)**: its log said the integrity-check bypass couldn't find what it patches in
the April 2026 game ("Could not find sussy_constant usage!" and similar). With that bypass broken, the game's
anti-tamper check can catch code hooks and crash the game. Call-watching is now off by default (it isn't needed
again).

**Time:** about 10 minutes. **You need:** RE4R with REFramework's latest release, v1.5.9 (the user's choice: no
nightly builds), and remod (`build\app\remod-app.exe`).

## Once: tell remod where the game is

1. Open remod and switch to **Use layout** (the switch above the graph).
2. In the **Pipeline** panel, set **Game folder** to the folder the game is installed in (the one with `re4.exe`).

## Install the probe

3. Switch to **Build layout**. Click **New** in the Pipeline panel (if the open graph has unsaved changes, save them
   first or choose Don't save).
4. In the **Nodes** panel along the bottom, open **Source** (or type `lua` in its search box) and click **Lua script**.
5. Switch to **Use layout**. On the Lua script block, set **Script** to
   `E:\Company\Github\Jetstream\spikes\movie_probe.lua` (type it, or use the `...` button).
6. Click **Test in game**. The status line at the bottom says it copied 1 file.

## Run it

7. Start the game. (If it was already running: press **Insert** for REFramework's menu, open **ScriptRunner**, click
   **Reset Scripts**.)
8. Make a movie play. Start a **New Game** (any difficulty) and let the opening play for about 30 seconds. Any
   pre-rendered movie works, so if the opening isn't one, another movie you can reach is fine. Nothing is saved
   unless you save.
9. Quit the game.

Leave `WATCH_CALLS` off: it hooks every method of the movie classes, and with REFramework v1.5.9 on the current
game that crashed RE4R at New Game.

## Take it out

10. In remod, on the same block, click **Remove from game**.

## Optional, same session: the SDK dump

This helps route 2 and turns on game-name checking for Lua scripts (M2). Do it **last**, because it may crash the game
when it finishes, and that's fine.

11. Start the game again, press **Insert**, open **DeveloperTools**, then **ObjectExplorer**, and click **Dump SDK**.
    Wait until it finishes or the game closes. It writes `il2cpp_dump.json` to the game's folder.
12. In remod's Pipeline panel (Use layout), set **SDK dump** to that file.

## Done

Tell Claude it's done. The result is `<game folder>\reframework\data\remod_movie_probe.txt`, and Claude reads it from
there: nothing to send.

**If something goes wrong:**
- **Test in game says REFramework isn't installed:** the Game folder is wrong, or REFramework isn't in it
  (`dinput8.dll` missing).
- **The game shows a message box naming `movie_probe.lua`:** note what it says and tell Claude.
- **The result file is missing:** the script didn't load. Check that `reframework\autorun\movie_probe.lua` exists in
  the game's folder, and press Reset Scripts.
- **The file has no `CALLED` lines:** no movie played while the probe was in. Try another movie.
