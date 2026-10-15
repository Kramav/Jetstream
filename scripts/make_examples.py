"""Writes the example graphs, a folder per game in examples/ (shipped in the release zip), and their sample files.

The same examples for every game whose files remod has been pointed at, each with that game's own files and only what
remod can do for it (CLAUDE.md §10 "Games"): examples 1-11 work on textures; RE4R's (meshes, movies, sounds,
cutscenes) also have 12-22. A game with no extracted files to pick real ones from (RE7, RE8, RE9 so far) has none.

Each graph is written without block positions. Afterwards, always tidy up and save every example with the app's
real block sizes: build/app/remod-app.exe --bake examples/<game>/<each>.json (one path per graph). Paths to
game files start with {game}, the Game files folder set in the app for that game, so the examples run on any PC.
Fields left out get their defaults when the graph loads. Run: python scripts/make_examples.py
"""
import json
import os
import shutil
import struct
import zlib

EXAMPLES = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'examples')
OUT = os.path.join(EXAMPLES, 're4r')  # the folder Graph.save writes to: the game being written
UI = '{game}/_chainsaw/ui/ui3200/tex/'
LEON = '{game}/_chainsaw/character/ch/cha0/cha000/00/cha000_00.mesh.221108797'

# Each game's files for the texture examples, found by looking at them (contact sheets of the decoded textures,
# 2026-10-13; RE4R's earlier). doc: a document with a see-through background; logo: where on it a logo goes (px);
# photo: a framed photo (Replace photo; photo_stains 0 for a colour photo, whose colour Stains would carry over);
# docs: a folder and pattern of a few documents; character: a colour texture
# with a streaming copy, and the pattern of its folder's colour textures. streaming: the documents and photo have a
# streaming copy too (RE3R's: same size), so the examples replace both (CLAUDE.md §9). RE4R reaches its character through Leon's mesh
# (meshes are supported there); the others by the texture itself.
RE3 = '{game}/escape/ui/ui3200/tex/'
RE2 = '{game}/sectionroot/ui/ui3200/tex/'
GAMES = {
    're4r': dict(doc=UI + 'cs_ui3210_file_008_00_iam.tex.143221013', doc_name='document', logo=(330, 740),
                 doc_edit='edits/file_008.png',
                 photo=UI + 'cs_ui3210_file_039_00_iam.tex.143221013', photo_y=-30,
                 docs=(UI[:-1], 'cs_ui3210_file_00*_iam.tex.*', 'The first documents of the files'),
                 who='Leon', part='shirt', colour='*_albd*'),
    're3r': dict(doc=RE3 + 'ui3210_file_03_iam.tex.34', doc_name='R.P.D. paper', logo=(400, 560),
                 photo=RE3 + 'ui3210_file_58_iam.tex.34', photo_y=0, photo_stains=0,
                 docs=(RE3[:-1], 'ui3210_file_32_iam.tex.*;ui3210_file_33_iam.tex.*;ui3210_file_34_iam.tex.*',
                       'Three of the files'),
                 who='Jill', part='outfit', colour='*_alb*', streaming=True,
                 character='{game}/escape/character/player/pl2000/mesh/pl2000_body_albm.tex.34'),
    're2r': dict(doc=RE2 + 'ui3210_file_17_iam.tex.10', doc_name='R.P.D. paper', logo=(400, 560),
                 photo=RE2 + 'ui3210_file_57_iam.tex.10', photo_y=0, photo_stains=0,
                 docs=(RE2[:-1], 'ui3210_file_6?_iam.tex.*', 'The files numbered 60 to 69'),
                 who='Leon', part='uniform', colour='*_alb*',
                 character='{game}/sectionroot/character/player/pl0000/pl0000/pl0000_body_albm.tex.10'),
}
game = 're4r'


class Graph:
    def __init__(self):
        self.nodes, self.links = [], []

    def add(self, type_, title=None, **params):
        if title:
            params['title'] = title
        self.nodes.append({'id': len(self.nodes) + 1, 'type': type_, 'params': {k: str(v) for k, v in params.items()}})
        return len(self.nodes)

    def link(self, a, a_port, b, b_port):
        self.links.append({'from': [a, a_port], 'to': [b, b_port]})

    def save(self, name):
        with open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n') as f:
            json.dump({'schema_version': 0, 'profile': game, 'nodes': self.nodes, 'links': self.links}, f, indent=2)
        print('wrote', game + '/' + name)


def package(g, name, description):
    return g.add('PackageMod', name=name, out='mods', version='1.0', description=description, replace='true')


def convert_and_package(g, image, texture, name, description, port='image'):
    """image -> Split -> Convert (original: texture) and Package's preview."""
    split = g.add('Split')
    g.link(image, port, split, 'in')
    convert = g.add('SaveTex')
    g.link(split, 'out', convert, 'image')
    g.link(texture, 'out', convert, 'original')
    pkg = package(g, name, description)
    g.link(convert, 'tex', pkg, 'tex')
    g.link(split, 'out', pkg, 'preview')
    return pkg


