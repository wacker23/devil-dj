# Dependency and Licence Policy

Default policy (until decision D1): **closed-source-capable**. Devil-DJ must be shippable as a closed or commercial product, so every dependency must allow that. If D1 later chooses open source (AGPL), this list can be relaxed — not before.

I'm summarizing licences for planning; get legal advice before a commercial release.

## Allowed licence families

MIT, BSD-2/3-Clause, Apache-2.0, BSL-1.0, zlib, ISC, Unlicense / public domain, MPL-2.0 and MPL-1.1 (only if we do not modify the library's files, or we publish our changes to those files).

## Forbidden

GPL (any version), AGPL, LGPL used via static linking (unless an alternative licence such as MPL is available, as with TagLib), CC BY-NC / any non-commercial clause, "research only" model weights, anything without a licence.

## Approved dependencies

| Dependency | Use | Licence | How we comply |
| --- | --- | --- | --- |
| JUCE 8 | App framework, audio I/O, UI | Starter/Indie/Pro EULA or AGPLv3 | Starter tier (free ≤ $20k revenue or funding per year); upgrade tier when revenue grows (D6) |
| Signalsmith Stretch (+ Signalsmith Linear) | Key lock, transpose | MIT | Keep licence text in About box + `THIRD_PARTY_NOTICES.txt` |
| quickjs-ng | Code Beat JS runtime | MIT | Notices file |
| SQLite | Library database | Public domain | — |
| TagLib | Tag reading | LGPL-2.1 **or** MPL-1.1 | Use under MPL-1.1, unmodified; notices file |
| Catch2 v3 | Tests (not shipped) | BSL-1.0 | — |
| xxHash | Content hashing | BSD-2-Clause | Notices file |
| zlib (transitive, via TagLib) | Compression inside TagLib | Zlib | Notices file |
| utfcpp (transitive, via TagLib, header-only) | UTF-8 handling inside TagLib | BSL-1.0 | Notices file |
| Windows Media Foundation | MP3/AAC/M4A/ALAC decoding | Part of Windows | System component |
| libFLAC, libogg, libvorbis (bundled inside JUCE's juce_audio_formats) | FLAC and Ogg Vorbis decoding | BSD-3-Clause | Comes with JUCE; list in notices file |
| ONNX Runtime (Phase 6) | ML inference, DirectML | MIT | Notices file |
| Beat This! weights (Phase 6, optional) | Beat tracking | MIT (code and published weights, per model cards) | Re-verify at integration time |
| HTDemucs weights (Phase 6) | Stem separation | **Unclear** — sources disagree (MIT vs. non-commercial) | Owner decision before shipping; fall back to another model if not clearly commercial-friendly |

## Development tools (never shipped)

Used only to build, format or test; nothing from them ends up in the app or installer, so the shipping-licence rules above don't apply. Not checked by `tools/check_licenses.py`.

| Tool | Use | Licence |
| --- | --- | --- |
| numpy | `tools/gen_test_signals.py` | BSD-3-Clause |
| soundfile (bundles libsndfile, LAME, mpg123, libvorbis, libFLAC) | `tools/gen_test_signals.py` writes the WAV/AIFF/FLAC/Ogg/MP3 test files | BSD-3-Clause (libsndfile, LAME, mpg123: LGPL-2.1, used as separate dev-time libraries) |
| clang-format | Code formatting | Apache-2.0 with LLVM exception |

## Needs a decision before use

| Item | Issue |
| --- | --- |
| ASIO SDK | Dual: GPLv3 or Steinberg's proprietary licence (free, requires signing). For closed source use the proprietary agreement (D7). |
| Ableton Link | Check its licence terms against D1 before integrating (Phase 7). |
| Rubber Band | GPL or paid commercial licence; only if Signalsmith quality is not good enough. |
| Auto-update libraries | Check licence when chosen (Phase 8). |
| LAION CLAP music checkpoint (Track Matcher embeddings, Phase 7) | Model mirrors list the checkpoints as CC0-1.0; confirm at the original source and note that the training data mix includes third-party datasets. |
| OpenL3 (embedding alternative) | Library is MIT; confirm the licence of the weight files. |
| Essentia models (Discogs-EffNet, MAEST) | CC BY-NC-SA 4.0 (non-commercial) with a proprietary licence on request — **forbidden** under the closed-capable policy unless we obtain a commercial licence. |

## Reference only (never copy code)

Strudel, TidalCycles (AGPL/GPL), Mixxx (GPL), libKeyFinder (GPLv3), Rubber Band (GPL edition), Essentia (AGPL). Reading papers and public format descriptions is fine; copying or translating their source code is not.

## Content and branding

No rekordbox / AlphaTheta / Pioneer DJ names, logos, skins, fonts, sample packs or screenshots in the app or repo. No third-party sample packs unless their licence allows redistribution in a commercial app. Built-in drum sounds are synthesized by our own code.

## Mechanics

- `tools/check_licenses.py` parses `cmake/Dependencies.cmake` (`FetchContent_Declare` names) and `vcpkg.json`, plus the vcpkg ports actually installed in the build tree (so transitive packages are caught), matches each one to the **Manifest packages** table below, and fails on anything missing or with a forbidden licence. It runs as a ctest test (`tools.check_licenses`), so CI fails too.
- Adding a dependency = add it to the manifest **and** a row here (after owner approval).

### Manifest packages

Machine-readable allowlist read by `tools/check_licenses.py`. `Package id` is the name used in `vcpkg.json` / the vcpkg port, or the `FetchContent_Declare` name. `Licence` is the licence we use it under (an SPDX id, or `JUCE-EULA` / `Public-Domain`).

| Package id | Source | Licence | Shipped |
| --- | --- | --- | --- |
| juce | FetchContent | JUCE-EULA | yes |
| signalsmith-linear | FetchContent (header-only) | MIT | yes |
| signalsmith-stretch | FetchContent (header-only) | MIT | yes |
| sqlite3 | vcpkg | Public-Domain | yes |
| taglib | vcpkg | MPL-1.1 | yes |
| zlib | vcpkg (via taglib) | Zlib | yes |
| utfcpp | vcpkg (via taglib) | BSL-1.0 | yes |
| xxhash | vcpkg | BSD-2-Clause | yes |
| catch2 | vcpkg | BSL-1.0 | no (tests) |
- `THIRD_PARTY_NOTICES.txt` is generated from this table and installed with the app; the About box shows it.

- P4-20: the JUCE module `juce_cryptography` (SHA-256 for PKCE) - part of JUCE, same licence (D6); Windows' own `crypt32` (DPAPI) for sealing the Spotify tokens. No new third-party library.
