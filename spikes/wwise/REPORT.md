# Wwise WEM header spike: findings and open questions

**Answered 2026-10-06** (checked against these files and the game's mva000 music and dialogue): every unknown
below except the PCM +-1 is resolved; the current layouts are in CLAUDE.md §10, "Story movies' sound".

For a reviewer. Everything here was read from 21 reference files that Wwise 2021.1.14 made from 7 known WAVs; the
steps are in `README.md`. Goal (CLAUDE.md §10, "Story movies' sound"): write RE4R's `.wem` files ourselves, without
Wwise. The game's banks are Wwise 2021.1 (bank version 140).

## Ask

1. Check the layouts below against the files (`python read_wem.py` dumps every field). Say where a claim is wrong.
2. Work out the **unknown fields** (the list at the end), ranked by what blocks a writer. For the Vorbis ones the
   useful answer is "this is derived from X by this formula", not just "constant".
3. Say which fields a writer can safely copy from a reference file and which must be computed.

Nothing has been tested in game. No source code was copied: the layouts come from the files alone. If you cite a
reader (vgmstream, ISC-style licence, fine to read; wwiser and RingingBloom have no licence: don't copy), say so.

## Files

- Inputs: `in\*.wav` (48 kHz, 16-bit; made by `make_samples.sh`, all 7 exist locally).

  | WAV | Channels | Length | Content |
  |---|---|---|---|
  | `tone_mono` | 1 | 3 s | tone |
  | `tone_stereo` | 2 | 3 s | a pitch per channel |
  | `tone_3ch` | 3 | 3 s | a pitch per channel |
  | `sweep_stereo` | 2 | 5 s | sweep |
  | `noise_stereo` | 2 | 3 s | noise |
  | `silence_stereo` | 2 | 3 s | silence |
  | `odd_length_stereo` | 2 | 1.2345 s (59256 samples) | tone |

- Outputs: `wem\Windows\{opus,vorbis,pcm}\<name>.wem` (7 each) and a `Wwise.dat` in each folder (ignore).
  Settings: Wwise defaults for each codec, as the input's rate and channels (ShareSets `remod_vorbis`, `remod_opus`,
  `remod_pcm`, `samples.wsources`). These folders, `in\` and the Wwise project are git-ignored; they exist on this PC.
- `read_wem.py`: dumps the fields below for every file. Run it first.
- Real game audio, for comparison: `streaming\_chainsaw\sound\wwise\ch_mva000_bgm.spck.1.x64` (AKPK, one Wwise
  Vorbis WEM, stereo 48 kHz) and `ch_mva000_dialogue.spck.1.x64.<lang>` (one Wwise Opus WEM, 3 channels, 48 kHz),
  under the extracted game folder (`REMOD_GAME`; ask the user for the path). Not looked at yet beyond their codec tags.

## Opus (tag 0x3041): solved, one thing unchecked

RIFF, chunks `fmt `, `seek`, `data` in that order, no padding; RIFF size = length - 8.

- `fmt ` is 36 bytes: tag, channels, 48000, avg bytes/s = data x 48000 / total samples (floor), align 0, bits 0,
  cbSize **16 but 18 extra bytes follow**.
- Extra (little endian): u16 960 | u16 channel config | u16 0 | u32 total samples | u32 packet count | u16 pre-skip
  312 | u8 1 | u8 mapping family (0 for mono and stereo, 1 for 3 channels).
- Channel config = mask << 12 | 0x100 | channels (mono 0x4101, stereo 0x3102, 3 ch 0x7103).
- Packet count = ceil(samples / 960) + 1 (3 s: 151; 59256 samples: 63; 5 s: 251). All 7 match.
- `seek`: one u16 per packet, its byte size. `data`: the raw Opus packets back to back, no length prefixes. Sizes sum
  to the data size exactly in all 7.
- Packets are 20 ms CELT fullband (TOC 0xf8 mono, 0xfc stereo); silence packets are 3 bytes (`fc ff fe`).
- **Unchecked:** the 3-channel file is two streams (stereo + mono), the first self-delimited as libopus's
  multistream encoder packs them. Inferred from the bytes, not verified by decoding.
- Open: how the pre-skip packet and the +1 interact with the sample count for lengths that aren't a multiple of 960
  (only the 59256-sample file tests this; it fits).

## PCM (tag 0xFFFE): solved, one unexplained difference

RIFF, `fmt ` (24), `JUNK` (4 zero bytes), `data` at offset 64.

- `fmt `: tag 0xFFFE (extensible) but cbSize 6 with 6 extra bytes; avg = 48000 x align; align = 2 x channels; bits 16.
- Extra: u16 0 (**0x10 for the 3-channel file**) | u16 channel config (as Opus) | u16 0.
- `data`: 16-bit little-endian interleaved.
- **Unexplained:** samples equal the WAV's in 4 of 7 files (mono, noise, silence, odd length). In `tone_stereo`,
  `sweep_stereo` and `tone_3ch`, 1-2% of samples differ by exactly +-1 (3120, 5934 and 4080 samples). Not a plain
  rounding of x * 32767 / 32768 (round, trunc, floor all tried: 3120 / 284280 / 143700 differing for `tone_stereo`).
  Hypothesis: a float stage inside Wwise. Peak of the sources is 16384. Why those three and not noise? What do they
  share (a tone or sweep, with exact sample values the float path rounds differently)? Harmless to hearing; only
  matters if byte-exact output is wanted.

## Vorbis (tag 0xFFFF): container solved, the audio payload not decoded

RIFF with `fmt ` (66 bytes) then `data`. `fmt `: tag, channels, 48000, avg bytes/s = (data - seek table) x 48000 /
total samples (floor), align 0, bits 0, cbSize 48, 48 extra bytes. Offsets are within the extra:

| At | Field | Status |
|---|---|---|
| 0 | u16: 0, but 0x10 for the 3-channel file | **unknown** |
| 2 | u16 channel config (as above) | known |
| 6 | u32 total samples | known |
| 10 | u32 setup block size (u16 size field + payload): 203 mono and 3 ch, 217 stereo | known |
| 14 | u32 data size minus the seek table | known |
| 20, 32 | u16 X, the same value in both places: 960, 64, 0, 8 in these files | **unknown** |
| 22 | u32 seek table size in bytes = 4 x entries | known |
| 26 | u32 audio offset in `data` = seek table + setup block | known |
| 30 | u16 largest audio packet payload | known (matches a walk of every file) |
| 34, 38 | two u32, constant per channel layout (stereo 0x3ed0, 0x40b0; mono and 3 ch 0x4704, 0x48cc) | **unknown** |
| 42 | u32 id, constant per layout (stereo 0xec69cb18; mono and 3 ch 0x081e7c2f) | looks like a hash of the setup |
| 46 | u8 blocksize-0 power (8), u8 blocksize-1 power (11) | known |

`data` = seek table, then setup block (u16 size, payload), then audio packets (each u16 size + payload), ending
exactly at the chunk's end (walked on all 7).

Seek table: `floor(samples / 16384)` entries of u16 a + u16 b (3 s: 8, 59256 samples: 3, 5 s: 14).
`a` is 16384, except the first which is 16384 plus 576 or 704 (it varies by file, so it depends on the first blocks; 576 in the mono tone and the silence, 704 in
the stereo and 3 ch tones and the noise). `b` is about the span's audio size in the loud files but **the silent file's
entries sum to 604 while its whole audio is 429 bytes**, so it isn't a byte count.

X@20 and X@32 (above) vary by file with no obvious rule: the 3 s mono tone and the silence have 960; the stereo tone
and 3 ch tone 64; noise and sweep 0; the odd-length file 8. Worth comparing against the seek table's first `a`
(576 / 704), the last span's remainder (samples mod 16384: 12928 for the 3 s files) and the packet blocksize
pattern.

### Not decoded (the hard part of a Vorbis writer)

- The setup payload: how codebooks are stored (Wwise ids into its library, not the full codebooks), and the floor,
  residue and mode data. Mono / stereo / 3 ch payloads differ; mono and 3 ch have the same size and id.
- The audio packet form: the first byte seems to carry the mode bit in place of Vorbis's packet-type bit (as in
  vgmstream's Wwise reader; not checked here).
- Whether Vorbis's own codebooks (libvorbis) can be expressed in Wwise's library at all (CLAUDE.md: uncertain). A
  yes / no from the setup bytes would decide whether a Vorbis writer is possible; Opus -> the music slot in game is
  the fallback.

## What would help most, in order

1. The meaning of the Vorbis seek table's `b` and of X@20 / X@32 / @34 / @38 / u16@0 (the "Open" cells above).
2. Whether the setup block can be generated: what its bytes encode and whether it is a fixed blob per channel layout
   (mono and 3 ch share a size and id, which suggests a blob chosen by layout) that a writer could ship.
3. The PCM +-1 difference (low priority).
4. Check against the game's own WEMs (the two spck files), which came from Wwise 2021.1 too: do the same fields
   hold on real music and dialogue?