def document(g, G, path, title):
    """A texture to edit, from its file: (node, port) to export it from, and finish(image, name, description) that
    converts the edit back and packages it, both copies where the game keeps a streaming copy."""
    tex = g.add('LoadTex', title, tex=path)
    split = g.add('Split')
    g.link(tex, 'tex', split, 'in')
    if not G.get('streaming'):
        return (split, 'out'), lambda image, name, desc, port='image': convert_and_package(g, image, split, name, desc, port)
    stream = g.add('StreamingCopy')
    g.link(split, 'out', stream, 'tex')

    def finish(image, name, description, port='image'):
        isplit = g.add('Split')
        g.link(image, port, isplit, 'in')
        convert = g.add('custom:convert_with_streaming')
        g.link(isplit, 'out', convert, 'in1')
        g.link(split, 'out', convert, 'in2')
        g.link(stream, 'streaming', convert, 'in3')
        pkg = package(g, name, description)
        g.link(convert, 'out9', pkg, 'tex')
        g.link(convert, 'out10', pkg, 'tex')
        g.link(isplit, 'out', pkg, 'preview')
        return pkg
    return (stream, 'full'), finish


def character_texture(g, G):
    """The character's colour texture: through its mesh's part where meshes are supported (RE4R), else the file."""
    if 'character' in G:
        return g.add('LoadTex', "%s's %s" % (G['who'], G['part']), tex=G['character'])
    return g.add('PartTexture', "Leon's shirt", mesh=LEON, materials='Shirts_Mat')


