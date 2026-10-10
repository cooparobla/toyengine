#!/usr/bin/env python3
"""Synthesizes the music and sound effects the audio_demo scene plays.

Run from the repo root (needs numpy; see the scratch-venv note in the README):
    python3 tools/gen_audio_demo_assets.py

Writes assets/audio/:
  music/meadow.wav      calm, C major, 92 BPM, 12 bars (~31 s)    -- playlist track 1
  music/expedition.wav  driving, D minor, 116 BPM, 12 bars (~25 s) -- playlist track 2
  music/nocturne.wav    slow night pads + bells, A minor, 68 BPM, 8 bars (~28 s) -- the zone track
  sfx/beacon.wav        1 s loop: a two-tone blip, for the compass speakers (pitched per speaker)
  sfx/drone.wav         2 s loop: a buzzing motor, for the orbiting (doppler) source
  sfx/crickets.wav      3 s loop: high chirps -- easy to localize
  sfx/chimes.wav        4 s loop: wind chimes
  sfx/fire.wav          3 s loop: campfire crackle
  sfx/music_box.wav     4.2 s loop: a little melody, for the directional (cone) speaker
  sfx/ping.wav          one-shot sonar ping, for the "ping from a direction" buttons

Music is stereo IMA-ADPCM at 32 kHz (a quarter of 16-bit PCM's size; miniaudio decodes it);
effects are mono 16-bit PCM at 48 kHz (mono, so they can be spatialized). Every loop is
seamless: note tails past the loop point are wrapped to its start, and reverb runs over two
passes so the second pass (the one kept) starts already reverberating.

Deterministic: all randomness comes from fixed seeds.
"""

import math
import pathlib
import struct

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "audio"

MUSIC_SR = 32000
SFX_SR = 48000


# --- primitives -------------------------------------------------------------------------------

def midi_hz(m):
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


NOTE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def n(name):
    """'C4' / 'F#3' / 'Bb2' -> midi number."""
    base = NOTE[name[0]]
    i = 1
    while i < len(name) and name[i] in "#b":
        base += 1 if name[i] == "#" else -1
        i += 1
    return base + 12 * (int(name[i:]) + 1)


def env_adsr(length, sr, a, d, s, r, hold=None):
    """Attack/decay/sustain/release; `hold` = seconds before release (default: up to the end)."""
    t = np.arange(length) / sr
    if hold is None:
        hold = max(length / sr - r, a)
    e = np.where(t < a, t / max(a, 1e-6), s + (1 - s) * np.exp(-(t - a) / max(d, 1e-6)))
    rel = t > hold
    e[rel] *= np.exp(-(t[rel] - hold) / max(r, 1e-6) * 4.0)
    return e


def pluck(f, dur, sr, bright=1.0, decay=1.6):
    """Additive plucked string / kalimba: harmonics decaying faster the higher they are."""
    length = int(dur * sr)
    t = np.arange(length) / sr
    out = np.zeros(length)
    for k in range(1, 14):
        fk = f * k * (1.0 + 0.0007 * k * k)
        if fk > sr * 0.45:
            break
        amp = (1.0 / k ** (1.6 - 0.5 * bright))
        out += amp * np.sin(2 * np.pi * fk * t) * np.exp(-t * (decay + 1.3 * k / bright))
    out *= np.minimum(t / 0.003, 1.0)
    return out


def saw_partials(f, t, max_hz, detune_cents=(0.0,), vibrato=0.0):
    out = np.zeros_like(t)
    for c in detune_cents:
        fc = f * 2 ** (c / 1200.0)
        phase_mod = vibrato * np.sin(2 * np.pi * 5.1 * t) / 5.1
        k = 1
        while fc * k < max_hz and k < 40:
            out += np.sin(2 * np.pi * fc * k * t + k * phase_mod) / k
            k += 1
    return out / len(detune_cents)


