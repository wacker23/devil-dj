# CLAUDE.md — Devil-DJ

Devil-DJ is a Windows DJ application written in C++20 with JUCE 8. It has three pillars:

1. **DJ engine** — 2–4 decks, library + analysis (BPM, beatgrid, key), hot cues, loops, pad modes, FX, sampler, stems, recording, Automix. Feature target: rekordbox 7 PERFORMANCE mode.
2. **Beat Maker** — step sequencer + piano roll + synthesized drum kits, locked to the master clock.
3. **Code Beat** — a live-coding "code deck" (TidalCycles/Strudel-style pattern language) on its own mixer channel.

Everything must be playable **without a DJ controller**: keyboard, mouse, touchscreen.

## Read before working

| File | What it is |
| --- | --- |
| `docs/PLAN.md` | The task list. Every change maps to a task ID (e.g. `P1-07`). |
| `docs/ARCHITECTURE.md` | Threads, modules, classes, data model, DSP design. |
| `docs/CODEBEAT_SPEC.md` | Pattern language, mini-notation grammar, scheduler. |
| `docs/BEATMAKER_SPEC.md` | Beat Maker data model, sequencer, drum synths. |
| `docs/MATCHER_SPEC.md` | Track Matcher: next-track scoring, personal ranker, Smart Sort, Bridge. |
| `docs/KEYMAP.md` | Default keyboard / mouse bindings. |
| `docs/LICENSES.md` | Dependency policy. Check it before adding any library. |
| `docs/DEVLOG.md` | Append one short entry per finished task. |

## How to pick and finish work

1. Find the first unchecked task in the **current phase** of `docs/PLAN.md`. Do not start a later phase early.
2. Before coding, write a short plan (files to touch, approach, tests). Ask the owner if the task is ambiguous.
3. Implement the smallest change that meets the task's **Done when** criteria.
4. Build, run all tests, and run the app if the task has UI or audio.
5. Tick the checkbox in `docs/PLAN.md`, append to `docs/DEVLOG.md`, commit with message `P1-07: <summary>`.
6. Stop and ask the owner before: adding a dependency, changing a public interface in `ARCHITECTURE.md`, changing the threading model, or anything listed under "Open decisions" in `PLAN.md`.

## Build and test (Windows, Visual Studio 2022)

```powershell
# configure (first time or after CMake changes)
cmake --preset windows-x64
# build
cmake --build --preset windows-x64-debug
cmake --build --preset windows-x64-release
# tests
ctest --preset windows-x64-debug --output-on-failure
# JS pattern engine tests (Code Beat)
cd js/codebeat; node --test
# run the app (exact path is confirmed in task P0-03; update this line if it differs)
.\build\windows-x64\src\app\DevilDJ_artefacts\Debug\Devil-DJ.exe
```

Requirements: Visual Studio 2022 (Desktop C++ workload), CMake ≥ 3.25, Git, Node.js ≥ 20 (only for JS tests). Dependencies come from CMake `FetchContent` and vcpkg manifest mode (`vcpkg.json`).

## Repository map

```
src/app/        entry point, main window, settings, app state
src/engine/     AudioEngine, Deck, Mixer, MasterClock, Sampler, Voices, Scheduler, Recorder
src/dsp/        filters, EQ/isolator, limiter, stretcher wrapper, resampler, fx/
src/analysis/   analysis jobs: waveform, beat tracker, key, loudness, phrase, stems
src/library/    SQLite database, migrations, repositories, importers, tag reader
src/beatmaker/  Beat Maker model, sequencer, drum synths, synths
src/codebeat/   QuickJS runtime wrapper, pattern bridge, code deck, sound bank
src/matcher/    next-track features, scorer, choice logger, ranker, Smart Sort, Bridge
src/ui/         all JUCE components, look and feel, keyboard controller
src/util/       lock-free queues, triple buffer, realtime guard, logging, Fraction
js/codebeat/    the pattern language itself (plain modern JS, embedded as binary data)
tests/          Catch2 tests mirroring src/ folders
tools/          scripts: license check, test-signal generator, matcher_eval
installer/      Inno Setup script
docs/           plan and specs
```

## Golden rules

### Real-time audio thread (never break these)

Code reachable from `AudioEngine::audioDeviceIOCallbackWithContext` must not:

- allocate or free memory (`new`, `delete`, `std::vector::push_back`, `std::string`, `std::function` copies, `juce::String`),
- lock a mutex, wait, sleep, or call any blocking OS API,
- do file or network I/O, logging to disk, or call into QuickJS,
- throw exceptions.

Talk to the audio thread only through `util/SpscQueue` (commands in) and `util/TripleBuffer` (state out). Big objects (decoded tracks, stems) are handed over by pointer and released on a non-audio thread through the engine's garbage queue. Debug builds install `RealtimeGuard`, which asserts if the audio thread allocates — keep it green.

### Licensing

- Default policy until decision D1 is made: **closed-source-capable**. No GPL, AGPL or non-commercial dependencies. MIT, BSD, Apache-2.0, BSL-1.0, zlib, ISC, public domain, MPL-2.0/1.1 (unmodified) are fine. See `docs/LICENSES.md`.
- Never copy code from rekordbox, Strudel, TidalCycles, Mixxx, libKeyFinder, Rubber Band or any GPL/AGPL project. Implement from the specs in `docs/`.
- No AlphaTheta / Pioneer / rekordbox names, logos, artwork, fonts, skins or sample content in the app or repo. Devil-DJ has its own look.
- `tools/check_licenses.py` runs in CI and fails on forbidden licenses.

### C++ style

- C++20, MSVC `/W4 /permissive-`, warnings as errors in CI. Format with the repo `.clang-format` (based on JUCE style, 4-space indent, 120 columns).
- Names: `PascalCase` types, `camelCase` functions and variables, `kPascalCase` constants, member variables without prefixes.
- Prefer value types, `std::unique_ptr`, `std::span`, `std::optional`. No raw owning pointers. No `using namespace` in headers.
- All engine/DSP classes have a `prepare(sampleRate, maxBlockSize)` / `reset()` / `process(...)` shape.
- Every new DSP or engine class gets unit tests. Bug fixes get a regression test.

### UI

- JUCE components only; no web views. Waveforms repaint from `juce::VBlankAttachment` at display rate and read engine state from the triple buffer.
- Every control works with mouse, touch and keyboard. Every action is a `CommandId` in `ui/Actions.h` so the keymap and MIDI Learn can bind it.
- Keys are bound by physical scan code, so Korean/other IME input does not break shortcuts.

## Test assets

- Never commit copyrighted music. Unit tests use generated signals (`tools/gen_test_signals.py`: click tracks at known BPM, sine sweeps, chords in known keys).
- The owner keeps a private folder of real tracks for manual evaluation; its path is set by `DEVILDJ_EVAL_DIR` and it is never committed.

## Open decisions (ask the owner)

- **D1** Open source (AGPL) vs closed/commercial. Until decided, follow the closed-capable policy above.
- **D3** GitHub repository location and CI secrets.
- **D4** Owner's laptop specs (test target).
- **D5** Product name trademark check ("Devil-DJ").
