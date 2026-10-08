# Spikes

Short experiments that answer a question only the real game (or a real tool) can. Each has a step-by-step guide.
Spikes don't hook game methods (`sdk.hook`): REFramework's latest release, v1.5.9, can't hide hooks from the current
game's anti-tamper check (it crashed the game during the movie probe), and players may run it. REFramework's own
callbacks (`re.on_frame`, `re.on_pre_application_entry`) are fine. (The user updated to a newer REFramework build on
2026-10-07.)

| Spike | Answers | Guide | Status |
|---|---|---|---|
| Movie probe | Can Lua start the game's movie player on our video? (M3 route 2) | [movie_probe.md](movie_probe.md) | Run 2026-10-07: data captured; the game then crashed (old REFramework, see the guide) |
| New movie probe | Our movie under a new movie id, in a cinematic state (paused, player locked, state unchanged after) (M3 route 2) | [new_movie_probe.md](new_movie_probe.md) | Done 2026-10-07 (run 3): our movie under a new id plays in the cinematic state; everything in [docs/re4r_movies.md](../docs/re4r_movies.md) |
| Character probe | Other characters in cutscenes: find them, take them over (the game's cutscene lock), animate and place them | [character_probe.md](character_probe.md) | Built 2026-10-07, not run |
| Sound probe | A game sound on command, a brand-new sound (our own bank), and a new movie's in-file sound | [sound_probe.md](sound_probe.md) | Run 1, 2026-10-07: a game sound on command and a brand-new sound both play; the movie with in-file sound not tried yet |
| Cutscene probe | Camera hook, finding Leon, animations from Lua (M3 route 3) | [cutscene_probe.md](cutscene_probe.md) | Run 2026-10-07: all four answered (see the guide); not needed again |
| HUD and freeze probe | Freezing Leon and hiding the HUD during a cutscene, with the game's own switches (M3 route 3) | [hud_freeze_probe.md](hud_freeze_probe.md) | Done 2026-10-07: HUD option 0; operation stop (layer Self) stops Leon |
| Wwise | Wwise's WEM layouts, to write them without Wwise | [wwise/README.md](wwise/README.md) | Done (2026-10-06) |
