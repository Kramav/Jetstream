#!/bin/sh
# The spike's source WAVs (README.md): 48 kHz 16-bit PCM, each a known signal, so Wwise's .wem output can be compared
# with what went in. Needs ffmpeg (only to make these; not a tool dependency). Run from anywhere.
set -e
cd "$(dirname "$0")"
mkdir -p in
wav() { ffmpeg -v error -y "$@"; }
# Tones: each channel its own pitch (440 / 660 / 880 Hz), so the channel order in the .wem shows.
wav -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=3" -af volume=0.5 -c:a pcm_s16le in/tone_mono.wav
wav -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*660*t):s=48000:d=3:c=stereo" -c:a pcm_s16le in/tone_stereo.wav
wav -f lavfi -i "aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*660*t)|0.5*sin(2*PI*880*t):s=48000:d=3:c=3.0" \
    -c:a pcm_s16le in/tone_3ch.wav
# A 5 s exponential sweep, 20 Hz to 20 kHz: every frequency once (the codecs' quality across the range).
wav -f lavfi -i "aevalsrc=0.5*sin(2*PI*20*5*(exp(t/5*log(1000))-1)/log(1000)):s=48000:d=5:c=stereo" \
    -c:a pcm_s16le in/sweep_stereo.wav
wav -f lavfi -i "anoisesrc=color=white:amplitude=0.3:sample_rate=48000:duration=3" -ac 2 -c:a pcm_s16le \
    in/noise_stereo.wav
wav -f lavfi -i "anullsrc=channel_layout=stereo:sample_rate=48000" -t 3 -c:a pcm_s16le in/silence_stereo.wav
# 59,256 samples (1.2345 s): not a whole number of codec frames, so the end padding shows.
wav -f lavfi -i "aevalsrc=0.5*sin(2*PI*1000*t)|0.5*sin(2*PI*1500*t):s=48000:d=1.2345:c=stereo" -c:a pcm_s16le \
    in/odd_length_stereo.wav
ls -l in
