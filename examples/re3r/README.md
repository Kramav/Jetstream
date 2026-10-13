# Resident Evil 3 (2020) examples

Ready-made graphs for RE3R (the ray-tracing version, Steam's default). Set your **Game files** folder in the
Pipeline panel to RE3R's extracted `natives\STM`, open a graph (`...` next to *Graph file*), and click **Run**. Built
mods go to `mods\`; install them with Fluffy.

RE3R's documents and photos also have a high-resolution streaming copy, so these graphs replace both (**Streaming
copy** and **Convert with streaming copy**). Textures are all remod supports for RE3R so far: no meshes, sounds,
movies or cutscenes yet. Checked by running each on RE3R's files and looking at the results; not tried in game yet.

| Example | What it shows |
|---|---|
| `01_edit_a_texture_by_hand` | An R.P.D. paper (`ui3210_file_03`) exported to `edits\document.png`, edited in your image editor, and put back. |
| `02_recolour_a_texture` | The same paper in another colour, with no hand step. |
| `03_your_picture_in_a_photo_frame` | `picture.png` in place of a printed photo (`ui3210_file_58`), keeping its frame and ageing. Stains are off: on a colour photo they'd bring the old picture back. |
| `04_a_logo_on_a_texture` | `logo.png` stamped on the paper. |
| `05_many_textures_at_once` | Three of the files, faded, through one chain (**Files in folder**). |
| `07_edit_a_character_texture` | Jill's outfit (`pl2000_body_albm`), edited by hand at full size; both copies replaced. |
| `08_only_the_colour_textures` | Only the colour textures (`*_alb*`) of Jill's folder, made grey. |
| `09_keep_the_alpha_data` | A picture as Jill's outfit's colour, its alpha (data, not transparency) kept. |
| `10_if_else_picture_or_recolour` | `my_picture.png` on the paper if it's there, else a recolour. |
| `11_back_up_each_build` | A darker paper, with a copy of each build in `backups\`. |
