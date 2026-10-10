# Merchant menu test

**What it answers:** does the game show a 5th entry in the merchant's first menu (Buy, Sell, Upgrade, ...) when we
add one to that menu's layout file, can our script label it, and what does the game do when you pick it? This
decides whether talk topics can live in the merchant's own menu.

**What's in it:**
- An edited copy of the menu's layout (`cs_ui3510.gui`), made from your extracted game files by
  `spikes/gui_add_menu_item` (REE-Lib, MIT). The 5th entry is a copy of the 4th, sharing its parts (the four entries
  already share them), placed 80 below it.
- A script that labels it "Talk (remod test)" and logs what happens. It picks nothing and closes nothing.

**Time:** 5 minutes. **You need:** RE4R with REFramework, Fluffy, your usual save (the merchant nearby).

**Risk:** picking the 5th entry may crash the game: the game's code only knows four. That's an answer too. Save
first if you care about anything since your last save.

**Status:**
- **Run 1 (2026-10-10): works.** The 5th entry showed with our label, the cursor reached it, confirming it did nothing
  (the game ignores an entry it doesn't know), no crash. Its selection highlight was larger than the others'.
- **Run 2 (ready):** two new entries, "Talk" and "Ask about Ashley" (each talk topic can be an entry). Short labels
  (is the big highlight just the long label?); F5 hides / shows the 6th (topics will show or hide by flags);
  confirming either closes the shop the game's own way (where remod will start that topic's cutscene).

- **Run 2 (2026-10-10):** both entries showed with their labels; F5 hid and showed the 6th; confirming did nothing
  (the script never saw the confirm); the highlight on ours still stretched across the screen.
- **Run 3 (ready):** ours copy the "Trade" entry's parts' sizes and positions; confirm is watched several ways
  (the game's confirm later in the frame, mouse click, Enter, Space), and the log says which one fired.
- **Run 3 (2026-10-10): works.** Both entries close the shop, picked by mouse or by the game's own confirm; the
  highlight was the game sizing its four entries in code (ours now copy those sizes). Done: talk topics in the
  merchant's menu are being built into remod (topic test next).

## Steps (run 3)

1. In Fluffy, uninstall the old `remod merchant menu test` and install the new
   `spikes\out\remod merchant menu test.zip`.
2. Load your usual save and talk to the merchant. Is the highlight on "Talk" and "Ask about Ashley" the size of the
   others' now?
3. Confirm on **Talk** with the mouse (click): does the shop close? Talk to him again and confirm on **Ask about
   Ashley** with the keyboard or a controller instead: closes too?
4. Quit the game.

## Taking it out

Uninstall **remod merchant menu test** in Fluffy. It replaces the menu's layout while installed, so take it out.

## Tell Claude

What you saw at 2 and 3 (and whether the game crashed). The log has the rest.

**If the menu looks broken or the game crashes on opening it:** your game's copy of the menu may be newer than your
extracted files (a game update). Say so; the file can be made from the game's current one.

## Rebuilding it

`spikes\make_merchant_menu_test.ps1` (needs the .NET 10 SDK and REE-Lib cloned to
`%LOCALAPPDATA%\remod\tools\RE-Engine-Lib`; pass `-Natives <your natives\stm>` if it isn't the default).
