# Spikes

Short experiments that answer a question only the real game (or a real tool) can. Each has a step-by-step guide.
Spikes don't hook game methods (`sdk.hook`): REFramework's latest release, v1.5.9, can't hide hooks from the current
game's anti-tamper check (it crashed the game during the movie probe), and players may run it. REFramework's own
callbacks (`re.on_frame`, `re.on_pre_application_entry`) are fine. (The user updated to a newer REFramework build on
2026-10-07.)

| Spike | Answers | Guide | Status |
|---|---|---|---|
| Movie probe | Can Lua start the game's movie player on our video? (M3 route 2) | [movie_probe.md](movie_probe.md) | Run 2026-10-07: data captured; the game then crashed (old REFramework, see the guide) |
| Cutscene probe | Camera hook, finding Leon, animations from Lua (M3 route 3) | [cutscene_probe.md](cutscene_probe.md) | Run 2026-10-07: all four answered (see the guide); not needed again |
| Wwise | Wwise's WEM layouts, to write them without Wwise | [wwise/README.md](wwise/README.md) | Done (2026-10-06) |
