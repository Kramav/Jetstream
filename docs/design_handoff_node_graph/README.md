# Handoff: Jetstream node graph redesign (option 1c, "Schematic")

## Overview
Redesign of the Graph panel in Jetstream, the RE Engine modding workflow tool. It covers both modes (Build layout and Use layout). Goals:
1. Make blocks recognizable at a glance by **family**, using outline shape.
2. Keep large graphs (25+ blocks) readable with **two zoom levels**: Far is compact symbols, Near is full blocks with fields.
3. In Use layout, show run progress **along the links** (a progress path).

The chosen direction is **1c**. Options 1a and 1b are in the same file for reference only. 1c Near reuses the 1a block geometry.

## About the design files
`Node Graph.dc.html` is a **design reference built in HTML**, not production code. Recreate it in the existing app. The screenshots suggest Dear ImGui with a node-editor library such as imnodes or imgui-node-editor. If so, draw block outlines, pins and links with `ImDrawList` (custom path per family) rather than the library's default node rectangle. Open the HTML in a browser to try every state: Build/Use switch, Far/Near zoom, click to select, minimap click.

The colour/type tokens come from the "Industry" design system (`styles.css` is included). Map them onto your ImGui style. Exact values are listed below.

## Fidelity
**High fidelity** for the node grammar: shapes, pin shapes, link styles, states and zoom behaviour. Window chrome (side panels, top bar) is **mid fidelity**. Keep your existing docking layout and use the panels as content guidance.

---

## 1. Block families (core of the redesign)
Each block type belongs to exactly one family. The family determines the outline. Dimensions are in canvas units at zoom 1. `w`/`h` = block size.

| Family | Blocks today | Outline path (local coords) | Extra marks |
|---|---|---|---|
| Source | Original texture, Use existing image | `M0 0 H w V h H 0 L 8 h/2 Z` (notch 8 into left edge) | — |
| Transform | Export image, Convert image to texture | plain rectangle | — |
| Your step (manual) | Edit image | rectangle | inner rect inset 3 (double frame); header hatched (45° lines, 5-unit spacing, 1.6 wide, accent-400) |
| Flow | Split | hexagon `M10 0 H w-10 L w h/2 L w-10 h H 10 L 0 h/2 Z`; **in Far: a 12×12 circle (junction dot)** | — |
| File | Copy / Move / Rename file | `M0 0 H w-10 L w 10 V h H 0 Z` | fold line `M w-10 0 V 10 H w` |
| Output | Package for Fluffy | rectangle | "stack" sheet behind: `M5 0 V-5 H w+5 V h-5 H w` |
| Value | Text | parallelogram `M8 0 H w L w-8 h H 0 Z` | — |

Palette glyphs (16×12, stroke 1.1, no fill) for the Blocks list and inspector:
```
source    M1 1H15V11H1L4 6Z
transform M1 1H15V11H1Z
manual    M1 1H15V11H1Z M3.5 3.5H12.5V8.5H3.5Z
flow      M4 1H12L15 6L12 11H4L1 6Z
file      M1 1H11L15 5V11H1Z M11 1V5H15
output    M1 3H13V11H1Z M3 3V1H15V9H13
value     M4 1H15L12 11H1Z
```
Every block also gets `+` registration marks at its 4 bbox corners: 7-unit cross offset 5 out, neutral-400. Selected: 10-unit cross offset 8, accent.

Pins sit **on** the outline edge. On slanted or hex edges, compute x from y:
- flow: `xL = 10·|y−h/2|/(h/2)`, `xR = w − xL`
- value: `xL = 8·(1 − y/h)`, `xR = w − 8·y/h`
- others: `xL = 0`, `xR = w`

## 2. Pin shapes = data type
| Type | Shape | Size |
|---|---|---|
| texture | square | 7×7 |
| image | circle | r 4 |
| text / path | diamond | 9×9 |
| "+ add" (variadic slot) | dashed hollow square, neutral-500 | 7×7 |
| field pin (Near only) | small hollow circle, neutral-500 | r 2.75 |

Wired pins are filled with accent-700. Unwired pins are filled with the bg colour and stroked in accent-700. On "not reached" blocks, pins use neutral-400.

## 3. Zoom levels (1c)
A toolbar segmented control, **Zoom: Far · chips | Near · fields**. In the real app, also switch automatically at a zoom threshold (suggestion: below ~0.6 → Far).

### Far (schematic)
- Block = family shape, **116×40** (height grows to `(max(ins,outs)+1)·9` if needed). Title centred inside: Barlow Condensed 600, 13px, line-height 1.05, padding 0 12.
- Key value below the shape, mono 9.5px, neutral-700, centred, margin-top 4. Examples: `ui3200_albd.tex`, `CleanHUD.zip`, `"Clean HUD"`.
- Pins spread evenly down the left/right edges: `y = h·(i+1)/(n+1)`. No pin labels. Ports show in the inspector.
- Split/Flow = 12×12 junction circle. All links meet at its centre. Selected: dashed ring r 10.
- Grid: column pitch 168, row pitch 74. Fit-to-view (scale ≤ 1).

### Near (full blocks)
- Same family outlines, block width **204** (flow 92). Layout from top:
  - header **28**: title Barlow Condensed 600 15px; status text right-aligned (Use layout)
  - divider line
  - port rows **18** each: input label left, output label right, 11px neutral-700; pins on these rows
  - 4 spacing
  - field rows **24** each: label column 64px (11px neutral-700, `*` if required), value with a 1px neutral-400 underline (mono 10.5px). Each field row has a field pin on the left edge.
  - 8 bottom padding