def texture_examples(G):
    doc = G['doc_name']
    # 1. The classic: export, edit by hand, convert, package.
    g = Graph()
    (src, port), finish = document(g, G, G['doc'], 'The texture to change')
    export = g.add('ExportImage', png=G.get('doc_edit', 'edits/document.png'))
    edit = g.add('EditImage', 'Your edit')
    g.link(src, port, export, 'tex')
    g.link(export, 'png', edit, 'png')
    finish(edit, 'My first texture mod', 'A %s, edited by hand' % doc)
    g.save('01_edit_a_texture_by_hand.json')

    # 2. No hand step: a colour change.
    g = Graph()
    (src, port), finish = document(g, G, G['doc'], 'The texture to change')
    export = g.add('ExportImage')  # no file: a working copy
    adjust = g.add('AdjustColour', hue=150, saturation=20)
    g.link(src, port, export, 'tex')
    g.link(export, 'png', adjust, 'image')
    finish(adjust, 'Recoloured %s' % doc, 'A %s in another colour' % doc)
    g.save('02_recolour_a_texture.json')

    # 3. Your picture in an old photo frame.
    g = Graph()
    (src, port), finish = document(g, G, G['photo'], 'The framed photo')
    export = g.add('ExportImage')
    picture = g.add('ImportImage', 'Your picture (change me)', png='picture.png')
    photo = g.add('ReplacePhoto', picture_y=G['photo_y'],
                  **({'stains': G['photo_stains']} if 'photo_stains' in G else {}))
    g.link(src, port, export, 'tex')
    g.link(export, 'png', photo, 'frame')
    g.link(picture, 'image', photo, 'picture')
    finish(photo, 'My photo', 'My picture in place of an old photo')
    g.save('03_your_picture_in_a_photo_frame.json')

    # 4. A logo on a texture.
    g = Graph()
    (src, port), finish = document(g, G, G['doc'], 'The texture to change')
    export = g.add('ExportImage')
    logo = g.add('ImportImage', 'Your logo (change me)', png='logo.png')
    overlay = g.add('OverlayImage', x=G['logo'][0], y=G['logo'][1], opacity=85)  # on the paper: around it see-through
    g.link(src, port, export, 'tex')
    g.link(export, 'png', overlay, 'base')
    g.link(logo, 'image', overlay, 'top')
    finish(overlay, 'Stamped %s' % doc, 'A logo stamped on a %s' % doc)
    g.save('04_a_logo_on_a_texture.json')

    # 5. Many textures at once: every matching file in a folder, through the same steps.
    g = Graph()
    folder, pattern, which = G['docs']
    files = g.add('FilesInFolder', 'The documents', folder=folder, pattern=pattern)
    tex = g.add('LoadTex')
    split = g.add('Split')
    export = g.add('ExportImage')
    adjust = g.add('AdjustColour', hue=-40, saturation=-30)
    pkg = package(g, 'Faded documents', which + ', faded')
    g.link(files, 'files', tex, 'tex')
    g.link(tex, 'tex', split, 'in')
    g.link(export, 'png', adjust, 'image')
    if G.get('streaming'):  # each one's streaming copy too
        stream = g.add('StreamingCopy')
        convert = g.add('custom:convert_with_streaming')
        g.link(split, 'out', stream, 'tex')
        g.link(split, 'out', convert, 'in2')
        g.link(stream, 'full', export, 'tex')
        g.link(adjust, 'image', convert, 'in1')
        g.link(stream, 'streaming', convert, 'in3')
        g.link(convert, 'out9', pkg, 'tex')
        g.link(convert, 'out10', pkg, 'tex')
    else:
        convert = g.add('SaveTex')
        g.link(split, 'out', export, 'tex')
        g.link(split, 'out', convert, 'original')
        g.link(adjust, 'image', convert, 'image')
        g.link(convert, 'tex', pkg, 'tex')
    g.save('05_many_textures_at_once.json')

    # 6. Recolour one part of a character: a block made of blocks (reads the mesh: RE4R only).
    if game == 're4r':
        g = Graph()
        recolour = g.add('custom:recolour_part', "Leon's trousers", in1=LEON, in2='Pants_Mat', in3=120, in4=10, in5=5)
        pkg = package(g, 'Green trousers', "Leon's trousers in another colour")
        g.link(recolour, 'out19', pkg, 'tex')
        g.link(recolour, 'out20', pkg, 'tex')
        g.link(recolour, 'out21', pkg, 'preview')
        g.save('06_recolour_part_of_a_character.json')

    who, part = G['who'], G['part']
    # 7. Edit a character texture by hand, at full size, replacing its streaming copy too.
    g = Graph()
    texture = character_texture(g, G)
    split = g.add('Split')
    stream = g.add('StreamingCopy')
    export = g.add('ExportImage', png='edits/%s_%s.png' % (who.lower(), part))
    edit = g.add('EditImage', 'Your edit')
    esplit = g.add('Split')
    convert = g.add('custom:convert_with_streaming')
    pkg = package(g, 'My %s' % part, "%s's %s, edited by hand" % (who, part))
    g.link(texture, 'tex', split, 'in')
    g.link(split, 'out', stream, 'tex')
    g.link(split, 'out', convert, 'in2')
    g.link(stream, 'full', export, 'tex')
    g.link(export, 'png', edit, 'png')
    g.link(edit, 'image', esplit, 'in')
    g.link(esplit, 'out', convert, 'in1')
    g.link(esplit, 'out', pkg, 'preview')
    g.link(stream, 'streaming', convert, 'in3')
    g.link(convert, 'out9', pkg, 'tex')
    g.link(convert, 'out10', pkg, 'tex')
    g.save('07_edit_a_character_texture.json')

    # 8. Conditions as a filter: only the colour textures of a folder go on.
    g = Graph()
    if 'character' in G:
        folder, pattern = G['character'].rsplit('/', 1)[0], G['character'].rsplit('/', 1)[1].split('_body')[0] + '_*.tex*'
    else:
        folder, pattern = LEON.rsplit('/', 1)[0], 'cha000_00_*.tex*'
    files = g.add('FilesInFolder', "%s's textures" % who, folder=folder, pattern=pattern)
    tex = g.add('LoadTex')
    split = g.add('Split')
    match = g.add('TextMatches', 'Is it a colour texture?', pattern=G['colour'])
    gate = g.add('If', 'Only colour textures')
    gsplit = g.add('Split')
    stream = g.add('StreamingCopy')
    export = g.add('ExportImage')
    adjust = g.add('AdjustColour', 'Grey', saturation=-100)
    convert = g.add('custom:convert_with_streaming')
    pkg = package(g, 'Grey %s' % who, "%s's colour textures without colour" % who)
    g.link(files, 'files', tex, 'tex')
    g.link(tex, 'tex', split, 'in')
    g.link(split, 'out', gate, 'value')
    g.link(split, 'out', match, 'text')
    g.link(match, 'yes', gate, 'condition')
    g.link(gate, 'value', gsplit, 'in')
    g.link(gsplit, 'out', stream, 'tex')
    g.link(gsplit, 'out', convert, 'in2')
    g.link(stream, 'full', export, 'tex')
    g.link(export, 'png', adjust, 'image')
    g.link(adjust, 'image', convert, 'in1')
    g.link(stream, 'streaming', convert, 'in3')
    g.link(convert, 'out9', pkg, 'tex')
    g.link(convert, 'out10', pkg, 'tex')
    g.save('08_only_the_colour_textures.json')

    # 9. Channels: a texture's alpha holds data; a new picture goes in as its colour only.
    g = Graph()
    texture = character_texture(g, G)
    split = g.add('Split')
    stream = g.add('StreamingCopy')
    export = g.add('ExportImage')
    esplit = g.add('Split')
    alpha = g.add('PickChannel', 'Its alpha: data, not transparency', channel='alpha')
    look = g.add('Preview')
    picture = g.add('ImportImage', 'Your picture (change me)', png='picture.png')
    fit = g.add('ResizeImage', fit='fill')
    merge = g.add('MergeChannels', 'Picture as colour, alpha kept')
    convert = g.add('custom:convert_with_streaming')
    pkg = package(g, 'Picture %s' % part, "A picture as %s's %s texture, keeping its alpha data" % (who, part))
    g.link(texture, 'tex', split, 'in')
    g.link(split, 'out', stream, 'tex')
    g.link(split, 'out', convert, 'in2')
    g.link(stream, 'full', export, 'tex')
    g.link(export, 'png', esplit, 'in')
    g.link(esplit, 'out', merge, 'base')
    g.link(esplit, 'out', alpha, 'image')
    g.link(esplit, 'out', fit, 'match')
    g.link(alpha, 'image', look, 'in')
    g.link(picture, 'image', fit, 'image')
    g.link(fit, 'image', merge, 'colour')
    g.link(merge, 'image', convert, 'in1')
    g.link(stream, 'streaming', convert, 'in3')
    g.link(convert, 'out9', pkg, 'tex')
    g.link(convert, 'out10', pkg, 'tex')
    g.save('09_keep_the_alpha_data.json')

    # 10. If / else: your picture if it's there, else a recolour.
    g = Graph()
    (src, port), finish = document(g, G, G['doc'], 'The texture to change')
    split = src  # used twice: the Split itself, else one after the full-size copy
    if port != 'out':
        split = g.add('Split')
        g.link(src, port, split, 'in')
    exists = g.add('FileExists', 'Is my picture there?', path='my_picture.png')
    csplit = g.add('Split')
    yes = g.add('If', 'Then: my picture')
    no = g.add('Not')
    other = g.add('If', 'Else: recolour')
    path = g.add('Value', 'My picture', value='my_picture.png')
    picture = g.add('ImportImage')
    fit = g.add('ResizeImage', fit='fill')
    export = g.add('ExportImage')
    adjust = g.add('AdjustColour', hue=90)
    first = g.add('FirstOf')
    g.link(exists, 'yes', csplit, 'in')
    g.link(csplit, 'out', yes, 'condition')
    g.link(csplit, 'out', no, 'in')
    g.link(no, 'yes', other, 'condition')
    g.link(path, 'value', yes, 'value')
    g.link(yes, 'value', picture, 'png')
    g.link(picture, 'image', fit, 'image')
    g.link(split, 'out', fit, 'match')
    g.link(split, 'out', other, 'value')
    g.link(other, 'value', export, 'tex')
    g.link(export, 'png', adjust, 'image')
    g.link(fit, 'image', first, 'first')
    g.link(adjust, 'image', first, 'second')
    finish(first, 'Picture or colour', 'my_picture.png if it is there, else a recoloured %s' % doc, 'value')
    g.save('10_if_else_picture_or_recolour.json')

    # 11. File steps: keep a copy of each build.
    g = Graph()
    (src, port), finish = document(g, G, G['doc'], 'The texture to change')
    export = g.add('ExportImage')
    adjust = g.add('AdjustColour', brightness=-25)
    pkg = finish(adjust, 'Darker %s' % doc, 'A darker %s, with a backup of each build' % doc)
    g.link(src, port, export, 'tex')
    g.link(export, 'png', adjust, 'image')
    copy = g.add('CopyFile', 'Back up the build', dest='backups/', if_exists='overwrite', create_dirs='true')
    g.link(pkg, 'mod', copy, 'source')
    g.save('11_back_up_each_build.json')


