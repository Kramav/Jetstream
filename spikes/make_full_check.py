"""Builds the full check's test pack (spikes/full_check.md) into spikes/out/full_check:
graphs/  the RE4R examples plus the check's own graphs (open them in remod from here), with their runs' outputs
mods/    every mod the guide installs, named by session ("S1 - My shirt.zip")

Run from the repo root once remod is built:  python spikes/make_full_check.py
Needs REMOD_GAME (RE4R's extracted natives\\stm), ffmpeg on PATH and Pillow. The AI examples (23, 24) reuse the
pictures already in the run cache; without them they generate (the AI settings in remod's Pipeline panel).
"""
import json, math, os, shutil, struct, subprocess, sys, wave
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REMOD = os.path.join(ROOT, 'build', 'cli', 'remod.exe')
GAME = os.environ.get('REMOD_GAME') or sys.exit('set REMOD_GAME to RE4R\'s extracted natives\\stm')
OUT = os.path.join(ROOT, 'spikes', 'out', 'full_check')
WORK, MODS = os.path.join(OUT, 'graphs'), os.path.join(OUT, 'mods')
LEON = '_chainsaw/character/ch/cha0/cha000/00/cha000_00_'


def remod(*args):
    print('remod', *args[:3], flush=True)
    subprocess.run([REMOD, *args], check=True)


def tex2png(game_path, png):
    remod('tex2png', '--profile', os.path.join(ROOT, 'profiles', 're4r.toml'), '--tex',
          os.path.join(GAME, game_path), '--out', png)
    return Image.open(png).convert('RGBA')


def load(name):
    with open(os.path.join(ROOT, 'examples', 're4r', name + '.json'), encoding='utf-8') as f:
        return json.load(f)