def pad(f, dur, sr, attack=0.6, release=1.2, brightness=2200.0):
    length = int((dur + release) * sr)
    t = np.arange(length) / sr
    tone = saw_partials(f, t, brightness, detune_cents=(-7.0, 0.0, 6.0), vibrato=0.002)
    return tone * env_adsr(length, sr, attack, 1.0, 0.85, release, hold=dur)


def bell(f, dur, sr, decay=1.2):
    length = int(dur * sr)
    t = np.arange(length) / sr
    out = np.zeros(length)
    for ratio, amp, dk in ((1.0, 1.0, 1.0), (2.0, 0.35, 1.6), (2.76, 0.45, 2.2), (5.4, 0.2, 3.5), (8.93, 0.1, 5.0)):
        if f * ratio < sr * 0.45:
            out += amp * np.sin(2 * np.pi * f * ratio * t) * np.exp(-t * decay * dk)
    return out * np.minimum(t / 0.002, 1.0)


def bass(f, dur, sr, drive=1.6):
    length = int(dur * sr)
    t = np.arange(length) / sr
    tone = np.sin(2 * np.pi * f * t) + 0.35 * np.sin(4 * np.pi * f * t) + 0.12 * np.sin(6 * np.pi * f * t)
    tone = np.tanh(tone * drive) / np.tanh(drive)
    return tone * env_adsr(length, sr, 0.008, 0.25, 0.6, 0.08)


def lead(f, dur, sr):
    """Brassy lead: saw partials whose brightness opens and closes with the note."""
    length = int((dur + 0.25) * sr)
    t = np.arange(length) / sr
    out = np.zeros(length)
    bright = 1.0 + 6.0 * np.exp(-t / 0.18)          # harmonics shown, over time
    vib = 0.004 * np.sin(2 * np.pi * 5.5 * t) * np.minimum(t / 0.4, 1.0)
    for k in range(1, 18):
        if f * k > sr * 0.45:
            break
        w = np.clip(bright - k * 0.5, 0.0, 1.0)
        out += w * np.sin(2 * np.pi * f * k * (t + vib)) / k
    return out * env_adsr(length, sr, 0.03, 0.3, 0.7, 0.2, hold=dur)


def one_pole_lp(x, cutoff, sr):
    a = math.exp(-2 * math.pi * cutoff / sr)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):          # short buffers only (drums, effects)
        acc = (1 - a) * x[i] + a * acc
        y[i] = acc
    return y


def kick(sr):
    length = int(0.45 * sr)
    t = np.arange(length) / sr
    f = 45 + 95 * np.exp(-t / 0.045)
    phase = 2 * np.pi * np.cumsum(f) / sr
    return np.sin(phase) * np.exp(-t / 0.16) + 0.3 * np.exp(-t / 0.004)


def snare(sr, rng):
    length = int(0.3 * sr)
    t = np.arange(length) / sr
    noise = rng.standard_normal(length)
    noise = noise - one_pole_lp(noise, 1200, sr)
    tone = np.sin(2 * np.pi * 185 * t) * np.exp(-t / 0.05)
    return (0.6 * noise * np.exp(-t / 0.07) + 0.5 * tone) * 0.8


def hat(sr, rng, open_=False):
    length = int((0.25 if open_ else 0.08) * sr)
    t = np.arange(length) / sr
    noise = rng.standard_normal(length)
    noise = noise - one_pole_lp(noise, 6000, sr)
    return 0.35 * noise * np.exp(-t / (0.09 if open_ else 0.018))


def comb_block(x, delay, g):
    """y[n] = x[n] + g*y[n-delay], vectorized one delay-length block at a time."""
    y = np.copy(x)
    for s in range(delay, len(x), delay):
        e = min(s + delay, len(x))
        y[s:e] += g * y[s - delay:e - delay]
    return y


def allpass_block(x, delay, g):
    y = np.zeros_like(x)
    xd = np.concatenate([np.zeros(delay), x])[:len(x)]
    y[:delay] = -g * x[:delay] + xd[:delay]
    for s in range(delay, len(x), delay):
        e = min(s + delay, len(x))
        y[s:e] = -g * x[s:e] + xd[s:e] + g * y[s - delay:e - delay]
    return y