for game in ['re2r', 're3r', 're4r']:  # RE4R last: everything after this is its own
    OUT = os.path.join(EXAMPLES, game)
    os.makedirs(OUT, exist_ok=True)
    texture_examples(GAMES[game])


# ---- RE4R only from here (meshes, movies, sounds, cutscenes) ----

# 12. Movies: a test card in place of a game movie (both its copies) and its sound (a beep each second in the music,
# dialogue and effects silent), to see where and how the game plays it.
g = Graph()
movie = g.add('ReplaceMovie', 'The movie to replace',
              movie='{game}/streaming/_chainsaw/movie/mv/mva000/mva000.mov.1.x64', video='')
pkg = package(g, 'Movie test', 'mva000 replaced by a test card showing its name and the seconds, beeping each second')
g.link(movie, 'movie', pkg, 'file')
g.link(movie, 'fhd', pkg, 'file')
g.link(movie, 'sound', pkg, 'file')
g.save('12_replace_a_movie.json')

# 13. A movie edited by hand: Export movie copies it out, Edit video waits for your edit, Replace movie encodes it.
g = Graph()
which = g.add('Value', 'The movie', value='{game}/streaming/_chainsaw/movie/mv/mva402/mva402.mov.1.x64')
split = g.add('Split')
export = g.add('ExportMovie', video='edits/mva402.mp4')
edit = g.add('EditVideo')
movie = g.add('ReplaceMovie', same_length='true')
pkg = package(g, 'Edited movie', 'mva402 edited by hand')
g.link(which, 'value', split, 'in')
g.link(split, 'out', export, 'movie')
g.link(split, 'out', movie, 'movie')
g.link(export, 'video', edit, 'video')
g.link(edit, 'video', movie, 'video')
g.link(movie, 'movie', pkg, 'file')
g.link(movie, 'fhd', pkg, 'file')
g.link(movie, 'sound', pkg, 'file')  # mva402 has no sound packages: nothing comes, but the link is the pattern
g.save('13_edit_a_movie_by_hand.json')

# 14. Game sounds: a Game sound block (a sound dragged from a bank's list in the Browser) with your audio in its place.
# This one: the intro's English narration, replaced by beeps.
g = Graph()
sound = g.add('GameSound', 'The intro narration (English)',
              sound='{game}/_chainsaw/sound/wwise/ch_mva000_dialogue.sbnk.1.x64.en#880852580', audio='beep.wav')
replace = g.add('ReplaceSounds')
pkg = package(g, 'Sound test', 'The intro narration (English) replaced by beeps')
g.link(sound, 'sound', replace, 'sounds')
g.link(replace, 'files', pkg, 'file')
g.save('14_replace_a_sound.json')

# 15. A game movie at a moment you choose: a cutscene file (cutscenes/play_a_movie.json, below) holding a fade to black, the
# intro movie (mva000) and a fade back. F9 in game: the HUD goes, Leon is held, the world pauses while it plays, and
# play resumes where it was.
g = Graph()
cutscene = g.add('Cutscene', 'Fade out, the intro movie, fade in', cutscene='cutscenes/play_a_movie.json')
pkg = package(g, 'Movie moment', 'Press F9 in game: the intro movie plays, then play resumes where it was')
g.link(cutscene, 'files', pkg, 'file')
g.save('15_play_a_game_movie.json')
os.makedirs(os.path.join(OUT, 'cutscenes'), exist_ok=True)  # not beside the graphs: every .json there is one
with open(os.path.join(OUT, 'cutscenes', 'play_a_movie.json'), 'w', encoding='utf-8', newline='\n') as f:
    json.dump({'schema_version': 0, 'name': 'Play the intro movie', 'length': 1.0, 'start': {'key': 'F9'},
               'fades': [{'t': 0.0, 'until': 0.5, 'from': 0.0, 'to': 1.0},
                         {'t': 0.5, 'until': 1.0, 'from': 1.0, 'to': 0.0}],
               'movies': [{'t': 0.5, 'id': 'mva000'}]}, f, indent=2)
