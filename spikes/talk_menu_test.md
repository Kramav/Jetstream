# Talk menu test

**What it answers:**
- Does talking to Ashley open remod's talk menu, and does picking a topic there play its cutscene?
- Do topics show in the order their files ask for?
- **Since 2026-10-13:** does remod's cutscene script still work in RE4R now that it's split in two (a shared part,
  and `remod_cutscene\re4.lua` with everything that names RE4's own code)? This test runs most of the moved code:
  the Interact icon, the talk menu's input, the merchant's menu, saves and loads, playing a cutscene. Step 7 adds
  new sounds.

remod draws this menu itself: the game has no list menu a script can open. It's opened by the game's own Interact
icon over her.

**What's in it:** example 21, built: `examples\re4r\mods\Talk topics.zip`.

| Where | Entry | Shows |
|---|---|---|
| Merchant's menu | **Talk** | always, first |
| Merchant's menu | **Ask about Ashley** | once the merchant's Talk has played in this save, second |
| Ashley (Interact) | **How are you holding up?** | always, first |
| Ashley (Interact) | **About the merchant** | once the merchant's Talk has played in this save, second |

Each topic is a 4-second test cutscene: a subtitle and fades.

**Time:** 5 minutes.

**You need:**
- RE4R with REFramework, and Fluffy;
- a save where Ashley is with Leon and a merchant is near.

## Steps

1. In Fluffy, **uninstall `remod topic test`**: it changes the same menu. Then install
   `examples\re4r\mods\Talk topics.zip`.
2. Load your save. Walk up to Ashley.
   - Does the game's Interact icon show over her?
   - Press Interact: does a **Talk** list open with only **How are you holding up?**
   - Does Leon stay still while it's open?
3. Press back (Escape, or your controller's back button). Does the list close and Leon move again?
4. Open it again and pick **How are you holding up?** (Enter, or your controller's confirm). Does its cutscene play?
5. Go to the merchant. In his menu, is **Talk** above **Ask about Ashley**? Before the merchant's Talk has played,
   Ask about Ashley shouldn't be there yet. Pick **Talk**.
6. Back at Ashley, open the list. Is it now **How are you holding up?** then **About the merchant**?
   - Move between them with the arrow keys or W / S, and with your controller if you play with one.
   - Pick **About the merchant**: does its cutscene play?
7. Optional, for the split: also install `examples\re4r\mods\New sound.zip` (example 17) and press **F7**. Do you
   hear three beeps?
8. Quit the game.

## Taking it out

Uninstall **Talk topics** (and **New sound**) in Fluffy. Talk topics replaces the merchant's menu layout while
installed.

## Tell Claude

Tell me, with the step:
- what you saw at steps 2-6;
- whether the controller moved the list, if you tried one;
- whether the game crashed.

The log has the rest: what the list held, which key or button moved it, what played.

**If there's no icon over Ashley:** a "[T] Talk" prompt shows instead (the log says why), and T opens the list.

## Rebuilding it

`remod run --graph examples\re4r\21_talk_topics.json` (the CLI finds your game files as the app does).
