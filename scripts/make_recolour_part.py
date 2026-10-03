"""Writes blocks/recolour_part.json: a built-in block made of blocks, with the Convert with streaming copy it uses
copied in (custom_nodes), as every graph keeps copies of the custom nodes it uses. Re-run after changing
blocks/convert_with_streaming.json (a test checks the copy matches)."""
import json

ROOT = __import__("os").path.join(__import__("os").path.dirname(__file__), "..", "blocks") + "/"
convert = json.load(open(ROOT + 'convert_with_streaming.json', encoding='utf-8'))

nodes, links = [], []


def node(i, t, params, x, y):
    nodes.append({"id": i, "type": t, "params": params, "pos": [x, y]})


def link(a, ap, b, bp):
    links.append({"from": [a, ap], "to": [b, bp]})


# Pins, top to bottom.
node(1, "NodeInput", {"name": "mesh", "default": ""}, 0, 0)
node(2, "NodeInput", {"name": "materials", "default": ""}, 0, 100)
node(3, "NodeInput", {"name": "hue", "default": "0"}, 0, 200)
node(4, "NodeInput", {"name": "saturation", "default": "0"}, 0, 300)
node(5, "NodeInput", {"name": "brightness", "default": "0"}, 0, 400)
node(6, "NodeInput", {"name": "contrast", "default": "0"}, 0, 500)
node(7, "Split", {}, 250, 0)
node(8, "Split", {}, 250, 100)
node(9, "PartTexture", {"mesh": "", "materials": "", "texture": ""}, 400, 0)
node(10, "Split", {}, 700, 0)
node(11, "StreamingCopy", {}, 850, 0)
node(12, "MeshMask", {"mesh": "", "size_of": "", "materials": "", "grow": "2", "save_to": "",
                      "title": "Where the parts are"}, 1450, 400)
node(13, "ExportImage", {"png": ""}, 1150, 0)
node(14, "Split", {}, 1400, 0)
node(15, "AdjustColour", {"image": "", "hue": "0", "saturation": "0", "brightness": "0", "contrast": "0",
                          "save_to": ""}, 1550, 150)
node(16, convert["type"], {}, 2300, 0)
node(17, "MaskBlend", {"base": "", "edited": "", "mask": "", "feather": "0", "invert": "false", "save_to": "",
                       "title": "Only on the parts"}, 1900, 0)
node(18, "Split", {}, 2150, 0)
node(19, "NodeOutput", {"name": "texture"}, 2700, 0)
node(20, "NodeOutput", {"name": "streaming texture"}, 2700, 100)
node(21, "NodeOutput", {"name": "preview"}, 2700, 200)

link(1, "value", 7, "in"); link(7, "out", 9, "mesh"); link(7, "out", 12, "mesh")
link(2, "value", 8, "in"); link(8, "out", 9, "materials"); link(8, "out", 12, "materials")
link(9, "tex", 10, "in"); link(10, "out", 11, "tex"); link(10, "out", 16, "in2")
link(11, "full", 13, "tex"); link(13, "png", 14, "in")
link(14, "out", 17, "base"); link(14, "out", 15, "image"); link(14, "out", 12, "size_of")
link(3, "value", 15, "hue"); link(4, "value", 15, "saturation"); link(5, "value", 15, "brightness")
link(6, "value", 15, "contrast")
link(15, "image", 17, "edited"); link(12, "image", 17, "mask")
link(17, "image", 18, "in"); link(18, "out", 16, "in1"); link(18, "out", 21, "value")
link(11, "streaming", 16, "in3")
link(16, "out9", 19, "value"); link(16, "out10", 20, "value")

block = {
    "type": "custom:recolour_part",
    "title": "Recolour part",
    "summary": "Changes the colour of some parts of a mesh (e.g. *Pants* on Leon) on the texture they use and on its "
               "streaming copy, leaving the rest of the texture and its alpha data as they are. Give the mesh, the "
               "parts' material names and the colour change; link both textures into Package. Open it (Edit custom "
               "node) to see the blocks it's made of.",
    "graph": {"schema_version": 0, "profile": "re4r", "nodes": nodes, "links": links, "custom_nodes": [convert]},
}
json.dump(block, open(ROOT + 'recolour_part.json', 'w', encoding='utf-8', newline='\n'), indent=2)
print('wrote recolour_part.json')