print('wrote cutscenes/play_a_movie.json')

# 16. A movie of your own, added to the game (none of its movies replaced): New movie makes it (here a test card;
# put your video in Your video), and a cutscene plays it by its name when you press F8.
g = Graph()
movie = g.add('NewMovie', 'Your new movie', name='rmd001', video='')
cutscene = g.add('Cutscene', 'Fade out, your movie, fade in', cutscene='cutscenes/play_new_movie.json')
pkg = package(g, 'New movie', 'Press F8 in game: your own movie plays, then play resumes where it was')
g.link(movie, 'files', pkg, 'file')
g.link(cutscene, 'files', pkg, 'file')
g.save('16_insert_a_new_movie.json')
with open(os.path.join(OUT, 'cutscenes', 'play_new_movie.json'), 'w', encoding='utf-8', newline='\n') as f:
    json.dump({'schema_version': 0, 'name': 'Play my new movie', 'length': 1.0, 'start': {'key': 'F8'},
               'fades': [{'t': 0.0, 'until': 0.5, 'from': 0.0, 'to': 1.0},
                         {'t': 0.5, 'until': 1.0, 'from': 1.0, 'to': 0.0}],
               'movies': [{'t': 0.5, 'id': 'rmd001'}]}, f, indent=2)
print('wrote cutscenes/play_new_movie.json')

# 17. A sound of your own, added to the game (none of its sounds replaced): New sound makes it from beep.wav, and a
# cutscene plays it by its name when you press F7.
g = Graph()
sound = g.add('NewSound', 'Your new sound', name='beeps', audio='beep.wav')
cutscene = g.add('Cutscene', 'Play your sound', cutscene='cutscenes/play_new_sound.json')
pkg = package(g, 'New sound', 'Press F7 in game: your own sound plays')
g.link(sound, 'files', pkg, 'file')
g.link(cutscene, 'files', pkg, 'file')
g.save('17_add_a_new_sound.json')
with open(os.path.join(OUT, 'cutscenes', 'play_new_sound.json'), 'w', encoding='utf-8', newline='\n') as f:
    json.dump({'schema_version': 0, 'name': 'Play my new sound', 'length': 2.0, 'start': {'key': 'F7'},
               'sounds': [{'t': 0.0, 'id': 'beeps'}]}, f, indent=2)
print('wrote cutscenes/play_new_sound.json')

# 18. A cutscene that starts by itself: its trigger is a spot (3 m around it). The spot is a real one, recorded in game
# (the cutscene probe's camera, 2026-10-07, in chapter 1's village), not invented; make your own with REFramework's
# menu > remod cutscenes > Make a trigger here, then the Cutscene block's Use trigger.
g = Graph()
cutscene = g.add('Cutscene', 'Starts when you get there', cutscene='cutscenes/started_by_itself.json')
pkg = package(g, 'Trigger test', 'Walk to the spot: a cutscene starts by itself')
g.link(cutscene, 'files', pkg, 'file')
g.save('18_start_a_cutscene_by_itself.json')
with open(os.path.join(OUT, 'cutscenes', 'started_by_itself.json'), 'w', encoding='utf-8', newline='\n') as f:
    json.dump({'schema_version': 0, 'name': 'Started by itself', 'length': 4.0, 'letterbox': 0.12,
               'trigger': {'near': {'position': [186.647, 28.059, 42.236], 'radius': 3.0}},
               'subtitles': [{'t': 0.5, 'until': 3.5, 'text': 'remod: this cutscene started by itself'}],
               'fades': [{'t': 3.5, 'until': 4.0, 'from': 0.0, 'to': 1.0}]}, f, indent=2)
print('wrote cutscenes/started_by_itself.json')

# 19. Other characters in a cutscene: Luis (remod ships his puppet definition), and a NEW character, rmc002: Ashley's
# body as new files in purple, made by New character (nothing of the game's replaced). F6 in game: both stand in front
# of Leon, the real Ashley hidden while it plays. Places are offsets from Leon (right, up, forward), so it plays
# anywhere; the cutscene editor (the block's Edit cutscene) changes them, or uses a spot written down in game.
g = Graph()
character = g.add('NewCharacter', 'Ashley in purple', character='ashley', name='rmc002', hue=280, saturation=55)
cutscene = g.add('Cutscene', 'Luis and the new character', cutscene='cutscenes/meet_new_character.json')
pkg = package(g, 'New character', 'Press F6 in game: Luis and a new character (Ashley in purple) meet Leon')
g.link(character, 'files', pkg, 'file')
g.link(cutscene, 'files', pkg, 'file')
g.save('19_a_new_character_in_a_cutscene.json')
with open(os.path.join(OUT, 'cutscenes', 'meet_new_character.json'), 'w', encoding='utf-8', newline='\n') as f:
    json.dump({'schema_version': 0, 'name': 'Meet a new character', 'length': 8.0, 'start': {'key': 'F6'},
               'letterbox': 0.1,
               'actors': [{'name': 'luis', 'puppet': 'luis', 'offset': [0.8, 0.0, 2.0]},
                          {'name': 'purple', 'puppet': 'rmc002', 'offset': [-0.8, 0.0, 2.0], 'hides': 'partner'}],
               'fades': [{'t': 0.0, 'until': 1.0, 'from': 1.0, 'to': 0.0},
                         {'t': 7.0, 'until': 8.0, 'from': 0.0, 'to': 1.0}],
               'subtitles': [{'t': 1.5, 'until': 4.0, 'text': 'Luis: Who is your friend, amigo?'},
                             {'t': 4.5, 'until': 6.5, 'text': 'A new character, made by remod.'}],
               'motions': []}, f, indent=2)
