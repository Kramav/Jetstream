# Topic test

**What it answers:** do talk topics work as entries in the merchant's menu, built the way any remod mod will build
them? remod now makes the menu's layout itself (four spare entries added to the game's file), and its cutscene script
puts topics into those spares, shows or hides them by flags, and plays a topic's cutscene when you pick it.

**What's in it** (`spikes\out\remod topic test.zip`, built by remod from `spikes\topic_test\`):
- **Talk:** always in the menu. A 4-second test cutscene (a subtitle and fades).
- **Ask about Ashley:** in the menu only once Talk has played in this save (its trigger needs the flag
  `remod:topic_talk`).
- The merchant's menu layout with the spare entries, and remod's cutscene script.

**Time:** 5 minutes. **You need:** RE4R with REFramework, Fluffy, a save near the merchant, and a second save slot
you can overwrite.

## Steps

1. In Fluffy, **uninstall `remod merchant menu test`** (it changes the same menu), then install
   `spikes\out\remod topic test.zip`.
2. Load your save and talk to the merchant. Is **Talk** the only new entry, sized like the others, with no icon?
   Does the cursor go from Trade to Talk and stop there, or does it land on empty space below?
3. Pick **Talk** (any way: click, keyboard or controller). Does the shop close and the Talk cutscene play?
4. Talk to the merchant again. Are **Talk** and **Ask about Ashley** both there now? Pick **Ask about Ashley**:
   does its cutscene play?
5. Save to the spare slot. Load your first save again (from before step 3), talk to the merchant: is **Talk** the
   only new entry again? Then load the spare slot: is **Ask about Ashley** back?
6. Quit the game.

## Taking it out

Uninstall **remod topic test** in Fluffy. It replaces the merchant's menu layout while installed.

## Tell Claude

What you saw at steps 2-5, and whether the game crashed. The log has the rest (which topics showed, what you picked,
what played).

**If the menu looks broken or the game crashes when it opens:** your game's copy of the menu may be newer than your
extracted files (a game update). Say so; the layout can be made from the game's current one.

## Rebuilding it

`remod run --graph spikes\topic_test\topic_test.json` (the CLI finds your game files as the app does).
