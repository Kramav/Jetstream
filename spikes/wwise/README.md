# Wwise reference samples

Goal: write the game's `.wem` audio files without Wwise (CLAUDE.md §10, "Story movies' sound"). These steps make
reference files with Wwise once, so the tool's own encoder can be checked against them. Wwise is needed only for
this and isn't part of the tool.

The game's banks are Wwise 2021.1 (bank version 140), so the samples must come from that version.

## What's here

| File | What it is |
|---|---|
| `in\*.wav` | 7 test signals, 48 kHz: tones (mono, stereo, 3 channels, each channel its own pitch), a sweep, noise, silence, and an odd length (1.2345 s). Made by `make_samples.sh` (needs ffmpeg). |
| `samples.wsources` | Every WAV three ways: `remod_vorbis`, `remod_opus`, `remod_pcm`. |
| `run.cmd` | Runs Wwise's command line converter on the list; output in `wem\`, its log in `wem_log.txt`. |

`in\`, `wem\`, `project\` and `wem_log.txt` are in `.gitignore`.

## Steps

1. **Install Wwise 2021.1.14.** Install the Audiokinetic Launcher (audiokinetic.com, free account; the
   non-commercial licence covers this). In it, open *Wwise*, choose version **2021.1.14** (build 8108 if it asks),
   and install. The defaults are enough; *Authoring* must be included.
2. **Make a project in `spikes\wwise\project\`.** Start Wwise 2021.1.14, then *Project → New*. Name it
   `WemSamples` and put it in this folder's `project` subfolder. Platforms: keep *Windows*.
3. **Make three conversion settings with these exact names.** In the *Project Explorer*, go to the *ShareSets*
   tab, then *Conversion Settings → Default Work Unit*. For each, right-click, *New Child → Conversion Settings*,
   name it, double-click it, and in the editor set the **Format** for **Windows**:

   | Name | Format |
   |---|---|
   | `remod_vorbis` | Vorbis (keep its default quality) |
   | `remod_opus` | Opus (keep its default bit rate) |
   | `remod_pcm` | PCM |

   Leave sample rate and channels at their defaults (as the input). Save the project (Ctrl+S), then close Wwise.
4. **Run `run.cmd`** (double-click it, or run it from a command prompt in this folder). It prints Wwise's output
   and ends with "Done." It writes 21 files: `wem\Windows\vorbis\`, `\opus\`, `\pcm\` (the `Windows` level may
   be missing, depending on the version).
5. **Tell Claude it's done**, and anything that differed from these steps (menu names, a different Opus or Vorbis
   setting). The files are read from disk; nothing needs sending.

If `run.cmd` fails, the reason is in `wem_log.txt`. The usual causes: the ShareSet names don't match exactly, or the
project wasn't saved after adding them.
