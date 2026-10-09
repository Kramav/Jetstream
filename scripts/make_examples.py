"""Writes the example graphs in examples/ (shipped in the release zip) and their sample pictures.

Each graph is written without block positions: the app lays such a graph out when it opens it (Tidy up). Paths to
game files start with {game}, the Game files folder set in the app, so the examples run on any PC. Fields left out
get their defaults when the graph loads. Run: python scripts/make_examples.py
"""
import json
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'examples')
UI = '{game}/_chainsaw/ui/ui3200/tex/'
LEON = '{game}/_chainsaw/character/ch/cha0/cha000/00/cha000_00.mesh.221108797'
os.makedirs(OUT, exist_ok=True)


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
            json.dump({'schema_version': 0, 'profile': 're4r', 'nodes': self.nodes, 'links': self.links}, f, indent=2)
        print('wrote', name)


def package(g, name, description):
    return g.add('PackageMod', name=name, out='mods', version='1.0', description=description, replace='true')


def convert_and_package(g, image, texture, name, description):
    """image -> Split -> Convert (original: texture) and Package's preview."""
    split = g.add('Split')
    g.link(image, 'image', split, 'in')
    convert = g.add('SaveTex')
    g.link(split, 'out', convert, 'image')
    g.link(texture, 'out', convert, 'original')
    pkg = package(g, name, description)
    g.link(convert, 'tex', pkg, 'tex')
    g.link(split, 'out', pkg, 'preview')
    return pkg


# 1. The classic: export, edit by hand, convert, package.
g = Graph()
tex = g.add('LoadTex', 'The texture to change', tex=UI + 'cs_ui3210_file_008_00_iam.tex.143221013')
split = g.add('Split')
export = g.add('ExportImage', png='edits/file_008.png')
edit = g.add('EditImage', 'Your edit')
g.link(tex, 'tex', split, 'in')
g.link(split, 'out', export, 'tex')
g.link(export, 'png', edit, 'png')
convert_and_package(g, edit, split, 'My first texture mod', 'A document, edited by hand')
g.save('01_edit_a_texture_by_hand.json')

# 2. No hand step: a colour change.
g = Graph()
tex = g.add('LoadTex', 'The texture to change', tex=UI + 'cs_ui3210_file_008_00_iam.tex.143221013')
split = g.add('Split')
export = g.add('ExportImage')  # no file: a working copy
adjust = g.add('AdjustColour', hue=150, saturation=20)
g.link(tex, 'tex', split, 'in')
g.link(split, 'out', export, 'tex')
g.link(export, 'png', adjust, 'image')
convert_and_package(g, adjust, split, 'Recoloured document', 'A document in another colour')
g.save('02_recolour_a_texture.json')

# 3. Your picture in an old photo frame.
g = Graph()
tex = g.add('LoadTex', 'The framed photo', tex=UI + 'cs_ui3210_file_039_00_iam.tex.143221013')
split = g.add('Split')
export = g.add('ExportImage')
picture = g.add('ImportImage', 'Your picture (change me)', png='picture.png')
photo = g.add('ReplacePhoto', picture_y=-30)
g.link(tex, 'tex', split, 'in')
g.link(split, 'out', export, 'tex')
g.link(export, 'png', photo, 'frame')
g.link(picture, 'image', photo, 'picture')
convert_and_package(g, photo, split, 'My photo', 'My picture in place of an old photo')
g.save('03_your_picture_in_a_photo_frame.json')

# 4. A logo on a texture.
g = Graph()
tex = g.add('LoadTex', 'The texture to change', tex=UI + 'cs_ui3210_file_008_00_iam.tex.143221013')
split = g.add('Split')
export = g.add('ExportImage')
logo = g.add('ImportImage', 'Your logo (change me)', png='logo.png')
overlay = g.add('OverlayImage', x=330, y=740, opacity=85)  # on the paper: outside it the texture is see-through
g.link(tex, 'tex', split, 'in')
g.link(split, 'out', export, 'tex')
g.link(export, 'png', overlay, 'base')
g.link(logo, 'image', overlay, 'top')
convert_and_package(g, overlay, split, 'Stamped document', 'A logo stamped on a document')
g.save('04_a_logo_on_a_texture.json')