print('wrote cutscenes/meet_new_character.json')


# 20. A picture of your own in every photo and painting: the photos among the files (UI textures), and the picture
# frames in the world (meshes). Drop your pictures in pictures\, named anything: Name pictures gives them out, the
# photos first, then the frames, in name order (1, 2 ... 10; a picture already named after a photo's texture or a
# frame's mesh goes to that one), as copies in edits\named pictures, where matches.txt says which went where. A photo
# or frame left without a picture (fewer pictures than places) stays as the game has it.
# - Photos: Files in folder lists the files holding one, and Replace photo takes <its name>.png from the copies (Join
#   path, {name}). Stains are off: on the colour photos (Leon's, the lab team, the two quest photos) they'd carry the
#   old picture over. Photos printed inside a book, a badge or a phone aren't in the list.
# - Paintings: Files in folder lists the 78 frame meshes of sm34 (the house and castle paintings), and Pictures in
#   frames puts each frame's picture on its canvas. Several frames share one texture (an atlas of paintings); each
#   atlas is written once with all of its frames' pictures. The canvas textures are the paintings' atlases (found by
#   eye, 2026-10-12); a frame using another texture for its picture needs its name added there.
PHOTOS = ['file_033_00', 'file_039_00', 'file_041_00', 'file_048_00', 'file_049_00', 'file_051_00', 'file_078_00',
          'file_089_00', 'questfile_main_002_01', 'questfile_main_002_02']
# The frame meshes holding a painting (every sm34 frame whose canvas uses one of PAINTINGS below), in path order.
FRAMES = ['512_00', '512_01', '512_02', '513_00', '513_01', '513_02', '517_00', '517_01', '517_02', '543_00', '544_00', '545_00', '547_00', '548_00', '549_00', '550_00', '551_00', '552_00', '575_00', '576_00', '579_00', '580_00', '581_00', '582_00', '583_00', '584_00', '596_00', '597_00', '598_00', '599_00', '600_00', '601_00', '602_00', '603_00', '604_00', '605_00', '606_00', '607_00', '608_00', '609_00', '610_00', '611_00', '612_00', '613_00', '614_00', '615_00', '616_00', '617_00', '618_00', '619_00', '620_00', '621_00', '622_00', '623_00', '624_00', '625_00', '626_00', '628_00', '630_00', '631_00', '634_00', '636_00', '669_00', '672_00', '672_01', '672_02', '672_03', '672_04', '672_05', '672_06', '674_00', '675_00', '676_00', '677_00', '678_00', '679_00', '680_00', '681_00']
g = Graph()
yours = g.add('NamePictures', 'Your pictures, named for each place')
named = g.add('Split')
g.link(yours, 'folder', named, 'in')
files = g.add('FilesInFolder', 'Every photo in the files', folder=UI[:-1], on_fail='skip',
              pattern=';'.join('cs_ui3210_%s_iam.tex.*' % p for p in PHOTOS))
photos = g.add('Split')
tex = g.add('LoadTex')
split = g.add('Split')
export = g.add('ExportImage')
picture = g.add('JoinPath', 'Its picture', add='{name}.png')
photo = g.add('ReplacePhoto', stains=0)  # stains: a colour photo's colour is its picture
convert = g.add('SaveTex')
pkg = package(g, 'My pictures', 'A picture of your own in every photo of the files and painting on the walls')
g.link(files, 'files', photos, 'in')
g.link(photos, 'out', tex, 'tex')  # first: a list takes its kind from its first link (textures only)
g.link(photos, 'out', yours, 'for')  # the photos get pictures first
g.link(tex, 'tex', split, 'in')
g.link(split, 'out', export, 'tex')
g.link(split, 'out', convert, 'original')
g.link(export, 'png', photo, 'frame')
g.link(named, 'out', picture, 'folder')
g.link(picture, 'path', photo, 'picture')
g.link(photo, 'image', convert, 'image')
g.link(convert, 'tex', pkg, 'tex')
PAINTINGS = ['512_a', '512_b', '512_c', '513_a', '513_b', '513_c', '517_a', '517_b', '517_c', '543_a', '543_b', '575_a',
             '579_c', '580_a', '596_a', '601_a', '605_a', '610_a', '614_a', '618_a', '623_a', '626_a', '628', '632',
             '635', '669', '672_a', '672_b', '673', '676', '680']
meshes = g.add('FilesInFolder', 'Every picture frame', folder='{game}/_chainsaw/environment/sm/sm3x/sm34',
               pattern=';'.join('sm34_%s.mesh.*' % f for f in FRAMES), subfolders='true')
frames = g.add('Split')
paint = g.add('FramePictures', canvas=';'.join('sm34_%s_albd*' % p for p in PAINTINGS))
g.link(meshes, 'files', frames, 'in')
g.link(frames, 'out', yours, 'for')  # then the frames
g.link(frames, 'out', paint, 'frames')
g.link(named, 'out', paint, 'pictures')
g.link(paint, 'textures', pkg, 'tex')
g.save('20_a_picture_in_every_photo_and_painting.json')


