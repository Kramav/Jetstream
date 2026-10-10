# Shop menu probe

**What it answers:** what the merchant's menu is made of, so we can plan a new entry in it (talk topics first, items
later). It is **read-only**: it changes nothing in the game and hooks nothing.

**Time:** about 5 minutes. **You need:** RE4R with REFramework, Fluffy, your usual save (the merchant nearby).

## Steps

1. In Fluffy, uninstall `remod interact icon probe` if it's still installed, and install
   `spikes\out\remod shop menu probe.zip`.
2. Load your usual save. White text at the top left says "remod shop probe: run 1 (read-only)" with a status line.
3. Talk to the merchant (Interact). On the first menu (Buy, Sell, ...), press **F5**.
4. Move the cursor over each entry of that first menu once (no need to pick them).
5. Open **Buy**. Press **F5** again. Move through each tab at the top once.
6. Leave the menu and quit the game.

## Taking it out

Uninstall **remod shop menu probe** in Fluffy.

## Tell Claude

Nothing needed unless the game crashed or the white text never showed. The log and
`reframework\data\remod_shop_probe.json` have the rest.