# 5. Many textures at once: every matching file in a folder, through the same steps.
g = Graph()
files = g.add('FilesInFolder', 'The documents', folder=UI[:-1], pattern='cs_ui3210_file_00*_iam.tex.*')
tex = g.add('LoadTex')
split = g.add('Split')
export = g.add('ExportImage')
adjust = g.add('AdjustColour', hue=-40, saturation=-30)
convert = g.add('SaveTex')
pkg = package(g, 'Faded documents', 'The first documents of the files, faded')
g.link(files, 'files', tex, 'tex')
g.link(tex, 'tex', split, 'in')
g.link(split, 'out', export, 'tex')
g.link(split, 'out', convert, 'original')
g.link(export, 'png', adjust, 'image')
g.link(adjust, 'image', convert, 'image')
g.link(convert, 'tex', pkg, 'tex')
g.save('05_many_textures_at_once.json')

# 6. Recolour one part of a character: a block made of blocks.
g = Graph()
recolour = g.add('custom:recolour_part', "Leon's trousers", in1=LEON, in2='Pants_Mat', in3=120, in4=10, in5=5)
pkg = package(g, 'Green trousers', "Leon's trousers in another colour")
g.link(recolour, 'out19', pkg, 'tex')
g.link(recolour, 'out20', pkg, 'tex')
g.link(recolour, 'out21', pkg, 'preview')
g.save('06_recolour_part_of_a_character.json')

# 7. Edit a character texture by hand, at full size, replacing its streaming copy too.
g = Graph()
part = g.add('PartTexture', "Leon's shirt", mesh=LEON, materials='Shirts_Mat')
split = g.add('Split')
stream = g.add('StreamingCopy')
export = g.add('ExportImage', png='edits/leon_shirt.png')
edit = g.add('EditImage', 'Your edit')
esplit = g.add('Split')
convert = g.add('custom:convert_with_streaming')
pkg = package(g, 'My shirt', "Leon's shirt, edited by hand")
g.link(part, 'tex', split, 'in')
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
files = g.add('FilesInFolder', "Leon's textures", folder=LEON.rsplit('/', 1)[0], pattern='cha000_00_*.tex*')
tex = g.add('LoadTex')
split = g.add('Split')
match = g.add('TextMatches', 'Is it a colour texture?', pattern='*_albd*')
gate = g.add('If', 'Only colour textures')
gsplit = g.add('Split')
stream = g.add('StreamingCopy')
export = g.add('ExportImage')
adjust = g.add('AdjustColour', 'Grey', saturation=-100)
convert = g.add('custom:convert_with_streaming')
pkg = package(g, 'Grey Leon', "Leon's colour textures without colour")
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
part = g.add('PartTexture', "Leon's shirt", mesh=LEON, materials='Shirts_Mat')
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
pkg = package(g, 'Picture shirt', "A picture as Leon's shirt texture, keeping its alpha data")
g.link(part, 'tex', split, 'in')
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
tex = g.add('LoadTex', 'The texture to change', tex=UI + 'cs_ui3210_file_008_00_iam.tex.143221013')
split = g.add('Split')
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
g.link(tex, 'tex', split, 'in')
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
fsplit = g.add('Split')
g.link(first, 'value', fsplit, 'in')
convert = g.add('SaveTex')
g.link(fsplit, 'out', convert, 'image')
g.link(split, 'out', convert, 'original')
pkg = package(g, 'Picture or colour', 'my_picture.png if it is there, else a recoloured document')
g.link(convert, 'tex', pkg, 'tex')
g.link(fsplit, 'out', pkg, 'preview')
g.save('10_if_else_picture_or_recolour.json')

# 11. File steps: keep a copy of each build.
g = Graph()
tex = g.add('LoadTex', 'The texture to change', tex=UI + 'cs_ui3210_file_008_00_iam.tex.143221013')
split = g.add('Split')
export = g.add('ExportImage')
adjust = g.add('AdjustColour', brightness=-25)
pkg = convert_and_package(g, adjust, split, 'Darker document', 'A darker document, with a backup of each build')
g.link(tex, 'tex', split, 'in')
g.link(split, 'out', export, 'tex')
g.link(export, 'png', adjust, 'image')
copy = g.add('CopyFile', 'Back up the build', dest='backups/', if_exists='overwrite', create_dirs='true')
g.link(pkg, 'mod', copy, 'source')
g.save('11_back_up_each_build.json')

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