def save(g, name):
    for n in g['nodes']:  # tidied up when opened in the app
        n['params'].pop('x', None), n['params'].pop('y', None)
    with open(os.path.join(WORK, name + '.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(g, f, indent=2)


def node(g, type_):
    return next(n for n in g['nodes'] if n['type'] == type_)


def graph(nodes, links):
    return {'schema_version': 0, 'profile': 're4r',
            'nodes': [{'id': i + 1, 'type': t, 'params': p} for i, (t, p) in enumerate(nodes)],
            'links': [{'from': list(a), 'to': list(b)} for a, b in links]}


def package(name, description):
    return ('PackageMod', {'name': name, 'out': 'mods', 'version': '1.0', 'author': '', 'description': description,
                           'replace': 'true'})


def beeps(path, pitches, every, length):
    """A mono 48 kHz WAV: a 0.25 s beep at each pitch, one every `every` seconds."""
    rate, n = 48000, int(length * 48000)
    data = bytearray(n * 2)
    for k, f in enumerate(pitches):
        s0 = int(k * every * rate)
        for i in range(int(0.25 * rate)):
            if s0 + i < n:
                struct.pack_into('<h', data, (s0 + i) * 2, int(12000 * math.sin(2 * math.pi * f * i / rate)))
    with wave.open(path, 'wb') as w:
        w.setnchannels(1), w.setsampwidth(2), w.setframerate(rate), w.writeframes(bytes(data))


# 1. The examples, fresh copies (the run cache keeps unchanged steps quick), and the check's own files.
os.makedirs(MODS, exist_ok=True)
for d in ('mods', 'edits'):  # outputs of the last build: made again
    shutil.rmtree(os.path.join(WORK, d), ignore_errors=True)
shutil.copytree(os.path.join(ROOT, 'examples', 're4r'), WORK, dirs_exist_ok=True,
                ignore=shutil.ignore_patterns('mods', 'edits'))
shutil.copytree(os.path.join(ROOT, 'spikes', 'full_check'), WORK, dirs_exist_ok=True)
os.makedirs(os.path.join(WORK, 'edits'), exist_ok=True)
font = ImageFont.truetype('arialbd.ttf', 56)

# 2. "Your edits" for the Edit image steps, made before the runs (Export image never overwrites a file for editing).
doc = tex2png('_chainsaw/ui/ui3200/tex/cs_ui3210_file_008_00_iam.tex.143221013', os.path.join(WORK, 'edits', 'file_008.png'))
ImageDraw.Draw(doc).text((265, 560), 'REMOD CHECK 01', font=font, fill=(220, 0, 0, 255))
doc.save(os.path.join(WORK, 'edits', 'file_008.png'))

shirt = tex2png('streaming/' + LEON + 'upperbody_albd.tex.143221013', os.path.join(WORK, 'edits', 'leon_shirt.png'))
w, a = shirt.width, shirt.getchannel('A')  # red bands across the whole sheet, alpha kept
mask = Image.new('L', shirt.size, 0)
for y in range(0, shirt.height, w // 16):
    ImageDraw.Draw(mask).rectangle((0, y, w, y + w // 32), fill=255)
red = Image.merge('RGBA', (Image.new('L', shirt.size, 230), Image.new('L', shirt.size, 20),
                           Image.new('L', shirt.size, 20), a))
Image.composite(red, shirt, mask).save(os.path.join(WORK, 'edits', 'leon_shirt.png'))

# Channel check: the shirt's colour texture with its alpha all 0; the trousers' normal map with R and G flat (128).
alpha = tex2png('streaming/' + LEON + 'upperbody_albd.tex.143221013', os.path.join(WORK, 'edits', 'shirt_alpha0.png'))
alpha.putalpha(0)
alpha.save(os.path.join(WORK, 'edits', 'shirt_alpha0.png'))
nrm = tex2png('streaming/' + LEON + 'lowerbody_nrmr.tex.143221013', os.path.join(WORK, 'edits', 'trousers_flat.png'))
_, _, b, a = nrm.split()
Image.merge('RGBA', (Image.new('L', nrm.size, 128), Image.new('L', nrm.size, 128), b, a)).save(
    os.path.join(WORK, 'edits', 'trousers_flat.png'))

# 3. Media: a 15 s video with a steady tone (the opening movie at its own length), rising beeps for the menu sounds.
subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc=size=1920x1080:rate=30:duration=15',
                '-f', 'lavfi', '-i', 'sine=frequency=440:duration=15', '-c:v', 'libx264', '-pix_fmt', 'yuv420p',
                '-c:a', 'aac', '-shortest', os.path.join(WORK, 'short_intro.mp4')], check=True)
beeps(os.path.join(WORK, 'rising.wav'), [523, 784, 1047], 1.0, 3.0)

# 4. The check's own graphs.
g = load('02_recolour_a_texture')  # the padded stamp (468 wide, stored 512)
node(g, 'LoadTex')['params']['tex'] = '{game}/_chainsaw/ui/ui3200/tex/cs_ui3200_stamp_im.tex.143221013'
node(g, 'AdjustColour')['params'].update(hue='180', saturation='0')
node(g, 'PackageMod')['params'].update(name='Stamp check', description='The Completed stamp, green to magenta')
save(g, 'x1_stamp')

# Channel check: example 7's chain twice into one package.
seven = load('07_edit_a_character_texture')
ch = {'schema_version': 0, 'profile': 're4r', 'nodes': [], 'links': []}
for k, (materials, texture, png) in enumerate([('Shirts_Mat', '', 'edits/shirt_alpha0.png'),
                                               ('Pants_Mat', '*_nrmr*', 'edits/trousers_flat.png')]):
    off = k * 10
    for n in seven['nodes']:
        if n['type'] == 'PackageMod' and k:
            continue
        p = dict(n['params'])
        if n['type'] == 'PartTexture':
            p.update(materials=materials, texture=texture, title=materials)
        if n['type'] == 'ExportImage':
            p['png'] = png
        if n['type'] == 'PackageMod':
            p.update(name='Channel check', description="Shirt alpha 0; trousers' normal map flat in R and G")
        ch['nodes'].append({'id': n['id'] + off, 'type': n['type'], 'params': p})
    pkg = node(seven, 'PackageMod')['id']
    for l in seven['links']:
        ch['links'].append({'from': [l['from'][0] + off, l['from'][1]],
                            'to': [pkg if l['to'][0] == pkg else l['to'][0] + off, l['to'][1]]})
save(ch, 'x2_channel_check')

g = load('22_replace_movies_from_a_folder')  # every movie but mva000 a test card (no _fhd: their bases write them)
node(g, 'FilesInFolder')['params']['pattern'] = ('mv7*.mov.1.x64;mva2??.mov.1.x64;mva3??.mov.1.x64;'
                                                 'mva4??.mov.1.x64;richtutorial_movie_0??.mov.1.x64')
node(g, 'ReplaceMovie')['params'].update(video='', missing='stop')
node(g, 'PackageMod')['params'].update(name='Movie cards', description="Every game movie but mva000 a test card naming it")
save(g, 'x3_movie_cards')

g = load('12_replace_a_movie')
node(g, 'ReplaceMovie')['params'].update(video='short_intro.mp4', same_length='false')
node(g, 'PackageMod')['params'].update(name='Short intro', description='mva000: a 15 s test pattern with a steady tone')
save(g, 'x4_short_intro')

sounds = []  # every sound of the game's menu bank (all held in the bank: none streamed)
for bank in ('ch_ui_ingame_media',):
    listing = subprocess.run([REMOD, 'sounds', '--file', os.path.join(GAME, '_chainsaw/sound/wwise', bank + '.sbnk.1.x64')],
                             check=True, capture_output=True, text=True).stdout
    sounds += ['{game}/_chainsaw/sound/wwise/%s.sbnk.1.x64#%s' % (bank, l.split()[0])
               for l in listing.splitlines() if l.strip() and 'not a sound' not in l]
n = len(sounds)
save(graph([('GameSound', {'sound': s, 'audio': 'rising.wav'}) for s in sounds]
           + [('ReplaceSounds', {'game': '{game}', 'same_length': 'false'}),
              package('Menu sounds', "The menus' %d sounds: three rising beeps, 3 s" % n)],
           [((k + 1, 'sound'), (n + 1, 'sounds')) for k in range(n)] + [((n + 1, 'files'), (n + 2, 'file'))]),
     'x5_menu_sounds')

for name, file, title in [('x6_after_movie', 'after_movie', 'After the opening'),
                          ('x7_event_animation', 'event_animation', 'Event animation')]:
    save(graph([('Cutscene', {'cutscene': 'cutscenes/%s.json' % file, 'editor': ''}),
                package(title, 'remod full check')], [((1, 'files'), (2, 'file'))]), name)
save(graph([('Cutscene', {'cutscene': 'cutscenes/my_own.json', 'editor': ''})], []), 'x8_my_own_cutscene')
save(graph([('LuaScript', {'script': 'broken_check.lua', 'editor': ''})], []), 'x9_broken_script')
g = load('05_many_textures_at_once')  # a list of hand edits: the YOUR STEP list and the ◀ ▶ (not run here)
node(g, 'AdjustColour').update(type='EditImage', params={'editor': '', 'title': 'Your edits'})
node(g, 'ExportImage')['params']['png'] = 'edits/batch/{name}.png'
for l in g['links']:
    if l['to'] == [5, 'image']:
        l['to'] = [5, 'png']
node(g, 'PackageMod')['params'].update(name='Batch edits', description='Several documents edited by hand')
save(g, 'x10_batch_edits')

# 5. Run them and gather the mods by session.
SESSIONS = {
    1: ['01_edit_a_texture_by_hand', '07_edit_a_character_texture', '20_a_picture_in_every_photo_and_painting',
        '24_a_new_material_on_a_character', 'x1_stamp'],
    2: ['04_a_logo_on_a_texture', '06_recolour_part_of_a_character', '23_an_ai_photo_in_a_frame'],
    3: ['x3_movie_cards', 'x4_short_intro', '15_play_a_game_movie', '16_insert_a_new_movie', '17_add_a_new_sound',
        'x6_after_movie', 'x2_channel_check'],
    4: ['14_replace_a_sound', 'x5_menu_sounds'],
    5: ['21_talk_topics', '18_start_a_cutscene_by_itself', '19_a_new_character_in_a_cutscene', 'x7_event_animation'],
}
for f in os.listdir(MODS):
    os.remove(os.path.join(MODS, f))
for s, names in SESSIONS.items():
    for name in names:
        before = set(os.listdir(os.path.join(WORK, 'mods'))) if os.path.isdir(os.path.join(WORK, 'mods')) else set()
        remod('run', '--graph', os.path.join(WORK, name + '.json'), '--game-files', GAME, '--edited', 'true')
        g = json.load(open(os.path.join(WORK, name + '.json'), encoding='utf-8'))
        zip_ = node(g, 'PackageMod')['params']['name'] + '.zip'
        shutil.copy(os.path.join(WORK, 'mods', zip_), os.path.join(MODS, 'S%d - %s' % (s, zip_)))
print('done:', MODS)