def reverb(x, sr, decay=0.82, damp_hz=5000, seed_offset=0):
    """Schroeder reverb (4 combs + 2 allpasses), wet signal only."""
    scale = sr / 44100.0
    combs = [1557, 1617, 1491, 1422]
    aps = [225, 556]
    src = one_pole_lp_fast(x, damp_hz, sr)
    wet = np.zeros_like(x)
    for c in combs:
        wet += comb_block(src, int((c + seed_offset) * scale), decay)
    wet /= len(combs)
    for a in aps:
        wet = allpass_block(wet, int((a + seed_offset // 3) * scale), 0.5)
    return wet


def one_pole_lp_fast(x, cutoff, sr):
    """A gentle 3-tap smoothing stand-in for a one-pole lowpass on long buffers."""
    k = max(1, int(sr / cutoff / 2))
    kernel = np.ones(k) / k
    return np.convolve(x, kernel, mode="same")


# --- track assembly ---------------------------------------------------------------------------

class Track:
    def __init__(self, bpm, bars, sr=MUSIC_SR, tail=6.0):
        self.sr = sr
        self.beat = 60.0 / bpm
        self.bar = self.beat * 4
        # A few samples off the exact bar length, so it fills whole ADPCM blocks: decoders play a
        # padded last block as silence, which would gap a looping track.
        self.length, self.block_samples = adpcm_friendly_length(int(round(bars * self.bar * sr)))
        self.buf = np.zeros((2, self.length + int(tail * sr)))
        self.send = np.zeros((2, self.length + int(tail * sr)))   # reverb send

    def at(self, bar, beat=0.0):
        return int(round((bar * 4 + beat) * self.beat * self.sr))

    def add(self, sig, start, gain=1.0, pan=0.0, rev=0.25):
        """Equal-power pan in [-1, 1]; rev = reverb send level."""
        e = min(start + len(sig), self.buf.shape[1])
        sig = sig[:e - start] * gain
        l = math.cos((pan + 1) * math.pi / 4)
        r = math.sin((pan + 1) * math.pi / 4)
        self.buf[0, start:e] += sig * l
        self.buf[1, start:e] += sig * r
        self.send[0, start:e] += sig * l * rev
        self.send[1, start:e] += sig * r * rev

    def render(self, peak=0.7, decay=0.82):
        L = self.length
        out = np.zeros((2, L))
        for ch in range(2):
            dry = self.buf[ch, :L].copy()
            send = self.send[ch, :L].copy()
            tail = self.buf.shape[1] - L
            dry[:tail] += self.buf[ch, L:]          # wrap note tails -> seamless loop
            send[:tail] += self.send[ch, L:]
            # Reverb over two passes; keep the second so the loop start is already reverberant.
            wet = reverb(np.concatenate([send, send]), self.sr, decay=decay, seed_offset=ch * 23)[L:]
            out[ch] = dry + wet * 0.9
        out /= max(np.abs(out).max(), 1e-9)
        self.audio = out * peak
        return self


def adpcm_friendly_length(frames):
    """Nearest length to `frames` that a whole number of stereo IMA blocks holds, and that
    block's samples-per-channel (8k + 1 for a block of 8k + 8 bytes)."""
    for d in range(0, 4096):
        for cand in (frames + d, frames - d):
            for k in range(255, 31, -1):
                if cand % (8 * k + 1) == 0:
                    return cand, 8 * k + 1
    raise ValueError("no ADPCM-friendly length")


def chord_tones(root, quality):
    iv = {"maj": (0, 4, 7), "min": (0, 3, 7), "maj7": (0, 4, 7, 11), "min7": (0, 3, 7, 10),
          "sus4": (0, 5, 7), "add9": (0, 4, 7, 14), "min9": (0, 3, 7, 14)}[quality]
    return [root + i for i in iv]


def meadow():
    tr = Track(bpm=92, bars=12)
    sr = tr.sr
    prog = [("C3", "add9"), ("G2", "maj"), ("A2", "min7"), ("F2", "maj7"),
            ("C3", "maj"), ("G2", "maj"), ("F2", "maj7"), ("G2", "sus4"),
            ("A2", "min7"), ("F2", "maj7"), ("C3", "add9"), ("G2", "maj")]
    rng = np.random.default_rng(1)
    for bar, (root, q) in enumerate(prog):
        r = n(root)
        tones = chord_tones(r + 12, q)
        for i, m in enumerate(tones):
            tr.add(pad(midi_hz(m), tr.bar * 0.98, sr, attack=0.5, brightness=1800), tr.at(bar), 0.10, pan=-0.5 + i * 0.33, rev=0.5)
        tr.add(bass(midi_hz(r - 12 if r > n("E2") else r), tr.beat * 1.8, sr, drive=1.2), tr.at(bar), 0.32, rev=0.05)
        tr.add(bass(midi_hz(r - 12 if r > n("E2") else r), tr.beat * 1.5, sr, drive=1.2), tr.at(bar, 2.5), 0.22, rev=0.05)
        # Eighth-note arpeggio, up then down through the chord, an octave up.
        arp = tones + [tones[0] + 12] + tones[::-1][1:3]
        for k in range(8):
            m = arp[k % len(arp)] + 12
            tr.add(pluck(midi_hz(m), 1.4, sr, bright=1.2), tr.at(bar, k * 0.5), 0.16 + 0.04 * (k % 2 == 0), pan=0.35 * math.sin(k * 0.8), rev=0.35)
        if bar >= 4:     # shaker on the off-beats from bar 5
            for k in range(4):
                tr.add(hat(sr, rng) * 0.6, tr.at(bar, k + 0.5), 0.5, pan=0.4, rev=0.1)
    # Bell melody over the second half (C major pentatonic).
    melody = [("E5", 0, 1.5), ("G5", 1.5, 0.5), ("A5", 2, 2), ("G5", 4, 1), ("E5", 5, 1), ("D5", 6, 2),
              ("C5", 8, 1.5), ("D5", 9.5, 0.5), ("E5", 10, 1), ("G5", 11, 1), ("A5", 12, 3), ("G5", 15, 1),
              ("E5", 16, 1.5), ("D5", 17.5, 0.5), ("C5", 18, 2), ("D5", 20, 1), ("E5", 21, 1), ("G5", 22, 2),
              ("A5", 24, 1), ("G5", 25, 1), ("E5", 26, 2), ("D5", 28, 1), ("C5", 29, 3)]
    for name, beat, dur in melody:
        tr.add(bell(midi_hz(n(name)), 2.5, sr, decay=0.9), tr.at(4, beat), 0.13, pan=-0.2, rev=0.5)
    return tr.render(peak=0.72)


def expedition():
    tr = Track(bpm=116, bars=12)
    sr = tr.sr
    rng = np.random.default_rng(2)
    prog = [("D2", "min"), ("Bb1", "maj"), ("F2", "maj"), ("C2", "maj")] * 3
    k_s, s_s = kick(sr), snare(sr, rng)
    for bar, (root, q) in enumerate(prog):
        r = n(root)
        tones = chord_tones(r + 24, q)
        for i, m in enumerate(tones):
            tr.add(pad(midi_hz(m), tr.bar * 0.95, sr, attack=0.15, release=0.4, brightness=3000), tr.at(bar), 0.07, pan=-0.6 + i * 0.6, rev=0.35)
        for k in range(8):      # driving eighth-note bass, octave jump on the off-beats
            m = r + (12 if k % 2 else 0)
            tr.add(bass(midi_hz(m), tr.beat * 0.45, sr, drive=2.2), tr.at(bar, k * 0.5), 0.26 if k % 2 == 0 else 0.18, rev=0.03)
        for k in range(16):     # 16th pluck ostinato
            m = tones[[0, 1, 2, 1][k % 4]] + 12
            tr.add(pluck(midi_hz(m), 0.6, sr, bright=1.6, decay=4.0), tr.at(bar, k * 0.25), 0.08, pan=0.5, rev=0.2)
        for beat in (0, 2, 2.5 if bar % 2 else 3.5):
            tr.add(k_s, tr.at(bar, beat), 0.55, rev=0.05)
        for beat in (1, 3):
            tr.add(s_s, tr.at(bar, beat), 0.38, pan=0.05, rev=0.25)
        for k in range(8):
            tr.add(hat(sr, rng, open_=(k == 7 and bar % 4 == 3)), tr.at(bar, k * 0.5), 0.55, pan=-0.3, rev=0.08)
    # Lead melody from bar 5.
    melody = [("A4", 0, 1.5), ("D5", 1.5, 0.5), ("F5", 2, 1), ("E5", 3, 1), ("D5", 4, 2), ("C5", 6, 1), ("A4", 7, 1),
              ("F4", 8, 1.5), ("A4", 9.5, 0.5), ("C5", 10, 1), ("D5", 11, 1), ("E5", 12, 3), ("G4", 15, 1),
              ("A4", 16, 1.5), ("D5", 17.5, 0.5), ("F5", 18, 1), ("G5", 19, 1), ("A5", 20, 2), ("G5", 22, 1), ("F5", 23, 1),
              ("E5", 24, 1.5), ("D5", 25.5, 0.5), ("C5", 26, 1), ("E5", 27, 1), ("D5", 28, 4)]
    for name, beat, dur in melody:
        tr.add(lead(midi_hz(n(name)), dur * tr.beat * 0.92, sr), tr.at(4, beat), 0.14, pan=0.1, rev=0.3)
    return tr.render(peak=0.75, decay=0.78)


def nocturne():
    tr = Track(bpm=68, bars=8, tail=9.0)
    sr = tr.sr
    rng = np.random.default_rng(3)
    prog = [("A2", "min9"), ("F2", "maj7"), ("D2", "min9"), ("E2", "sus4"),
            ("A2", "min9"), ("F2", "maj7"), ("D2", "min9"), ("E2", "maj")]
    for bar, (root, q) in enumerate(prog):
        r = n(root)
        for i, m in enumerate(chord_tones(r + 12, q)):
            tr.add(pad(midi_hz(m), tr.bar, sr, attack=1.4, release=2.5, brightness=1200), tr.at(bar), 0.11, pan=-0.6 + i * 0.4, rev=0.7)
        tr.add(pad(midi_hz(r - 12), tr.bar, sr, attack=0.8, release=1.5, brightness=300), tr.at(bar), 0.22, rev=0.2)
    melody = [("E5", 0, 2), ("C5", 2, 1), ("B4", 3, 1), ("A4", 4, 3), ("C5", 7, 1), ("D5", 8, 2), ("E5", 10, 2),
              ("G5", 12, 1.5), ("F5", 13.5, 0.5), ("E5", 14, 2), ("E5", 16, 2), ("A5", 18, 2), ("G5", 20, 1),
              ("E5", 21, 1), ("D5", 22, 2), ("C5", 24, 1), ("B4", 25, 1), ("C5", 26, 1), ("D5", 27, 1), ("B4", 28, 4)]
    for name, beat, dur in melody:
        tr.add(bell(midi_hz(n(name)), 4.0, sr, decay=0.55), tr.at(0, beat), 0.15, pan=0.3 * math.sin(beat), rev=0.8)
    # Soft wind: slowly swelling filtered noise.
    L = tr.length
    t = np.arange(L) / sr
    wind = one_pole_lp_fast(rng.standard_normal(L), 600, sr)
    wind *= 0.5 + 0.5 * np.sin(2 * np.pi * t / (L / sr) * 2)   # 2 swells per loop: seamless
    tr.add(wind, 0, 0.05, pan=-0.3, rev=0.4)
    return tr.render(peak=0.68, decay=0.86)


# --- effects (mono) ---------------------------------------------------------------------------

def wrap_loop(sig, length):
    out = np.zeros(length)
    for s in range(0, len(sig), length):
        chunk = sig[s:s + length]
        out[:len(chunk)] += chunk
    return out


def norm(x, peak):
    return x / max(np.abs(x).max(), 1e-9) * peak


def beacon():
    sr = SFX_SR
    t = np.arange(int(0.25 * sr)) / sr
    def blip(f):
        return (np.sin(2 * np.pi * f * t) + 0.3 * np.sin(4 * np.pi * f * t) + 0.12 * np.sin(6 * np.pi * f * t)) * np.exp(-t / 0.05) * np.minimum(t / 0.002, 1)
    out = np.zeros(sr)
    out[:len(t)] += blip(880)
    s = int(0.12 * sr)
    out[s:s + len(t)] += blip(1320)
    return norm(out, 0.8)


def drone():
    sr = SFX_SR
    t = np.arange(2 * sr) / sr
    tone = saw_partials(110.0, t, 6000, detune_cents=(0.0,)) + 0.5 * saw_partials(220.0, t, 6000, detune_cents=(0.0,))
    tone *= 0.75 + 0.25 * np.sin(2 * np.pi * 4.0 * t)       # 8 wobbles per loop
    return norm(np.tanh(tone * 1.5), 0.7)


def crickets():
    sr = SFX_SR
    L = 3 * sr
    rng = np.random.default_rng(4)
    out = np.zeros(L + sr)
    for start in (0.1, 0.9, 1.55, 2.3):
        s = int(start * sr)
        for p in range(int(rng.integers(3, 6))):
            t = np.arange(int(0.03 * sr)) / sr
            f = 4400 + 150 * rng.standard_normal()
            pulse = np.sin(2 * np.pi * f * t) * np.sin(np.pi * t / t[-1]) ** 2
            o = s + int(p * 0.045 * sr)
            out[o:o + len(pulse)] += pulse
    return norm(wrap_loop(out, L), 0.6)


def chimes():
    sr = SFX_SR
    L = 4 * sr
    rng = np.random.default_rng(5)
    out = np.zeros(L + 4 * sr)
    notes = [n(x) for x in ("C6", "D6", "E6", "G6", "A6", "C7")]
    for i in range(9):
        s = int(i * L / 9 + rng.uniform(0, 0.15) * sr)
        sig = bell(midi_hz(notes[int(rng.integers(len(notes)))]), 3.5, sr, decay=0.9)
        out[s:s + len(sig)] += sig * rng.uniform(0.5, 1.0)
    return norm(wrap_loop(out, L), 0.6)


def fire():
    sr = SFX_SR
    L = 3 * sr
    rng = np.random.default_rng(6)
    bed = one_pole_lp_fast(rng.standard_normal(L), 900, sr) * 0.35
    out = bed.copy()
    for _ in range(70):
        s = int(rng.integers(0, L - 2000))
        k = int(rng.integers(80, 900))
        click = rng.standard_normal(k) * np.exp(-np.arange(k) / (k / 5.0))
        out[s:s + k] += click * rng.uniform(0.3, 1.2)
    return norm(out, 0.7)


def music_box():
    sr = SFX_SR
    beat = 0.35
    seq = ["E6", "G6", "C7", "B6", "G6", "E6", "D6", "G6", "F6", "D6", "B5", "C6"]
    L = int(len(seq) * beat * sr)
    out = np.zeros(L + 3 * sr)
    for i, name in enumerate(seq):
        s = int(i * beat * sr)
        sig = pluck(midi_hz(n(name)), 2.0, sr, bright=2.0, decay=2.5) + 0.4 * bell(midi_hz(n(name)), 2.0, sr, decay=1.5)
        out[s:s + len(sig)] += sig
    return norm(wrap_loop(out, L), 0.7)


def ping():
    sr = SFX_SR
    t = np.arange(int(1.2 * sr)) / sr
    f = 1500 * (1 + 0.04 * np.exp(-t / 0.05))
    sig = np.sin(2 * np.pi * np.cumsum(f) / sr) * np.exp(-t / 0.25) * np.minimum(t / 0.002, 1)
    sig += 0.25 * np.sin(2 * np.pi * 3000 * t) * np.exp(-t / 0.06)
    return norm(sig, 0.8)


# --- file writers -----------------------------------------------------------------------------

def write_pcm16(path, mono, sr):
    data = (np.clip(mono, -1, 1) * 32767).astype("<i2").tobytes()
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, sr, sr * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


IMA_STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
             107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
             876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
             4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
             22385, 24623, 27086, 29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


class ImaState:
    def __init__(self):
        self.pred = 0
        self.index = 0

    def encode(self, sample):
        step = IMA_STEPS[self.index]
        diff = sample - self.pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            code |= 4; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            code |= 2; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            code |= 1; delta += step
        self.pred += -delta if code & 8 else delta
        self.pred = max(-32768, min(32767, self.pred))
        self.index = max(0, min(88, self.index + IMA_INDEX[code & 7]))
        return code


def write_ima_adpcm(path, stereo, sr, spb):
    """Stereo IMA-ADPCM WAV (format 0x11), as dr_wav / miniaudio read it. `spb` = samples per
    block per channel (8k + 1); the length should be a whole number of blocks."""
    ch = 2
    block_align = (spb - 1) * 4 * ch // 8 + 4 * ch
    pcm = (np.clip(stereo, -1, 1) * 32767).astype(np.int32)
    frames = pcm.shape[1]
    nblocks = (frames + spb - 1) // spb
    padded = np.zeros((2, nblocks * spb), dtype=np.int32)
    padded[:, :frames] = pcm
    states = [ImaState(), ImaState()]
    out = bytearray()
    for b in range(nblocks):
        base = b * spb
        for c in range(ch):
            st = states[c]
            st.pred = int(padded[c, base])
            out += struct.pack("<hBB", st.pred, st.index, 0)
        # Then 8 samples (4 bytes) per channel, interleaved channel by channel.
        codes = [[states[c].encode(int(s)) for s in padded[c, base + 1:base + spb]] for c in range(ch)]
        for g in range(0, spb - 1, 8):
            for c in range(ch):
                cs = codes[c][g:g + 8]
                for j in range(0, 8, 2):
                    out.append((cs[j] & 0xF) | ((cs[j + 1] & 0xF) << 4))
    path.parent.mkdir(parents=True, exist_ok=True)
    byte_rate = sr * block_align // spb
    fmt = struct.pack("<HHIIHHHH", 0x11, ch, sr, byte_rate, block_align, 4, 2, spb)
    with open(path, "wb") as f:
        body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
        body += b"fact" + struct.pack("<II", 4, frames)
        body += b"data" + struct.pack("<I", len(out)) + bytes(out)
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def main():
    for name, fn in (("meadow", meadow), ("expedition", expedition), ("nocturne", nocturne)):
        tr = fn()
        audio = tr.audio
        p = OUT / "music" / f"{name}.wav"
        write_ima_adpcm(p, audio, MUSIC_SR, tr.block_samples)
        print(f"{p.relative_to(ROOT)}  {audio.shape[1] / MUSIC_SR:.1f} s  {p.stat().st_size // 1024} KB")
    for name, fn in (("beacon", beacon), ("drone", drone), ("crickets", crickets), ("chimes", chimes),
                     ("fire", fire), ("music_box", music_box), ("ping", ping)):
        sig = fn()
        p = OUT / "sfx" / f"{name}.wav"
        write_pcm16(p, sig, SFX_SR)
        print(f"{p.relative_to(ROOT)}  {len(sig) / SFX_SR:.2f} s  {p.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
