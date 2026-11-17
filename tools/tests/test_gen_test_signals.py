"""Tests for tools/gen_test_signals.py: the ground truth in manifest.json must match the audio."""

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import gen_test_signals as gen  # noqa: E402


class GeneratedSignalsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = Path(tempfile.mkdtemp(prefix="ddj_signals_"))
        assert gen.main(["--out", str(cls.out)]) == 0
        cls.manifest = json.loads((cls.out / "manifest.json").read_text(encoding="utf-8"))
        cls.by_file = {e["file"]: e for e in cls.manifest["files"]}

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def read(self, rel):
        audio, sr = sf.read(self.out / rel, always_2d=True)
        return audio[:, 0], sr

    def test_expected_files_exist(self):
        files = set(self.by_file)
        for bpm in gen.CLICK_BPMS:
            self.assertIn(f"clicks/click_{bpm}bpm.wav", files)
        self.assertEqual(len([f for f in files if f.startswith("keys/")]), 24)
        self.assertTrue(any(f.startswith("sweeps/") for f in files))
        self.assertTrue(any(f.startswith("padded/") for f in files))
        for rel, entry in self.by_file.items():
            info = sf.info(self.out / rel)
            self.assertEqual(info.samplerate, gen.DEFAULT_SAMPLE_RATE, rel)
            self.assertEqual(info.channels, 2, rel)
            self.assertEqual(info.frames, entry["frames"], rel)

    def test_click_onsets_match_manifest(self):
        for bpm in gen.CLICK_BPMS:
            entry = self.by_file[f"clicks/click_{bpm}bpm.wav"]
            audio, sr = self.read(entry["file"])
            onsets = self.onsets(audio)
            with self.subTest(bpm=bpm):
                self.assertEqual(len(onsets), entry["props"]["beats"])
                self.assertLessEqual(abs(onsets[0] - entry["props"]["first_beat_frame"]), 1)
                intervals = np.diff(onsets) / sr
                self.assertLess(np.max(np.abs(intervals - 60.0 / bpm)), 1.5 / sr)

    def test_downbeats_are_accented_every_four_beats(self):
        audio, sr = self.read("clicks/click_128bpm.wav")
        onsets = self.onsets(audio)
        peaks = [np.max(np.abs(audio[o:o + int(0.01 * sr)])) for o in onsets[:12]]
        for i, peak in enumerate(peaks):
            if i % 4 == 0:
                self.assertGreater(peak, 0.7, f"beat {i} should be a downbeat")
            else:
                self.assertLess(peak, 0.55, f"beat {i} should not be a downbeat")

    def test_padded_click_has_known_silence_and_first_beat(self):
        entry = self.by_file["padded/click_128bpm_padded.wav"]
        audio, sr = self.read(entry["file"])
        lead = int(entry["props"]["leading_silence_sec"] * sr)
        trail = int(entry["props"]["trailing_silence_sec"] * sr)
        self.assertEqual(np.max(np.abs(audio[:lead])), 0.0)
        self.assertEqual(np.max(np.abs(audio[-trail:])), 0.0)
        self.assertLessEqual(abs(self.onsets(audio)[0] - entry["props"]["first_beat_frame"]), 1)

    def test_silence_is_silent(self):
        audio, _ = self.read("padded/silence_5s.wav")
        self.assertEqual(np.max(np.abs(audio)), 0.0)

    def test_sine_frequency_and_level(self):
        for rel, entry in self.by_file.items():
            if entry["kind"] != "sine" or rel.startswith("padded/"):
                continue
            audio, sr = self.read(rel)
            with self.subTest(file=rel):
                expected_peak = 10 ** (entry["props"]["level_dbfs"] / 20)
                self.assertAlmostEqual(np.max(np.abs(audio)), expected_peak, delta=0.002)
                middle = audio[sr:2 * sr]  # skip the fade-in
                spectrum = np.abs(np.fft.rfft(middle * np.hanning(len(middle))))
                peak_hz = np.argmax(spectrum) * sr / len(middle)
                self.assertAlmostEqual(peak_hz, entry["props"]["frequency_hz"], delta=1.0)

    def test_key_loops_have_the_tonic_as_strongest_pitch_class(self):
        for rel, entry in self.by_file.items():
            if entry["kind"] != "key":
                continue
            audio, sr = self.read(rel)
            with self.subTest(file=rel):
                chroma = self.chroma(audio, sr)
                self.assertEqual(int(np.argmax(chroma)), entry["props"]["key_index"] % 12)

    def test_camelot_codes_match_the_spec(self):
        # MATCHER_SPEC §13 examples.
        expected = {0: "8B", 21: "8A", 7: "9B", 16: "9A", 5: "7B", 14: "7A"}
        for key_index, code in expected.items():
            self.assertEqual(gen.camelot(key_index), code)
        # Every Camelot code appears exactly once.
        self.assertEqual(len({gen.camelot(k) for k in range(24)}), 24)

    def test_nothing_clips(self):
        for rel in self.by_file:
            audio, _ = self.read(rel)
            self.assertLess(np.max(np.abs(audio)), 1.0, rel)

    def test_output_is_deterministic(self):
        other = Path(tempfile.mkdtemp(prefix="ddj_signals_again_"))
        try:
            self.assertEqual(gen.main(["--out", str(other)]), 0)
            for rel in self.by_file:
                self.assertEqual((self.out / rel).read_bytes(), (other / rel).read_bytes(), rel)
        finally:
            shutil.rmtree(other, ignore_errors=True)

    def test_second_run_is_skipped_when_up_to_date(self):
        self.assertTrue(gen.up_to_date(self.out, gen.DEFAULT_SAMPLE_RATE))
        self.assertFalse(gen.up_to_date(self.out, 48_000))

    # ---- helpers ----

    @staticmethod
    def onsets(audio, threshold=0.05, min_gap=0.1, sr=gen.DEFAULT_SAMPLE_RATE):
        above = np.flatnonzero(np.abs(audio) > threshold)
        result = []
        for i in above:
            if not result or i - result[-1] > min_gap * sr:
                result.append(int(i))
        # The first sample above the threshold lags the true start by a few samples (sine starts at 0);
        # walk back to the first non-zero sample.
        return [GeneratedSignalsTests.walk_back(audio, o) for o in result]

    @staticmethod
    def walk_back(audio, i):
        while i > 0 and audio[i - 1] != 0.0:
            i -= 1
        return i

    @staticmethod
    def chroma(audio, sr):
        n = 8192
        bins = np.zeros(12)
        freqs = np.fft.rfftfreq(n, 1 / sr)
        valid = (freqs > 60) & (freqs < 2000)
        pitch_class = (np.round(12 * np.log2(freqs[valid] / 440.0)) + 9).astype(int) % 12
        for start in range(0, len(audio) - n, n // 2):
            spectrum = np.abs(np.fft.rfft(audio[start:start + n] * np.hanning(n))) ** 2
            np.add.at(bins, pitch_class, spectrum[valid])
        return bins


if __name__ == "__main__":
    unittest.main()