- **Wired fields become ports.** A field linked from another block is removed from the field list and only appears as a port.
- Scale 1, **centred on the selected block**. Selecting another block re-centres the view.
- **Minimap** bottom-right, 18px inset: 220 wide (height proportional), bg colour, 1px neutral-400 frame with corner marks, caption "OVERVIEW · CLICK TO JUMP" (mono 9.5px, letter-spacing .08em). Blocks are drawn as their family shapes in neutral-300, the selected one in accent. The viewport is a rect stroked in accent-800, 1.25. Clicking a block selects it and jumps to it.
- Double-clicking a block flips just that block between chip and full (1a/1b behaviour, optional in 1c).

## 4. Links
- **Far/Near in 1c: orthogonal** `H mx V y2 H x2`. Lane rule:
  - Adjacent column: `mx = colRight + gap/2 + (inIdx − (nIn−1)/2)·5`
  - Spanning 2+ columns: `mx = colRight + 10 + inIdx·5` (turn right after the source, then run horizontally at the target's height)
- Build layout: accent, 1.3 wide. Links attached to the selected block: accent-800, 2.
- Draw order: pending → failed → done → active (on top).

## 5. Use layout: progress path
Node state comes from the last run: done / waiting (your step) / failed / not reached. Link state is derived from its **source** block:

| Link state | Rule | Stroke |
|---|---|---|
| done | source done, target not waiting | accent, 2.25, solid |
| active | source done, target waiting | accent-900, 2.25, dash 7 5, **animated** dash offset −24 per 0.9s, linear, infinite |
| failed | source failed | text colour, 1.5, dash 2 3 |
| pending | source not done | neutral-400, 1.25, dash 3 3 |

Block treatment (Far, 1c):
- done: fill accent-200, title accent-900, outline accent-600 1.1
- waiting: fill accent-900, title bg colour, outline accent-900 2
- failed: fill text colour, title bg colour, outline 2. The key line changes to the reason ("preview.png not found").
- not reached: bg fill, outline neutral-400 dashed 4 3, whole block 55% opacity

In Near the same colours fill only the **header band** (done = accent-100), with status text DONE / YOUR STEP / FAILED in mono 9px 600, letter-spacing .06em.

A legend in the graph toolbar shows the four link styles: Done, Waiting for you, Failed, Not reached.

Right panel (Use):
- "YOUR STEP · n" card: 1.5px accent-900 frame with corner marks, title, instruction copy, buttons **Open in editor** (secondary) and **Done editing** (primary).
- Numbered step list. Each row has a 9×9 state square: done = accent, waiting = accent-900, failed = text, not reached = dashed. The label is right-aligned. Clicking a row selects that block.
- A failure note under the list.

## 6. Build layout panels
- Left **Blocks** palette: search, then groups by family (mono 10.5px uppercase heading), each item = glyph + name. Hover fill accent-100. Drag to canvas.
- Right **Inspector** for the selected block: family kicker (glyph + mono 10.5px uppercase accent-700), title Barlow Condensed 600 26px, description 12.5px neutral-700, fields (label + underlined value, `…` picker for paths; wired fields show `← Source block · port` in accent-700), then IN / OUT port lists with types.
- Top bar: wordmark, graph file, game selector, **Build layout | Use layout** segmented control (active = accent fill, bg text), Load/Save (Build) or Run again (primary, Use).

## 7. State
```
mode: 'build' | 'use'
zoom: 'far' | 'near'           // also auto from canvas zoom
selectedBlockId
perBlockCollapsed: {id: bool}  // optional, flips one block between chip/full
run: { [blockId]: 'done'|'waiting'|'failed'|'pending', failReason? }
```
Link state is derived, never stored.

## 8. Design tokens
Colours:
- bg `#f2f2f3`, surface `#e9e9ea`, text `#1d1f20`, divider = text at 16% alpha
- accent `#5980a6`; ramp 100 `#eef6ff`, 200 `#d6ebff`, 300 `#b5d9fd`, 400 `#94bce3`, 500 `#749dc4`, 600 `#597ea3`, 700 `#416180`, 800 `#2c455d`, 900 `#1d2d3d`
- neutral ramp 100 `#f5f5f8`, 200 `#e7e7ea`, 300 `#d4d4d7`, 400 `#b7b7ba`, 500 `#98989b`, 600 `#7a7a7d`, 700 `#5d5d60`, 800 `#424244`, 900 `#2b2b2d`
- Canvas grid: 24px squares, 1px neutral-200 lines on bg.

Type:
- Headings/titles: Barlow Condensed 600
- Body: Barlow
- Values/paths/status: ui-monospace (Menlo / Consolas)

Shape rules:
- Square corners everywhere (radius 0). 1px hairline frames with `+` corner marks.
- Mono accent only. State is shown by fill/line style, not by new hues.
- Icons: Lucide, stroke 1.5.

## Assets
None. All shapes are vector paths listed above. The texture/3D thumbnails in the mock are placeholders for your real previews.

## Files
- `Node Graph.dc.html`: the interactive reference (options 1a/1b/1c; 1c is the chosen one). Needs `support.js` next to it and the `_ds/…/styles.css` path to render.
- `styles.css`: Industry design-system tokens.
- Logic worth reading in the HTML: `geo()` (block sizes and pin positions per zoom), `shapeD()` (family outlines), `buildSvg()` (links, states, lane routing), `opt()` (zoom, centring, minimap).
