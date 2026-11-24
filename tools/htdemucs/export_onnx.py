#!/usr/bin/env python3
"""HTDemucs (Demucs v4, MIT code; the weights' licence is NOT clear - the owner's own use only, never shipped) ->
an ONNX "core" for Devil-DJ's stem separation (P6-03), plus a reference for the C++ port.

The core is HTDemucs's forward pass without its STFT / inverse STFT (complex numbers don't export): Devil-DJ does
those in C++ exactly as demucs.htdemucs does them (_spec, _magnitude, _mask, _ispec).

    inputs:  mag [1, 4, 2048, 336]  the mixture STFT, channels L.re L.im R.re R.im (normalised STFT, n_fft 4096,
                                    hop 1024, demucs's padding); mix [1, 2, 343980] the waveform (7.8 s at 44.1 kHz)
    outputs: spec [1, 4, 4, 2048, 336] each source's STFT in the same layout (drums, bass, other, vocals)
             wave [1, 4, 2, 343980]    each source's time-branch waveform (added to the inverse STFT of spec)

Dev-only, in build/venv-beatthis (torch, demucs, onnx, onnxruntime). Run from the repository root:

    build\\venv-beatthis\\Scripts\\python.exe tools\\htdemucs\\export_onnx.py

Reads build/models/htdemucs-955717e8.th (downloaded by the owner's go-ahead); writes, all in build/models (never
committed): htdemucs_core.onnx, htdemucs_ref_in.f32 (one segment of our own synthetic stereo audio, 2 x 343980)
and htdemucs_ref_out.f32 (the full PyTorch model's output for it, 4 x 2 x 343980).
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
import onnx
import onnxruntime
import torch
from einops import rearrange

from demucs.states import load_model

ROOT = Path(__file__).resolve().parents[2]
MODELS = ROOT / "build" / "models"
SR = 44100


class Core(torch.nn.Module):
    """HTDemucs.forward between the STFT and the inverse STFT (demucs/htdemucs.py), unchanged in order and maths."""

    def __init__(self, m):
        super().__init__()
        self.m = m

    def forward(self, mag, mix):
        m = self.m
        x = mag
        B, C, Fq, T = x.shape
        mean = x.mean(dim=(1, 2, 3), keepdim=True)
        std = x.std(dim=(1, 2, 3), keepdim=True)
        x = (x - mean) / (1e-5 + std)
        xt = mix
        meant = xt.mean(dim=(1, 2), keepdim=True)
        stdt = xt.std(dim=(1, 2), keepdim=True)
        xt = (xt - meant) / (1e-5 + stdt)
        saved, saved_t, lengths, lengths_t = [], [], [], []
        for idx, encode in enumerate(m.encoder):
            lengths.append(x.shape[-1])
            inject = None
            if idx < len(m.tencoder):
                lengths_t.append(xt.shape[-1])
                tenc = m.tencoder[idx]
                xt = tenc(xt)
                if not tenc.empty:
                    saved_t.append(xt)
                else:
                    inject = xt
            x = encode(x, inject)
            if idx == 0 and m.freq_emb is not None:
                frs = torch.arange(x.shape[-2], device=x.device)
                emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                x = x + m.freq_emb_scale * emb
            saved.append(x)
        if m.crosstransformer:
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = rearrange(x, "b c f t-> b c (f t)")
                x = m.channel_upsampler(x)
                x = rearrange(x, "b c (f t)-> b c f t", f=f)
                xt = m.channel_upsampler_t(xt)
            x, xt = m.crosstransformer(x, xt)
            if m.bottom_channels:
                x = rearrange(x, "b c f t-> b c (f t)")
                x = m.channel_downsampler(x)
                x = rearrange(x, "b c (f t)-> b c f t", f=f)
                xt = m.channel_downsampler_t(xt)
        for idx, decode in enumerate(m.decoder):
            skip = saved.pop(-1)
            x, pre = decode(x, skip, lengths.pop(-1))
            offset = m.depth - len(m.tdecoder)
            if idx >= offset:
                tdec = m.tdecoder[idx - offset]
                length_t = lengths_t.pop(-1)
                if tdec.empty:
                    pre = pre[:, :, 0]
                    xt, _ = tdec(pre, None, length_t)
                else:
                    skip = saved_t.pop(-1)
                    xt, _ = tdec(xt, skip, length_t)
        S = len(m.sources)
        x = x.view(B, S, -1, Fq, T)
        x = x * std[:, None] + mean[:, None]
        xt = xt.view(B, S, -1, mix.shape[-1])
        xt = xt * stdt[:, None] + meant[:, None]
        return x, xt


def synthetic() -> np.ndarray:
    """7.8 s of our own stereo test music: a kick and a hat pattern, a bass line and a chord pad with a 'voice'
    (a vibrato sine with formant-like harmonics). Seeded, so the same every time."""
    rng = np.random.default_rng(7)
    n = 343980
    t = np.arange(n) / SR
    out = np.zeros((2, n))
    beat = 60 / 122
    for k in range(int(n / SR / beat) + 1):
        s = int(k * beat * SR)
        L = min(n - s, int(0.3 * SR))
        if L <= 0:
            continue
        tt = np.arange(L) / SR
        kick = np.sin(2 * np.pi * (45 + 110 * np.exp(-tt * 25)) * tt) * np.exp(-tt * 9)
        out[:, s:s + L] += 0.7 * kick
        hs = s + int(beat / 2 * SR)
        HL = min(n - hs, int(0.05 * SR))
        if HL > 0:
            hat = np.diff(np.concatenate([[0], rng.standard_normal(HL)])) * np.exp(-np.arange(HL) / SR * 70)
            out[0, hs:hs + HL] += 0.12 * hat
            out[1, hs:hs + HL] += 0.09 * hat
    bass = 0.3 * np.sin(2 * np.pi * np.where((t // (4 * beat)) % 2 == 0, 55.0, 41.2) * t)
    pad = sum(0.06 * np.sin(2 * np.pi * f * t + p) for f, p in [(220, 0), (277.2, 1), (329.6, 2)])
    f0 = 330 * (1 + 0.01 * np.sin(2 * np.pi * 5.5 * t))
    voice = sum((0.12 / h) * np.sin(2 * np.pi * h * np.cumsum(f0) / SR) for h in range(1, 8))
    voice *= (np.sin(2 * np.pi * 0.5 * t) > -0.3)
    out += bass + pad
    out[0] += 0.9 * voice
    out[1] += 1.1 * voice
    return (out / np.max(np.abs(out)) * 0.8).astype(np.float32)


def main() -> None:
    weights = MODELS / "htdemucs-955717e8.th"
    model = load_model(str(weights)) if weights.exists() else None
    if model is None:
        raise SystemExit(f"no weights at {weights}")
    model.eval()
    print("sources", model.sources, "segment", float(model.segment), "nfft", model.nfft, "hop", model.hop_length)
    core = Core(model).eval()

    mix = torch.tensor(synthetic())[None]  # 1, 2, 343980
    with torch.no_grad():
        z = model._spec(mix)
        mag = model._magnitude(z)
        full = model(mix)  # the full model, for the reference
    print("mag", tuple(mag.shape), "full", tuple(full.shape))

    path = MODELS / "htdemucs_core.onnx"
    torch.onnx.export(core, (mag, mix), str(path), input_names=["mag", "mix"], output_names=["spec", "wave"],
                      opset_version=17, dynamo=False)
    onnx.checker.check_model(str(path))
    print(f"wrote {path} ({path.stat().st_size / 1e6:.1f} MB)")

    mix.numpy().astype("<f4").tofile(MODELS / "htdemucs_ref_in.f32")
    full.numpy().astype("<f4").tofile(MODELS / "htdemucs_ref_out.f32")

    # The ONNX core + demucs's own mask / inverse STFT reproduces the full model.
    sess = onnxruntime.InferenceSession(str(path), providers=["CPUExecutionProvider"])
    spec, wave = sess.run(None, {"mag": mag.numpy(), "mix": mix.numpy()})
    with torch.no_grad():
        zout = model._mask(z, torch.tensor(spec))
        x = model._ispec(zout, mix.shape[-1]) + torch.tensor(wave)
    err = (x - full).abs().max().item()
    print(f"onnx core + python STFT vs the full model: max |diff| {err:.2e}")


if __name__ == "__main__":
    main()
