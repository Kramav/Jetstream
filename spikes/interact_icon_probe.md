# Interact icon probe

**What it answers:** can our talk triggers show the game's own interact icon (the large key icon the merchant and
doors show) instead of the drawn "[T] Talk" text? If it works, the icon shows the right key for keyboard or
gamepad, and a talk trigger could start on the game's Interact button.

**How:** the probe copies the merchant's own interaction trigger, points the copy's icon at Ashley's head, and asks the
game every frame to draw it, the way the game's interactions ask. It changes nothing of the merchant's. It hooks no
game methods.

**Time:** about 5 minutes. **You need:** RE4R with REFramework, Fluffy, your usual save (the merchant and Ashley
nearby, as in the trigger tests).

**Status:**
- **Run 1 (2026-10-09):** no icon at Ashley; F6's input check threw every frame (REFramework caught it, the game
  went on). Our hand-made work wasn't linked to its trigger. The log did catch the game's own icon at the merchant:
  key Interact, text `<ICON INTERACT-F>`, at his icon spot, he its owner.
- **Run 2 (2026-10-09/10): the game's own icon showed at Ashley** and followed her, but at her feet; F6 took it
  away. Every Interact press registered on our icon (the log: checkInput true for one frame per press); nothing
  happened in game because the probe doesn't start anything yet.
- **Run 3 (2026-10-10): looks great (user); done.** Built into remod's cutscene script the same day.
- Run 3 as built: the icon on a marker kept 0.3 m above her Head joint, Ashley as its owner (not the merchant). A
  green "INTERACT PRESSED" line flashes at the top left when a press registers. Same steps; the top line says "run 3".
  Look at: the icon's height, and whether pressing Interact near the merchant while our icon shows opens his shop.

## Steps

1. In Fluffy, install `spikes\out\remod interact icon probe.zip`. Leave the trigger test mod installed or not; the
   probe uses only F5 and F6.
2. Load your usual save. White text at the top left says "remod interact icon probe: run 1".
3. Walk up to the merchant until his own icon shows. Press **F5** (it writes down his interaction).
4. Walk over near Ashley, away from the merchant. Press **F6**.
   - Does the game's icon appear at Ashley's head? What does it show (the key, any text)?
   - While it shows, press the game's Interact key (the one the icon shows). Does anything happen in the game?
5. Press **F6** again: the icon should go.
6. Quit the game.

**If the game crashes at F6:** that's an answer too (the copy is missing something the game needs). Just say so.

## Taking it out

Uninstall **remod interact icon probe** in Fluffy.

## Tell Claude

Only what you saw at step 4: whether the icon appeared at Ashley, what it showed, and what Interact did. The log and
`reframework\data\remod_interact_icon_probe.json` have the rest.