# 21. Talk topics: entries in the merchant's menu (after Buy, Sell, Upgrade, Trade), and in remod's talk menu on Ashley
# (Interact on her). Each topic is a cutscene picked from the menu; flags decide which show: the second of each only
# once the merchant's Talk has played in this save (remod:talk_merchant, the flag named after that file). Topics show
# in their order (lower first). The merchant's needs his menu's layout, made from the game files and installed by
# Fluffy. Ashley's kind is ch2_a1z0 (REFramework's menu > remod cutscenes > Characters near Leon shows others).
TOPICS = [('talk_merchant', 'merchant', 'Talk', 1, None, 'Merchant: remod says hello, stranger.'),
          ('talk_merchant_ashley', 'merchant', 'Ask about Ashley', 2, 'remod:talk_merchant',
           'Merchant: shown once you have talked to me in this save.'),
          ('talk_ashley', 'ch2_a1z0', 'How are you holding up?', 1, None, 'Ashley: remod says hello, Leon.'),
          ('talk_ashley_merchant', 'ch2_a1z0', 'About the merchant', 2, 'remod:talk_merchant',
           'Ashley: shown once you have talked to the merchant in this save.')]
g = Graph()
pkg = package(g, 'Talk topics', 'Talk to the merchant (his menu) and to Ashley (Interact on her): topics that play cutscenes')
for name, npc, label, order, flag, line in TOPICS:
    cutscene = g.add('Cutscene', label, cutscene='cutscenes/%s.json' % name)
    g.link(cutscene, 'files', pkg, 'file')
    trigger = {'topic': {'npc': npc, 'label': label, 'order': order}}
    if flag:
        trigger['flags'] = [flag]
    with open(os.path.join(OUT, 'cutscenes', name + '.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'schema_version': 0, 'name': label, 'length': 4.0, 'letterbox': 0.12, 'trigger': trigger,
                   'subtitles': [{'t': 0.5, 'until': 3.5, 'text': line}],
                   'fades': [{'t': 0.0, 'until': 0.5, 'from': 1.0, 'to': 0.0},
                             {'t': 3.5, 'until': 4.0, 'from': 0.0, 'to': 1.0}]}, f, indent=2)
    print('wrote cutscenes/%s.json' % name)
g.save('21_talk_topics.json')

# 22. Any of the game's movies replaced by your videos, in one go: put a video in videos\ named after the movie it
# replaces (mva000.mp4; the ids are in README.md) and run. Files in folder lists every movie the game has; Replace
# movie runs for each, takes videos\<its id>.mp4, and leaves a movie without one as the game has it. A replaced movie
# gets its 1080p copy and its sound packages too, if it has them. Videos are cut, or hold their last frame, to the
# movie's length (Same length).
g = Graph()
movies = g.add('FilesInFolder', 'Every movie in the game', folder='{game}/streaming/_chainsaw/movie',
               pattern='*.mov.1.x64', subfolders='true')
movie = g.add('ReplaceMovie', video='videos/{name}.mp4', missing='skip')
pkg = package(g, 'My movies', "The game's movies replaced by your videos")
g.link(movies, 'files', movie, 'movie')
g.link(movie, 'movie', pkg, 'file')
g.link(movie, 'fhd', pkg, 'file')
g.link(movie, 'sound', pkg, 'file')
g.save('22_replace_movies_from_a_folder.json')

# 23. AI (CLAUDE.md §10 M4): your picture made into an old photograph by an AI model on your graphics card, then put in
# an old photo frame as example 3 does (Replace photo: the frame untouched, the old photo's toning and ageing kept), then
# AI cleanup redraws the seam where the new photo meets the frame, and Keep big changes lets only its real fixes through
# (its overall softening doesn't).
# Needs the AI tool and model set in the Pipeline panel. Not on a character texture: the AI turns a texture sheet's
# layout into mush (spike 2026-10-13).
g = Graph()
(src, port), finish = document(g, GAMES[game], GAMES[game]['photo'], 'The framed photo')
export = g.add('ExportImage')
picture = g.add('ImportImage', 'Your picture (change me)', png='picture.png')
ai = g.add('GenerateImage', 'Make it an old photograph',
           prompt='old sepia photograph of a stone castle on a hill at dusk, 1900s, film grain, detailed',
           strength=75)
photo = g.add('ReplacePhoto', picture_y=GAMES[game]['photo_y'])
frame = g.add('Split')
clean = g.add('AiCleanup', 'Clean the seam', prompt='old photograph in a worn dark wooden frame, seamless edge')
replaced = g.add('Split')
keep = g.add('KeepChanges', 'Only the real fixes')
g.link(src, port, export, 'tex')
g.link(picture, 'image', ai, 'image')
g.link(export, 'png', frame, 'in')
g.link(frame, 'out', photo, 'frame')
g.link(frame, 'out', clean, 'original')
g.link(ai, 'image', photo, 'picture')
g.link(photo, 'image', replaced, 'in')
g.link(replaced, 'out', clean, 'image')
g.link(replaced, 'out', keep, 'before')
g.link(clean, 'image', keep, 'after')
finish(keep, 'My AI photo', 'An AI-made old photograph in place of an old photo')
g.save('23_an_ai_photo_in_a_frame.json')

# 24. AI on a character (CLAUDE.md §10 M4, the material path): the AI can't repaint a texture sheet (its layout turns
# to mush), so Make material makes a repeating picture of a material from the prompt alone, and Lay material on puts it
# on the trousers' parts (Mesh mask), keeping the original's folds, dirt and wear; both copies written.
g = Graph()
texture = g.add('PartTexture', "Leon's trousers", mesh=LEON, materials='Pants_Mat')
split = g.add('Split')
stream = g.add('StreamingCopy')
full = g.add('Split')
export = g.add('ExportImage')
mask = g.add('MeshMask', 'Where the trousers are', mesh=LEON, materials='Pants_Mat')
material = g.add('MakeMaterial', 'Khaki canvas (change me)',
                 prompt='seamless khaki canvas fabric texture, flat lay, top-down, evenly lit, fine twill weave, '
                        'high detail')
