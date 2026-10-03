# Example graphs

Ready-made graphs for Resident Evil 4 (2023). Open one in remod (`...` next to *Graph file*), click **Run**, and look at
what each block did.

**Before you start:** set **Game files** in the Pipeline panel to your REtool folder (`...\re_chunk_000\natives\stm`).
The examples find the game's files through it: their paths start with `{game}`. Then use **Save As...** to save a copy
somewhere of your own before changing one. Each writes its mod to a `mods` folder next to the graph.

| Example | Shows |
|---|---|
| `01_edit_a_texture_by_hand` | The basic mod: export a document, edit it in your image editor (**Edit image** waits for you), convert it back, package it. |
| `02_recolour_a_texture` | No hand step: **Adjust colour** changes it. Export image without a file keeps a working copy for you. |
| `03_your_picture_in_a_photo_frame` | **Replace photo** puts `picture.png` into an old framed photo, keeping the frame and the photo's ageing. Use your own picture. |
| `04_a_logo_on_a_texture` | **Overlay image** stamps `logo.png` on a document. The paper is where the texture isn't see-through. |
| `05_many_textures_at_once` | **Files in folder**: the same steps for every matching texture in a folder, packaged as one mod. |
| `06_recolour_part_of_a_character` | **Recolour part**, a block made of blocks: Leon's trousers in another colour, on the texture and its streaming copy. Right-click it, **Edit custom node**, to see inside. |
| `07_edit_a_character_texture` | **Part texture** finds the texture of Leon's shirt from his mesh; **Streaming copy** and **Convert with streaming copy** replace both copies the game has. You edit at full size. |
| `08_only_the_colour_textures` | Conditions: **Text matches** and **If** let only Leon's colour (`albd`) textures through. The others show *Not needed*. |
| `09_keep_the_alpha_data` | Channels: **Pick channel** shows the alpha (data, not transparency); **Merge channels** puts a picture in as the colour and keeps it. |
| `10_if_else_picture_or_recolour` | If / else: `my_picture.png` if it's next to the graph, else a recolour. **File exists**, **Not**, two **If**s and **First of**. |
| `11_back_up_each_build` | File steps: **Copy file** keeps a copy of each built mod in `backups`. |

The blocks are laid out when a graph opens. Move them as you like and save.
