"""Dumps the header fields of Wwise .wem files (opus / vorbis / pcm) made by run.cmd.

    python read_wem.py [wem\\Windows]        # default folder: wem\\Windows beside this file
Prints one block per file; findings and open questions are in REPORT.md.
"""
import struct, sys, glob, os

def chunks(d):
    p, out = 12, []
    while p < len(d):
        t, s = d[p:p + 4], struct.unpack('<I', d[p + 4:p + 8])[0]
        out.append((t.decode(), p + 8, s))
        p += 8 + s + (s & 1)
    return out

def show(path):
    d = open(path, 'rb').read()
    ch = {c[0]: c for c in chunks(d)}
    _, o, n = ch['fmt ']
    b = d[o:o + n]
    tag, c, rate, avg, al, bits, cb = struct.unpack('<HHIIHHH', b[:18])
    ex = b[18:]
    print(f"{path}  {len(d)} B  riff_ok={struct.unpack('<I', d[4:8])[0] == len(d) - 8}")
    print("  chunks", [(k, v[2]) for k, v in ch.items()])
    print(f"  fmt tag={tag:#06x} ch={c} rate={rate} avg={avg} align={al} bits={bits} cbSize={cb} extra={len(ex)}B")
    print("  extra", ex.hex(' ', 2))
    if tag == 0x3041:  # opus
        u = struct.unpack('<HHHIIHBB', ex)
        print(f"  frame={u[0]} chcfg={u[1]:#06x} z={u[2]} samples={u[3]} packets={u[4]} preskip={u[5]} ver={u[6]} family={u[7]}")
        _, so, sn = ch['seek']
        sizes = struct.unpack(f'<{sn // 2}H', d[so:so + sn])
        print(f"  seek: {len(sizes)} sizes, sum={sum(sizes)}, data={ch['data'][2]}, first={sizes[:4]}")
    elif tag == 0xFFFF:  # vorbis
        g = lambda f, i: struct.unpack(f, ex[i:i + struct.calcsize(f)])[0]
        print(f"  u16@0={g('<H', 0):#x} chcfg={g('<H', 2):#06x} samples={g('<I', 6)} setup_blk={g('<I', 10)} "
              f"after_table={g('<I', 14)} X@20={g('<H', 20)} X@32={g('<H', 32)} table={g('<I', 22)} audio_off={g('<I', 26)} "
              f"maxpkt={g('<H', 30)}")
        print(f"  @34={g('<I', 34):#x} @38={g('<I', 38):#x} id@42={g('<I', 42):#010x} blocksizes=2^{ex[46]},2^{ex[47]}")
        da = d[ch['data'][1]:ch['data'][1] + ch['data'][2]]
        tab = g('<I', 22)
        print("  seek table (u16 a, u16 b):", [struct.unpack('<HH', da[i:i + 4]) for i in range(0, tab, 4)])
    elif tag == 0xFFFE:  # pcm
        print("  data offset", ch['data'][1], "JUNK", d[ch['JUNK'][1]:ch['JUNK'][1] + 4].hex())

if __name__ == '__main__':
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wem', 'Windows')
    for f in sorted(glob.glob(os.path.join(root, '*', '*.wem'))):
        show(f)