lay = g.add('LayMaterial')
lsplit = g.add('Split')
convert = g.add('custom:convert_with_streaming')
pkg = package(g, 'Khaki trousers', "Leon's trousers in an AI-made khaki canvas")
g.link(texture, 'tex', split, 'in')
g.link(split, 'out', stream, 'tex')
g.link(split, 'out', convert, 'in2')
g.link(stream, 'full', full, 'in')
g.link(full, 'out', export, 'tex')
g.link(full, 'out', mask, 'size_of')
g.link(export, 'png', lay, 'image')
g.link(material, 'image', lay, 'material')
g.link(mask, 'image', lay, 'mask')
g.link(lay, 'image', lsplit, 'in')
g.link(lsplit, 'out', convert, 'in1')
g.link(lsplit, 'out', pkg, 'preview')
g.link(stream, 'streaming', convert, 'in3')
g.link(convert, 'out9', pkg, 'tex')
g.link(convert, 'out10', pkg, 'tex')
g.save('24_a_new_material_on_a_character.json')


# Sample pictures: a picture to put in a frame, and a logo with transparency.
def png(name, w, h, pixel):
    rows = b''.join(b'\0' + b''.join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    data += chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')
    open(os.path.join(OUT, name), 'wb').write(data)
    print('wrote', name)


def sky(x, y):  # a sunset over hills: sky gradient, a sun, two hill lines
    w, h = 480, 360
    sun = (x - 300) ** 2 + (y - 150) ** 2 < 45 ** 2
    hill1 = y > 250 + 30 * __import__('math').sin(x / 60)
    hill2 = y > 290 + 20 * __import__('math').sin(x / 35 + 1)
    if hill2:
        return (40, 60, 45, 255)
    if hill1:
        return (70, 95, 70, 255)
    if sun:
        return (255, 220, 140, 255)
    t = y / h
    return (int(250 - 120 * t), int(150 - 60 * t), int(90 + 90 * t), 255)


def logo(x, y):  # a red ring with a bar, transparent around it
    d = ((x - 64) ** 2 + (y - 64) ** 2) ** 0.5
    if 44 < d < 58 or (abs(y - 64) < 8 and abs(x - 64) < 40):
        return (200, 30, 30, 255)
    return (0, 0, 0, 0)


os.makedirs(OUT, exist_ok=True)
png('picture.png', 480, 360, sky)
png('logo.png', 128, 128, logo)
os.makedirs(os.path.join(OUT, 'pictures'), exist_ok=True)
for i, p in enumerate(PHOTOS):  # example 20's: the sunset in a different colour for each photo
    tint = lambda x, y, i=i: (lambda r, g_, b, a: (g_, b, r, a) if i % 3 == 1 else (b, r, g_, a) if i % 3 == 2
                              else (r, g_, b, a))(*sky((x + 37 * i) % 480, y))
    png('pictures/sample %02d.png' % (i + 1), 480, 360, tint)

# The paintings': a colour card each with its frame's number on it, so a look in game says which frame took which
# sample (and matches.txt which file it was).
GLYPHS = {'0': '111101101101111', '1': '010110010010111', '2': '111001111100111', '3': '111001111001111',
          '4': '101101111001001', '5': '111100111001111', '6': '111100111101111', '7': '111001001001001',
          '8': '111101111101111', '9': '111101111001111', '_': '000000000000111'}


def card(label, i, n):
    import colorsys
    bg = tuple(int(255 * c) for c in colorsys.hsv_to_rgb(i / n, 0.55, 0.9)) + (255,)
    w, h, k = 240, 320, 6  # k: a glyph pixel's size
    x0, y0 = (w - (4 * len(label) - 1) * k) // 2, (h - 5 * k) // 2

    def pixel(x, y):
        if min(x, y, w - 1 - x, h - 1 - y) < 8:
            return (20, 20, 20, 255)
        gx, gy = (x - x0) // k, (y - y0) // k
        if x >= x0 and y >= y0 and gy < 5 and gx < 4 * len(label) and gx % 4 < 3 and                 GLYPHS[label[gx // 4]][gy * 3 + gx % 4] == '1':
            return (20, 20, 20, 255)
        return bg
    return pixel


for i, f in enumerate(FRAMES):
    png('pictures/sample %02d.png' % (len(PHOTOS) + i + 1), 240, 320, card(f, i, len(FRAMES)))


# Sample sound: three short beeps (48 kHz mono).
def wav(name, seconds):
    import math
    rate = 48000
    samples = [int(12000 * math.sin(2 * math.pi * 880 * n / rate)) if (n % (rate // 3)) < rate // 10 else 0
               for n in range(int(seconds * rate))]
    data = struct.pack('<%dh' % len(samples), *samples)
    head = b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16)
    open(os.path.join(OUT, name), 'wb').write(head + b'data' + struct.pack('<I', len(data)) + data)
    print('wrote', name)


wav('beep.wav', 1)

# The sample picture and logo, for the other games' examples too.
for other in ['re2r', 're3r']:
    for name in ['picture.png', 'logo.png']:
        shutil.copyfile(os.path.join(OUT, name), os.path.join(EXAMPLES, other, name))
