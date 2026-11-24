#!/usr/bin/env python3
"""Beat This! (CPJKU, MIT code and weights) -> ONNX, plus reference data for Devil-DJ's C++ port (P6-02).

Dev-only (never shipped): needs the venv in build/venv-beatthis (torch, torchaudio, beat-this, onnx, onnxruntime).
Run from the repository root:

    build\\venv-beatthis\\Scripts\\python.exe tools\\beatthis\\export_onnx.py [checkpoint]   (default final0)

Writes
  build/models/beat_this_<checkpoint>.onnx      the model: spectrogram frames [1, T, 128] -> beat, downbeat logits [1, T]
  tests/data/beatthis/clip.wav                  8 s of our own synthetic music at 22050 Hz (16-bit mono)
  tests/data/beatthis/mel_fbank.f32             the mel filterbank torchaudio uses (513 x 128, float32 LE)
  tests/data/beatthis/spect.f32                 the front-end's output for the clip (frames x 128, float32 LE)
  tests/data/beatthis/logits.f32                the model's logits for it (frames x 2: beat, downbeat), chunked as
                                                the original's inference does (1500 frames, borders of 6, keep_first)
  tests/data/beatthis/long_logits.f32           the same for 70 s of the clip's first 16 beats end to end (chunking)
  tests/data/beatthis/beats.txt                 the post-processed beats and downbeats (seconds), one per line
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import onnx
import onnxruntime
import soundfile
import torch
import torchaudio

from beat_this.inference import load_model, split_predict_aggregate
from beat_this.model.postprocessor import Postprocessor
from beat_this.preprocessing import LogMelSpect

ROOT = Path(__file__).resolve().parents[2]
SR = 22050


def clip() -> np.ndarray:
    """8 s, 124 BPM: a kick on every beat, a snare-like noise burst on 2 and 4, closed hats on the eighths and a bass
    note per bar - our own, generated here (seeded noise, so the same every time)."""
    rng = np.random.default_rng(124)
    n = 8 * SR
    out = np.zeros(n)
    beat = 60.0 / 124.0
    t = np.arange(n) / SR
    for k in range(int(8 / beat) + 1):
        start = int(k * beat * SR)
        # kick: a falling sine
        length = int(0.25 * SR)
        tt = np.arange(length) / SR
        kick = np.sin(2 * np.pi * (50 + 120 * np.exp(-tt * 30)) * tt) * np.exp(-tt * 12)
        end = min(n, start + length)
        out[start:end] += 0.8 * kick[: end - start]
        if k % 2 == 1:  # snare on 2 and 4
            length = int(0.15 * SR)
            snare = rng.standard_normal(length) * np.exp(-np.arange(length) / SR * 25)
            end = min(n, start + length)
            out[start:end] += 0.35 * snare[: end - start]
        for half in (0, 1):  # hats on the eighths
            hs = start + int(half * beat / 2 * SR)
            length = int(0.04 * SR)
            hat = rng.standard_normal(length) * np.exp(-np.arange(length) / SR * 90)
            hat = np.diff(np.concatenate([[0.0], hat]))  # brighter
            end = min(n, hs + length)
            if hs < n:
                out[hs:end] += 0.15 * hat[: end - hs]
    for bar in range(int(8 / (4 * beat)) + 1):  # a bass note per bar
        start = int(bar * 4 * beat * SR)
        length = min(n - start, int(4 * beat * SR))
        if length <= 0:
            continue
        tt = np.arange(length) / SR
        freq = [55.0, 55.0 * 1.5, 55.0 * 4 / 3, 55.0][bar % 4]
        out[start : start + length] += 0.25 * np.sin(2 * np.pi * freq * tt) * np.minimum(1, tt * 50)
    return np.clip(out / np.max(np.abs(out)) * 0.9, -1, 1)


class Logits(torch.nn.Module):
    """The model with plain tensor outputs (beat, downbeat) for the export."""

    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, spect):
        out = self.model(spect)
        return out["beat"], out["downbeat"]


def main() -> None:
    name = sys.argv[1] if len(sys.argv) > 1 else "final0"
    model = load_model(name, "cpu")
    wrapped = Logits(model).eval()

    models = ROOT / "build" / "models"
    models.mkdir(parents=True, exist_ok=True)
    onnx_path = models / f"beat_this_{name}.onnx"
    dummy = torch.zeros(1, 1500, 128)
    torch.onnx.export(
        wrapped,
        (dummy,),
        str(onnx_path),
        input_names=["spect"],
        output_names=["beat", "downbeat"],
        dynamic_axes={"spect": {1: "frames"}, "beat": {1: "frames"}, "downbeat": {1: "frames"}},
        opset_version=17,
        dynamo=False,
    )
    onnx.checker.check_model(str(onnx_path))
    print(f"wrote {onnx_path} ({onnx_path.stat().st_size / 1e6:.1f} MB)")

    # Reference data from the original code, for the C++ port.
    data = ROOT / "tests" / "data" / "beatthis"
    data.mkdir(parents=True, exist_ok=True)
    audio = clip()
    soundfile.write(str(data / "clip.wav"), audio, SR, subtype="PCM_16")
    audio16, _ = soundfile.read(str(data / "clip.wav"), dtype="float64")  # exactly what the C++ test will read
    front = LogMelSpect()
    fbank = front.spect_class.mel_scale.fb.detach().numpy().astype("<f4")  # 513 x 128
    fbank.tofile(data / "mel_fbank.f32")
    def chunked(spect):
        out = split_predict_aggregate(spect, 1500, 6, "keep_first", model)
        return out["beat"], out["downbeat"]

    with torch.no_grad():
        spect = front(torch.tensor(audio16, dtype=torch.float32))
        spect.numpy().astype("<f4").tofile(data / "spect.f32")
        beat, downbeat = chunked(spect)
        # The chunking reference: the clip's first 16 beats (rounded to samples) end to end, to 70 s.
        bars = int(round(16 * 60 / 124 * SR))
        piece = np.tile(audio16[:bars], int(np.ceil(70 * SR / bars)))[: int(np.ceil(70 * SR / bars)) * bars]
        long_beat, long_down = chunked(front(torch.tensor(piece, dtype=torch.float32)))
    np.stack([long_beat.numpy(), long_down.numpy()], axis=1).astype("<f4").tofile(data / "long_logits.f32")
    logits = torch.stack([beat, downbeat], dim=1).numpy().astype("<f4")
    logits.tofile(data / "logits.f32")
    beats, downbeats = Postprocessor(type="minimal", fps=50)(beat, downbeat)
    beat, downbeat = beat.unsqueeze(0), downbeat.unsqueeze(0)
    with open(data / "beats.txt", "w", encoding="utf-8") as f:
        f.write("# beat seconds, downbeat (1) or not (0) - from beat_this postp_minimal\n")
        down = set(np.round(downbeats, 4))
        for b in beats:
            f.write(f"{b:.4f} {1 if round(b, 4) in down else 0}\n")
    print(f"reference: {spect.shape[0]} frames, {len(beats)} beats, {len(downbeats)} downbeats")

    # The ONNX model gives what PyTorch gives (one pass over the same frames).
    session = onnxruntime.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    ob, od = session.run(None, {"spect": spect.unsqueeze(0).numpy()})
    with torch.no_grad():
        tb, td = wrapped(spect.unsqueeze(0))
    print(f"onnx vs torch: max |diff| beat {np.max(np.abs(ob[0] - tb[0].numpy())):.2e}, "
          f"downbeat {np.max(np.abs(od[0] - td[0].numpy())):.2e}")


if __name__ == "__main__":
    main()
