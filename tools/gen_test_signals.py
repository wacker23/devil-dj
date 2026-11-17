#!/usr/bin/env python3
"""Generate the synthetic audio test signals for Devil-DJ (task P0-13).

Writes WAV files with known ground truth into tests/data/generated/ (git-ignored) plus manifest.json
describing every file. Tests never use real music; they use these.

  clicks/   click tracks at 90/120/128/140/174 BPM, known first beat, accented downbeats (4/4)
  tones/    sine tones at known frequency and level (key lock, loudness)
  sweeps/   logarithmic sine sweep 20 Hz - 20 kHz
  keys/     I-IV-V-I chord loops in all 24 keys (key index 0..23 as in ARCHITECTURE §7, Camelot code)
  padded/   files with leading/trailing silence, and pure silence

Deterministic: the same version of this script always writes identical files. Skips work when the
manifest says the files are already up to date (use --force to regenerate).

Requires: numpy, soundfile (dev tools only, not shipped with the app).
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np
import soundfile as sf

GENERATOR_VERSION = 1
DEFAULT_SAMPLE_RATE = 44_100
DEFAULT_OUT = Path(__file__).resolve().parent.parent / "tests" / "data" / "generated"

CLICK_BPMS = [90, 120, 128, 140, 174]
CLICK_FIRST_BEAT_SEC = 0.5      # deliberately not 0, so first-beat detection is tested
CLICK_DURATION_SEC = 30.0
CLICK_LENGTH_SEC = 0.010        # 10 ms decaying sine burst
CLICK_HZ, DOWNBEAT_HZ = 1_000.0, 2_000.0
CLICK_LEVEL, DOWNBEAT_LEVEL = 0.5, 0.8
BEATS_PER_BAR = 4

PITCH_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
FILE_PITCH_NAMES = ["C", "Cs", "D", "Ds", "E", "F", "Fs", "G", "Gs", "A", "As", "B"]  # no '#' in file names
CHORD_SEC = 1.0                 # one chord per second (= 2 beats at 120 BPM)
KEY_LOOP_REPEATS = 2            # I-IV-V-I x 2 = 8 s


@dataclass
class Entry:
    file: str                   # relative to the output folder, forward slashes
    kind: str
    sample_rate: int
    channels: int
    duration_sec: float
    frames: int
    props: dict = field(default_factory=dict)


# ---- helpers ----

def db_to_gain(db: float) -> float:
    return 10.0 ** (db / 20.0)


def camelot(key_index: int) -> str:
    """Camelot code for key index 0..23 (0-11 major C..B, 12-23 minor C..B), MATCHER_SPEC §4.2."""
    minor = key_index >= 12
    p = key_index % 12
    number = (((7 * p) % 12 + (4 if minor else 7)) % 12) + 1
    return f"{number}{'A' if minor else 'B'}"


def midi_to_hz(note: float) -> float:
    return 440.0 * 2.0 ** ((note - 69.0) / 12.0)


def write(out: Path, rel: str, audio: np.ndarray, sr: int, kind: str, props: dict) -> Entry:
    path = out / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    if audio.ndim == 1:
        audio = np.stack([audio, audio], axis=1)  # stereo, both channels identical
    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    assert peak <= 1.0, f"{rel} clips (peak {peak})"
    sf.write(path, audio.astype(np.float32), sr, subtype="PCM_16")
    return Entry(rel, kind, sr, audio.shape[1], round(audio.shape[0] / sr, 6), int(audio.shape[0]), props)


# ---- signals ----

def click_track(sr: int, bpm: float, duration: float, first_beat: float) -> tuple[np.ndarray, list[int]]:
    n = int(round(duration * sr))
    audio = np.zeros(n)
    t = np.arange(int(round(CLICK_LENGTH_SEC * sr))) / sr
    envelope = np.exp(-t / (CLICK_LENGTH_SEC / 5.0))
    onsets = []
    beat = 0
    while True:
        start = int(round((first_beat + beat * 60.0 / bpm) * sr))
        if start + len(t) > n:
            break
        downbeat = beat % BEATS_PER_BAR == 0
        hz, level = (DOWNBEAT_HZ, DOWNBEAT_LEVEL) if downbeat else (CLICK_HZ, CLICK_LEVEL)
        audio[start:start + len(t)] += level * envelope * np.sin(2 * np.pi * hz * t)
        onsets.append(start)
        beat += 1
    return audio, onsets


def sine(sr: int, hz: float, duration: float, level_dbfs: float) -> np.ndarray:
    t = np.arange(int(round(duration * sr))) / sr
    audio = db_to_gain(level_dbfs) * np.sin(2 * np.pi * hz * t)
    fade = int(0.005 * sr)  # 5 ms fades so there are no clicks at the ends
    ramp = np.linspace(0.0, 1.0, fade)
    audio[:fade] *= ramp
    audio[-fade:] *= ramp[::-1]
    return audio


def log_sweep(sr: int, f0: float, f1: float, duration: float, level_dbfs: float) -> np.ndarray:
    t = np.arange(int(round(duration * sr))) / sr
    k = np.log(f1 / f0)
    phase = 2 * np.pi * f0 * duration / k * (np.exp(t / duration * k) - 1.0)
    audio = db_to_gain(level_dbfs) * np.sin(phase)
    fade = int(0.01 * sr)
    ramp = np.linspace(0.0, 1.0, fade)
    audio[:fade] *= ramp
    audio[-fade:] *= ramp[::-1]
    return audio


def harmonic_note(sr: int, midi: float, duration: float) -> np.ndarray:
    """A soft organ-like tone: fundamental + decaying harmonics, attack/release ramps."""
    t = np.arange(int(round(duration * sr))) / sr
    hz = midi_to_hz(midi)
    audio = np.zeros_like(t)
    for h, amp in enumerate([1.0, 0.5, 0.25, 0.12], start=1):
        if hz * h < sr / 2:
            audio += amp * np.sin(2 * np.pi * hz * h * t)
    env = np.ones_like(t)
    a, r = int(0.01 * sr), int(0.05 * sr)
    env[:a] = np.linspace(0, 1, a)
    env[-r:] = np.linspace(1, 0, r)
    return audio * env


def chord(sr: int, root_midi: int, intervals: list[int], duration: float) -> np.ndarray:
    notes = [root_midi - 12] + [root_midi + i for i in intervals]  # bass an octave below + triad
    audio = sum(harmonic_note(sr, n, duration) for n in notes)
    return audio / len(notes)


def key_loop(sr: int, key_index: int) -> tuple[np.ndarray, list[str]]:
    """I-IV-V-I (major) or i-iv-V-i with harmonic-minor V (minor), repeated."""
    minor = key_index >= 12
    tonic = 60 + key_index % 12  # middle-C octave
    third = 3 if minor else 4
    progression = [
        (0, [0, third, 7]),
        (5, [0, third, 7]),          # IV / iv
        (7, [0, 4, 7]),              # V is major in both (harmonic minor)
        (0, [0, third, 7]),
    ]
    roman = ["i", "iv", "V", "i"] if minor else ["I", "IV", "V", "I"]
    bar = np.concatenate([chord(sr, tonic + offset, iv, CHORD_SEC) for offset, iv in progression])
    audio = np.tile(bar, KEY_LOOP_REPEATS)
    return 0.7 * audio / np.max(np.abs(audio)), roman


# ---- main ----

def generate(out: Path, sr: int) -> list[Entry]:
    entries: list[Entry] = []

    for bpm in CLICK_BPMS:
        audio, onsets = click_track(sr, bpm, CLICK_DURATION_SEC, CLICK_FIRST_BEAT_SEC)
        entries.append(write(out, f"clicks/click_{bpm}bpm.wav", audio, sr, "click", {
            "bpm": bpm, "first_beat_sec": CLICK_FIRST_BEAT_SEC, "first_beat_frame": onsets[0],
            "beats": len(onsets), "beats_per_bar": BEATS_PER_BAR, "downbeat_every": BEATS_PER_BAR,
            "click_hz": CLICK_HZ, "downbeat_hz": DOWNBEAT_HZ}))

    for hz, level, dur in [(440.0, -6.0, 10.0), (1_000.0, -20.0, 10.0), (1_000.0, -6.0, 10.0)]:
        name = f"tones/sine_{int(hz)}hz_minus{int(-level)}dbfs.wav"
        entries.append(write(out, name, sine(sr, hz, dur, level), sr, "sine",
                             {"frequency_hz": hz, "level_dbfs": level}))

    entries.append(write(out, "sweeps/log_sweep_20_20000hz_10s.wav", log_sweep(sr, 20.0, 20_000.0, 10.0, -6.0),
                         sr, "sweep", {"start_hz": 20.0, "end_hz": 20_000.0, "level_dbfs": -6.0, "law": "log"}))

    for key_index in range(24):
        minor = key_index >= 12
        p = key_index % 12
        audio, roman = key_loop(sr, key_index)
        mode = "minor" if minor else "major"
        entries.append(write(out, f"keys/key_{key_index:02d}_{FILE_PITCH_NAMES[p]}_{mode}.wav", audio, sr, "key", {
            "key_index": key_index, "key_name": f"{PITCH_NAMES[p]} {mode}", "camelot": camelot(key_index),
            "progression": roman, "chord_sec": CHORD_SEC}))

    # Silence-padded variants: known amounts of silence before/after real content.
    pad_before, pad_after = 2.0, 3.0
    click, onsets = click_track(sr, 128, 10.0, CLICK_FIRST_BEAT_SEC)
    padded = np.concatenate([np.zeros(int(pad_before * sr)), click, np.zeros(int(pad_after * sr))])
    entries.append(write(out, "padded/click_128bpm_padded.wav", padded, sr, "click", {
        "bpm": 128, "first_beat_sec": pad_before + CLICK_FIRST_BEAT_SEC,
        "first_beat_frame": int(pad_before * sr) + onsets[0], "beats": len(onsets), "beats_per_bar": BEATS_PER_BAR,
        "leading_silence_sec": pad_before, "trailing_silence_sec": pad_after}))

    tone = sine(sr, 440.0, 5.0, -6.0)
    padded = np.concatenate([np.zeros(int(pad_before * sr)), tone, np.zeros(int(pad_after * sr))])
    entries.append(write(out, "padded/sine_440hz_padded.wav", padded, sr, "sine", {
        "frequency_hz": 440.0, "level_dbfs": -6.0, "leading_silence_sec": pad_before,
        "trailing_silence_sec": pad_after}))

    entries.append(write(out, "padded/silence_5s.wav", np.zeros(int(5.0 * sr)), sr, "silence", {}))
    return entries


def up_to_date(out: Path, sr: int) -> bool:
    manifest = out / "manifest.json"
    if not manifest.is_file():
        return False
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return False
    if data.get("generator_version") != GENERATOR_VERSION or data.get("sample_rate") != sr:
        return False
    return all((out / e["file"]).is_file() for e in data.get("files", []))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate Devil-DJ test signals.")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT, help=f"output folder (default {DEFAULT_OUT})")
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--force", action="store_true", help="regenerate even if up to date")
    args = parser.parse_args(argv)

    if not args.force and up_to_date(args.out, args.sample_rate):
        print(f"gen_test_signals: up to date ({args.out})")
        return 0

    args.out.mkdir(parents=True, exist_ok=True)
    entries = generate(args.out, args.sample_rate)
    manifest = {
        "generator_version": GENERATOR_VERSION,
        "sample_rate": args.sample_rate,
        "format": "WAV PCM 16-bit stereo (both channels identical)",
        "files": [asdict(e) for e in entries],
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    total = sum(e.frames for e in entries) / args.sample_rate
    print(f"gen_test_signals: wrote {len(entries)} files ({total:.0f} s of audio) to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
