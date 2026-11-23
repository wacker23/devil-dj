# Devil-DJ — Full Development Plan

Version 1.0 · 17 Nov 2026 · Owner: Sophia · Language: C++20 + JUCE 8 · Platform: Windows 10/11 x64

This is the working task list. Claude Code (or any developer) works through it top to bottom, one task per change. Each task has an ID, a short description, the main files, and **Done when** criteria that must all pass before the box is ticked.

Timeline assumes focused full-time work; at a part-time pace expect 9–12 months to 1.0.

| Phase | Name | Weeks | Gate at the end |
| --- | --- | --- | --- |
| 0 | Foundations | 1 | Empty app builds in CI, tests run |
| 1 | Decks and mixer | 2–4 | Two tracks crossfaded from the keyboard |
| 2 | Library and analysis | 5–8 | Import a folder, BPM/key/grid, sync works |
| 3 | Performance (+ Track Matcher v1) | 9–13 | **MVP**: 1-hour keyboard-only set, no dropouts |
| 4 | Beat Maker | 14–17 | A beat plays in sync under a track |
| 5 | Code Beat | 18–22 | **Beta**: live-coded pattern crossfaded against a track |
| 6 | Stems and smart features | 23–27 | 4-stem mixing, Drum Swap, personal Track Matcher |
| 7 | Pro extras | 28–31 | 4 decks, MIDI Learn, mic, Edit mode, Smart Sort, Bridge |
| 8 | Polish and release | 32–35 | **1.0**: signed installer, public download |

---

## Global Definition of Done (applies to every task)

- Builds with zero warnings in Debug and Release (`/W4 /permissive-`).
- All Catch2 and JS tests pass; new code has tests (DSP/engine/library/analysis) or a manual test note (UI).
- `RealtimeGuard` reports no audio-thread allocation during the task's manual test.
- No new dependency without owner approval and a `docs/LICENSES.md` entry.
- `docs/PLAN.md` box ticked, `docs/DEVLOG.md` entry added, commit message starts with the task ID.

## Open decisions

| ID | Question | Default until decided |
| --- | --- | --- |
| D1 | Open source (AGPL) or closed/commercial? | Closed-capable: permissive dependencies only, own pattern engine |
| D2 | Framework | **Decided: C++20 + JUCE 8** |
| D3 | GitHub repo and CI | **Repo: github.com/wacker23/devil-dj** (currently public — confirm), GitHub Actions `windows-latest` |
| D4 | Owner's laptop specs | Target: 4-core CPU, 8 GB RAM, integrated GPU, built-in audio |
| D5 | Name "Devil-DJ" trademark check (Korea + target markets) | Keep as working name |
| D6 | JUCE licence tier | Starter (free up to $20k/yr revenue) or AGPL if D1 = open |
| D7 | ASIO support licence (proprietary agreement vs GPLv3) | WASAPI only until decided |

---

## Phase 0 — Foundations (week 1)

Goal: a repository where an empty JUCE window builds locally and in CI, with the utility classes every later phase relies on.

- [x] **P0-01 Repository skeleton**
  Create the folder layout from `CLAUDE.md`, `.gitignore` (build/, out/, .vs/, *.user), `.gitattributes` (LF for text, binary for audio), `.clang-format`, `.editorconfig`, `README.md`, `docs/DEVLOG.md`.
  Done when: tree matches `CLAUDE.md`; clang-format runs clean on an example file.

- [x] **P0-02 CMake + presets**
  Top-level `CMakeLists.txt` (C++20, MSVC flags `/W4 /permissive- /utf-8 /MP`), `CMakePresets.json` with configure preset `windows-x64` (generator "Visual Studio 18 2026" — owner has VS 2026, arch x64, binaryDir `build/windows-x64`) and build/test presets `windows-x64-debug` / `windows-x64-release`. `cmake/Dependencies.cmake` pulls JUCE with `FetchContent` pinned to the latest JUCE 8.0.x release tag.
  Done when: `cmake --preset windows-x64` and both build presets succeed on a clean machine.

- [x] **P0-03 App target**
  `src/app/Main.cpp` with `juce::JUCEApplication`, `juce_add_gui_app(DevilDJ ...)` (PRODUCT_NAME "Devil-DJ"), a resizable `MainWindow` with an empty `MainComponent`, dark default look and feel, window size/position persisted.
  Done when: the exe opens a window, remembers its size after restart.

- [x] **P0-04 vcpkg manifest**
  `vcpkg.json` with `sqlite3`, `taglib`, `catch2`, `xxhash`. CMake toolchain wiring via `CMAKE_TOOLCHAIN_FILE` in the preset (vcpkg as a submodule or `VCPKG_ROOT`).
  Done when: a test target links Catch2 and SQLite; TagLib links into the app.

- [x] **P0-05 Test harness**
  `tests/CMakeLists.txt`, Catch2 main, `ctest` integration, one smoke test per module folder.
  Done when: `ctest --preset windows-x64-debug` runs and passes.

- [x] **P0-06 CI**
  `.github/workflows/windows.yml`: checkout, cache vcpkg + FetchContent, configure, build Debug + Release, run ctest, run `tools/check_licenses.py`, upload Release exe as an artifact.
  Done when: a push to `main` goes green and produces a downloadable zip.

- [x] **P0-07 License checker**
  `tools/check_licenses.py`: reads `docs/LICENSES.md` allowlist and the dependency manifest (`cmake/Dependencies.cmake` + `vcpkg.json`), fails on any dependency not listed or with a forbidden licence.
  Done when: adding a fake GPL entry makes CI fail.

- [x] **P0-08 Lock-free primitives** (`src/util/`)
  `SpscQueue<T, Capacity>` (power-of-two ring, acquire/release atomics, `tryPush`, `tryPop`), `TripleBuffer<T>` (writer publishes, reader gets latest without blocking), `GarbageQueue` (audio thread pushes pointers to release; a timer thread frees them).
  Done when: unit tests incl. a 2-thread stress test (10M items, no loss, no reorder) pass in Release.

- [x] **P0-09 RealtimeGuard** (`src/util/RealtimeGuard.*`)
  Debug-only global `operator new/delete` override that checks a `thread_local bool inRealtimeContext` and calls `jassertfalse` + logs a stack trace once. `ScopedRealtimeContext` RAII sets the flag.
  Done when: a test that allocates inside the scope triggers the guard; normal code is unaffected; Release builds compile it out.

- [x] **P0-10 Logging + crash dumps**
  `util/Log` (async file logger to `%LOCALAPPDATA%\Devil-DJ\logs`, rotating, never called from audio thread), unhandled exception filter that writes a minidump (`MiniDumpWriteDump`) next to the logs.
  Done when: a debug menu "Crash test" produces a `.dmp` and a log line.

- [x] **P0-11 Settings store**
  `app/Settings` wrapping `juce::PropertiesFile` in `%APPDATA%\Devil-DJ\settings.xml`, typed getters/setters, change listeners.
  Done when: values survive restart; unit test for defaults.

- [x] **P0-12 Fraction type**
  `util/Fraction` (int64 numerator/denominator, normalized, +−×÷, comparison, floor, `toDouble`) — used by the beat clock and pattern engine bridge.
  Done when: property tests (random values) for arithmetic identities pass.

- [x] **P0-13 Test signal generator**
  `tools/gen_test_signals.py` (Python, no deps beyond numpy/soundfile): click tracks at 90/120/128/140/174 BPM with known first beat, sine sweeps, major/minor chord loops in all 24 keys, silence-padded files. Output to `tests/data/generated/` (git-ignored, generated in CI).
  Done when: CI generates the files before tests run.

**Phase 0 gate:** CI green on `main`; empty window launches from the CI artifact on the owner's laptop. **Passed 2026-11-18** (CI run 44159622127, artifact 6cb119d).

---

## Phase 1 — Decks and mixer (weeks 2–4)

Goal: two decks play real files with key lock; a mixer with EQ, filter, faders and crossfader; master and headphone cue outputs; keyboard control.

### Audio I/O and engine core

- [x] **P1-01 Audio device manager**
  `engine/AudioDeviceService` wrapping `juce::AudioDeviceManager`: lists device types (Windows Audio, Windows Audio Exclusive, Low Latency, DirectSound), sample rate, buffer size; saves/restores the choice; reports xruns (callback time > buffer time) and current latency.
  Done when: settings page can switch devices without restart; selected device persists.

- [x] **P1-02 AudioEngine skeleton**
  `engine/AudioEngine` implements `juce::AudioIODeviceCallback`. Owns `CommandQueue` (SpscQueue of `EngineCommand` — a `std::variant` of POD structs), publishes `EngineState` via TripleBuffer once per block, enters `ScopedRealtimeContext` inside the callback, measures CPU load per block.
  Done when: engine outputs silence with a test tone command; CPU meter visible in a debug overlay.

- [x] **P1-03 Output routing**
  Output layouts: (a) stereo master only; (b) 4-channel device: master 1/2, cue 3/4; (c) **split mode** on a stereo device: master summed to mono in left, cue mono in right. Configurable in settings.
  Done when: each layout verified with a test tone panned per bus; documented in settings tooltip.

### Decoding and track loading

- [x] **P1-04 Decoder abstraction**
  `engine/TrackDecoder`: JUCE readers for WAV/AIFF/FLAC/OGG; Windows Media Foundation `IMFSourceReader` for MP3/AAC/M4A/ALAC. Output float stereo at the device rate (resample at load with a high-quality windowed-sinc resampler if needed).
  Done when: generated test files and one file of each format decode; duration within 1 ms of reference; unit test compares decoded WAV to source.

- [x] **P1-05 TrackLoader thread**
  Background thread pool decodes into `std::shared_ptr<const TrackBuffer>` (interleaved or planar float, sample rate, length, file path, content hash). Loading posts `LoadTrack{deck, ptr}` to the engine; the replaced buffer goes to `GarbageQueue`. Progress events for UI.
  Done when: loading a 10-min MP3 doesn't glitch playback on the other deck; memory is released after replacing a track (verified with a counter).

### Deck playback

- [x] **P1-06 Deck transport**
  `engine/Deck`: play/pause, cue (CDJ-style: paused → set cue at playhead; playing → jump to cue and pause; hold → preview, release returns), seek, position in samples (double), end-of-track handling, 2 ms micro-fades on every jump to avoid clicks.
  Done when: unit tests for cue logic state machine; no clicks on jumps (test: max sample delta at jump < threshold).

- [x] **P1-07 Tempo and pitch**
  Tempo fader with ranges ±6/±10/±16/±50 %, fine steps; pitch bend (temporary ± up to 4 %, ramped); tempo reset. Without key lock: variable-rate playback via high-quality interpolation (`dsp/Resampler`, cubic Hermite for speed, optional sinc).
  Done when: playing a 120 BPM click at +8 % measures 129.6 BPM ±0.05 in a test.

- [x] **P1-08 Key lock (Master Tempo)**
  `dsp/Stretcher` interface with `SignalsmithStretcher` implementation (MIT). Handles latency compensation so the displayed position matches what is heard. Key lock toggle per deck; transpose in semitones for later Key Shift.
  Done when: 440 Hz sine at +10 % tempo stays 440 Hz ±1 Hz with key lock; CPU for 2 decks with key lock < 15 % of the 256-sample budget at 48 kHz on the target laptop (measure and log).

- [x] **P1-09 Jog / scratch from mouse**
  Drag on the zoomed waveform: while dragging, playback rate follows mouse velocity (smoothed), key lock bypassed; release resumes the previous rate with a 50 ms ramp. Playing vs paused behaviour (nudge vs scrub). Scroll wheel on waveform = fine nudge.
  Done when: scratching sounds continuous (no zipper noise) at 60 Hz UI event rate; manual test note.

### Mixer

- [x] **P1-10 Channel strip DSP**
  `engine/ChannelStrip`: trim gain (±12 dB), 3-band EQ with two modes — *EQ* (low shelf 70 Hz, peak 1 kHz, high shelf 13 kHz, −26…+6 dB) and *Isolator* (Linkwitz-Riley 4th-order crossovers at 250 Hz / 2.5 kHz, per-band −∞…+6 dB kill) — and a one-knob filter (centre off; left = resonant low-pass, right = high-pass; TPT state-variable filter), channel fader with curve.
  All parameters smoothed (20 ms).
  Done when: frequency-response tests: isolator bands sum flat ±0.1 dB at 0 dB; kill attenuates > 60 dB at band centre; filter cutoff sweeps without clicks.

- [x] **P1-11 Crossfader and master**
  Crossfader with assign A/THRU/B per channel and curves: smooth (constant power), linear, sharp cut (scratch). Master gain, brick-wall look-ahead limiter (1.5 ms, −0.3 dBFS ceiling), peak + RMS meters per channel and master (in `EngineState`).
  Done when: unit tests for curve values at 0/0.5/1; limiter never exceeds ceiling on a +12 dB test signal.

- [x] **P1-12 Headphone cue**
  CUE button per channel, cue/master mix knob, headphone level. Routed to the cue bus selected in P1-03. Split mode works with the cue knob.
  Done when: cueing deck B while A plays to master verified in all three output layouts.

### UI

- [x] **P1-13 Deck view**
  Track title/artist, time elapsed/remaining, BPM (with tempo %), key lock indicator, play/cue/sync buttons, tempo slider, overview waveform with playhead and click-to-seek.
  Done when: all controls usable with mouse and touch; screenshot in DEVLOG.

- [x] **P1-14 Waveform data + zoomed view**
  `analysis/WaveformBuilder` (run at load for now): per-track arrays at 150 points/s of peak + RMS for low (<250 Hz), mid, high (>2.5 kHz) bands; overview at 1,024 points. `ui/WaveformView` draws the zoomed view centred on the playhead, 3-band colour mode, zoom with Ctrl+wheel, repaints via `juce::VBlankAttachment`, playhead extrapolated from the last `EngineState` timestamp.
  Done when: 60 fps on the target laptop with 2 decks (measure frame time); waveform visibly aligned with audio (manual click-track test).

- [x] **P1-15 Mixer view**
  Trim, EQ ×3, filter, fader, cue button, meters per channel; crossfader; master level + meter; headphone mix/level. Knobs: drag vertical, scroll wheel, double-click reset, Alt for fine.
  Done when: every control works by mouse, wheel and touch.

- [x] **P1-16 Action system + keyboard controller v1**
  `ui/Actions.h` enumerates every bindable action (`CommandId`); `ui/KeyboardController` maps physical scan codes (`juce::KeyPress` + raw key code) to actions with press/release/hold semantics; default map from `docs/KEYMAP.md` (transport, pitch bend, crossfader, load). Ignores key repeat; works with Korean IME active. Includes a unit test that fails if two actions share a key + modifier in the same focus context.
  Done when: Phase 1 actions in KEYMAP work; unit test for the binding table; manual test with IME on.

- [x] **P1-17 Keymap overlay**
  Hold Ctrl+/ to show a translucent overlay of the current bindings drawn on a keyboard layout.
  Done when: overlay reflects user rebinding.

- [x] **P1-18 Simple file loading**
  Temporary file browser panel (folder tree + file list) with "Load to A/B" (Tab/Enter) and drag-and-drop from Windows Explorer onto a deck. Replaced by the library in Phase 2.
  Done when: drag-and-drop and keyboard loading both work.

- [x] **P1-19 Audio settings page**
  Device, sample rate, buffer size, output layout, latency readout, test tone button, xrun counter.
  Done when: owner can set up her laptop + headphones without help (manual test).

- [x] **P1-20 Soak test**
  Debug overlay toggle (F12) showing CPU per block, xruns, memory. 60-minute run with two decks looping, key lock on, EQ moving.
  Done when: zero xruns at 256 samples / 48 kHz on the target laptop, or the smallest safe buffer is documented.

**Phase 1 gate:** load two MP3s, play both, change tempo with key lock, EQ and crossfade between them, cue in headphones — all from the keyboard. **Passed 2026-11-19** (owner, keyboard only; 60-minute soak PASS, 0 xruns).

---

## Phase 2 — Library and analysis (weeks 5–8)

Goal: a real music library with BPM, beatgrid and key analysis, plus Sync, Quantize and Beat Jump.

### Library

- [x] **P2-01 Database layer**
  `library/Database` (SQLite, WAL mode, prepared-statement cache, single writer thread + read connections), `library/Migrations` using `PRAGMA user_version`. Schema v1 from `ARCHITECTURE.md` §7.
  Done when: migration tests from empty DB; concurrent read during write test.

- [x] **P2-02 Track import**
  `library/Importer`: add files/folders (recursive), skip duplicates by path + content hash (xxHash of first/last 1 MB + size), read tags with TagLib (title, artist, album, genre, year, BPM tag, key tag, comment, artwork), queue analysis.
  Done when: importing 2,000 files takes < 60 s excluding analysis; re-import adds nothing.

- [x] **P2-03 Repositories**
  `TrackRepository`, `PlaylistRepository`, `CueRepository`, `TagRepository` with typed C++ structs; all DB access off the audio thread; change notifications to UI.
  Done when: CRUD unit tests per repository.

- [x] **P2-04 Browser UI**
  Left: tree (Collection, Playlists, Smart playlists, History, Folders). Right: virtualized `juce::TableListBox` with configurable/sortable columns (title, artist, BPM, key, time, rating, colour, genre, date added, comment). Artwork thumbnail. Handles 50,000 tracks smoothly.
  Done when: scrolling 50k rows at 60 fps; column layout persists.

- [x] **P2-05 Search and filter**
  Instant search (title/artist/album/comment) with SQLite FTS5; filters for BPM range, key, rating, colour, genre, tags.
  Done when: search results < 50 ms on 50k tracks.

- [x] **P2-06 Playlists**
  Create/rename/delete, folders, drag tracks in, reorder, delete from playlist, M3U/M3U8 import and export.
  Done when: round-trip M3U8 test.

- [x] **P2-07 Smart playlists**
  Rule builder (field, operator, value; AND/OR), live-updating.
  Done when: rule → SQL generation unit tests.

- [x] **P2-08 Tags, ratings, colours**
  My-Tag-style custom tag categories, 0–5 star rating, track colour; editable inline and via keyboard (Ctrl+Shift+1…5 rating, see KEYMAP).
  Done when: persisted and filterable.

- [x] **P2-09 Importers from other software**
  rekordbox XML import (tracks, playlists, cues, beatgrid tempo markers), iTunes/Apple Music library XML import. Write our own XML parsing against the public format; no AlphaTheta code.
  Done when: sample XML fixture (hand-written) imports cues and playlists correctly.

- [x] **P2-10 Missing files**
  Detect missing files, "relocate" dialog (search a folder by filename + size), "show all missing".
  Done when: moving a folder and relocating restores all links.

- [x] **P2-11 Library backup**
  Backup/restore DB + settings to a zip; automatic daily backup (keep 7).
  Done when: restore test.

### Analysis

- [x] **P2-12 Analysis service**
  `analysis/AnalysisService`: job queue, worker pool (cores − 1, lowest priority), progress per track, `analysis_version` per result so upgrades re-analyze. Decodes to mono 22.05 kHz for analysis plus waveform data at full rate.
  Done when: 500 tracks analyze in the background while two decks play with no xruns.

- [x] **P2-13 Beat tracker v1 (classic DSP, our own)**
  Spectral-flux onset envelope → tempo estimation by autocorrelation/comb-filter tempogram (range 70–180 BPM with octave-error correction preferring 85–175) → beat positions by dynamic programming → constant-tempo grid fit (least squares on beat times) → downbeat estimate (bar phase with strongest low-frequency onset pattern). Variable-tempo detection flags tracks whose local tempo drifts > 1 %.
  Done when: on generated click tracks BPM error < 0.02 and first beat < 5 ms; on the owner's eval set (DEVILDJ_EVAL_DIR, ≥ 50 tracks with known BPM) ≥ 90 % within ±0.5 BPM (report written to DEVLOG).

- [x] **P2-14 Beatgrid model + editing**
  Grid = list of anchors (beat index, sample position, BPM until next anchor). UI tools: set downbeat here, shift grid ±1 ms / ±1 beat, BPM ×2 / ÷2, tap tempo, metronome click on cue bus, lock analysis.
  Done when: grid edits persist; metronome stays on the grid for 10 minutes on a 128 BPM click track.

- [x] **P2-15 Key detection (our own)**
  Chromagram (FFT 8192, hop 2048, 36 bins/octave folded to 12, tuning estimation), averaged over the track with energy weighting, correlated against 24 key profiles; output key + confidence. Display as musical key and Camelot code (user setting).
  Done when: all 24 generated chord loops detected correctly; eval-set accuracy reported (target ≥ 75 % exact, ≥ 90 % including relative/fifth).

- [x] **P2-16 Loudness**
  ITU-R BS.1770 integrated loudness (K-weighting, gating) per track; Auto Gain sets trim so tracks play at a target loudness (setting, default chosen by listening tests).
  Done when: test vectors (generated sine at known level) within 0.1 LU.

- [x] **P2-17 Analysis cache**
  Waveform/peaks stored in a compact binary file per track (`%LOCALAPPDATA%\Devil-DJ\cache\<hash>.wfm`); deck load reads cache instead of recomputing.
  Done when: loading an analyzed track shows its waveform in < 100 ms.

### Beat-aware deck features

- [x] **P2-18 MasterClock**
  `engine/MasterClock`: master BPM and beat phase (double beats) advanced per sample block; master source = a chosen deck (auto: the first playing synced deck) or internal tempo. Provides `beatAt(sampleTime)` / `sampleAt(beat)` for the scheduler.
  Done when: unit tests for phase continuity across tempo changes.

- [x] **P2-19 Beat Sync**
  Sync button: follower matches master BPM (half/double aware) and phase. Phase correction = proportional rate nudge limited to ±0.5 %, converges < 1 beat; "sync lock" keeps it during tempo changes; tempo fader moves all synced decks.
  Done when: two click tracks at 124 and 128 BPM, synced, stay within 1 ms for 10 minutes (automated engine test, offline render).

- [x] **P2-20 Quantize**
  Global quantize toggle + resolution (1/8 … 1 beat). Cue, hot cue, loop in, beat jump and play-from-cue snap to the grid, executed sample-accurately on the audio thread at the next grid point.
  Done when: engine test: triggered actions land on beat ±1 sample.

- [x] **P2-21 Beat Jump**
  Jump ±(1/8, 1/4, 1/2, 1, 2, 4, 8, 16, 32, 64, 128) beats and a fine 5 ms jump; while looping, moves the loop.
  Done when: unit tests; keyboard bindings in KEYMAP.

- [x] **P2-22 Key Sync and Key Shift**
  Key Sync: transpose a deck to the master deck's key (smallest semitone move, compatible-key option). Key Shift ±1 semitone steps (needs key lock).
  Done when: transposed key shows in the deck header; test with generated chords.

- [x] **P2-23 History**
  Every track played > 60 s goes into the session history (date-stamped playlist); export as text tracklist. Also store each transition (previous track → next track, time) for the Track Matcher's history feature.
  Done when: history survives restart; transitions queryable.

- [x] **P2-24 Matcher features v1** (spec: `docs/MATCHER_SPEC.md` §4.3–4.5)
  New analyzers: `EnergyAnalyzer` (energy per 16-bar window + intro/outro/whole, library-percentile normalized), `SoundVector` (32-d MFCC/spectral fingerprint), `GrooveProfile` (48-d accent profile). Stored in `track_features` (schema v2 migration).
  Done when: unit tests on generated signals (e.g. denser clicks → higher energy; four-on-the-floor vs broken-beat clicks give clearly different groove vectors); library-percentile normalization recomputed after imports.

- [x] **P2-25 Separate headphone device** (added by the owner, 2026-11-21)
  Master on one output device (e.g. the laptop speakers), the headphone cue on a second device (the headphone jack when Windows lists it separately, a USB headset): a second audio callback reads the cue from a lock-free buffer that follows both clocks (adaptive resampling, at most ±0.5 %). Audio settings: a fourth headphone layout with the device list.
  Done when: unit tests with drifting clocks (no dropouts, the pitch kept); two fake sound cards play master and cue at once; the owner hears master in the speakers and cue in the headphones.

**Phase 2 gate:** import the owner's music folder, analyze it in the background, sync two tracks and beat-jump/quantize cleanly.

---

## Phase 3 — Performance (weeks 9–13) → MVP

Goal: everything needed for a real set: cues, loops, pads, FX, sampler, recording, Automix, auto-transition.

### Cues and loops

- [x] **P3-01 Hot cues**
  16 hot cues per track (two pages of 8 pads), set/trigger/delete, colours, labels, quantized, stored in DB. Loop hot cues (store a loop). Sorting by time option.
  Done when: persist across restarts; keyboard pads trigger them.

- [x] **P3-02 Memory cues and loops**
  Up to 10 memory cues + 10 memory loops per track, list panel, call previous/next.
  Done when: CRUD + call tested.

- [x] **P3-03 Loops**
  Auto beat loop 1/64…512 beats, manual loop in/out, halve/double, reloop, loop exit, loop move (beat jump while looping). Sample-accurate wrap with 2 ms crossfade. Loop export to WAV (to sampler or file).
  Done when: engine tests for loop wrap accuracy; no clicks on a sine test.

- [x] **P3-04 Slip mode**
  Shadow playhead keeps running during loops, hot-cue holds, scratching, reverse and pause; releasing returns to the shadow position.
  Done when: engine test: slip loop of 1 beat for 4 beats returns to exactly +4 beats.

- [x] **P3-05 Reverse and brake**
  Reverse playback toggle; vinyl brake / spin-back as release actions with adjustable time.
  Done when: manual test.

### Pads

- [x] **P3-06 Pad engine + pad grid UI**
  8-pad grid per deck (on screen + keyboard rows from KEYMAP), modes switchable via F1–F4 (+Shift for more): Hot Cue, Beat Loop (1/4…32), Beat Jump, Slicer, Key Shift (−4…+3 semitones), Keyboard (play hot cue 1 chromatically), Pad FX, Sampler.
  Done when: each mode works from keyboard and mouse; pad colours reflect state.

- [x] **P3-07 Slicer**
  Divide 8/16/32 beats ahead into 8 slices; pad press plays that slice quantized (roll while held), slip-style return on release; domain shift.
  Done when: manual test on a 128 BPM track sounds on-grid.

### Effects

- [x] **P3-08 FX framework**
  `dsp/fx/Effect` interface (prepare, reset, process stereo block, beat-synced parameters from MasterClock, dry/wet, on/off with tail handling). Two FX units, each with up to 3 slots (multi mode) or 1 effect with full parameters (single mode); target selectable per unit: deck 1–4, sampler, code deck, master.
  Done when: unit tests for tail handling and bypass (bit-exact when off).

- [x] **P3-09 Beat FX set** (all our own DSP)
  Echo (tempo-synced delay, feedback, LP in feedback), Ping-Pong delay, Reverb (FDN 8-line), Flanger, Phaser, Filter LFO, Roll (quantized buffer repeat), Trans (beat-synced gate), Spiral (feedback delay with shortening time), Bit Crush, Noise sweep, Pitch (via stretcher transpose), Vinyl brake (release FX), Helix/Freeze (hold buffer). Beat lengths 1/16…16 beats; manual tap BPM option.
  Done when: each effect has a parameter-sweep test (no NaN/Inf, no output > +6 dBFS on 0 dBFS input) and a manual listening note.

- [x] **P3-10 Color FX (one knob per channel)**
  Selector shared across channels: Filter (default), Space (reverb send), Dub Echo, Noise, Crush, Pitch. Knob centre = off, left/right = two characters.
  Done when: manual listening test; parameter smoothing verified.

- [x] **P3-11 Release FX**
  Hold-to-apply effects that cut the dry signal on release (echo out, brake, spin-back, filter sweep) with beat length.
  Done when: manual test; works with Pad FX.

- [x] **P3-12 Pad FX**
  2 banks × 8 pads of preset effect + parameter snapshots; momentary or latched; user-editable presets.
  Done when: presets persist; keyboard mode triggers them.

### Sampler and sequencer

- [x] **P3-13 Sampler deck**
  4 banks × 8 slots; load from library, file, loop capture or Beat Maker (Phase 4); modes one-shot / loop / gate; quantized start; sync to master BPM (stretch); per-slot gain; sampler bus with its own channel strip, cue, and crossfader assign.
  Done when: 8 simultaneous looped slots, synced, no xruns.

- [x] **P3-14 Capture**
  Capture the current loop or slicer range of a deck into a sampler slot (saved as WAV in the user samples folder).
  Done when: captured loop plays back sample-identical (test).

- [x] **P3-15 Sequencer**
  Record sampler pad presses into a 1/2/4-bar quantized loop; overdub, mute per slot, erase, save up to 8 sequences per bank, recall via "Sequence Call" pad mode; metronome option.
  Done when: recorded sequence replays identically after restart.

### Recording and auto-mixing

- [x] **P3-16 Mix recording**
  Record master (or a chosen bus) to WAV 24-bit or FLAC via a writer thread; level meter; auto start on signal / auto split after 20 s of silence; writes a tracklist `.txt` and `.cue` with timestamps from history.
  Done when: 60-min recording has no gaps (compare sample count with wall clock); file opens in other software.

- [x] **P3-17 Auto-transition**
  Action "T": plans a transition from the playing deck to the other: chooses length (8/16/32 beats; setting), starts at the next phrase boundary (or next 16-beat boundary before phrase detection exists), syncs, then runs automation lanes on the audio thread: crossfader ramp, low-EQ swap at the midpoint, optional filter sweep or echo out. Cancel with T again.
  Done when: engine test renders a transition offline and checks crossfader/EQ positions per beat; manual listening test.

- [x] **P3-18 Automix**
  Automix panel: queue a playlist; repeat/random; transition style; uses mix-out = last 32 beats before track end (or outro phrase later) and mix-in = first downbeat; runs auto-transitions continuously.
  Done when: 10-track automix plays unattended, all transitions on beat.

- [x] **P3-19 Track Matcher v1 core** (spec: `docs/MATCHER_SPEC.md` §2–5)
  `src/matcher/`: Camelot conversions + relationship table, hard filters as one SQL prefilter, features f_tempo, f_key, f_energy, f_sound (v1 vector), f_groove, f_tags, f_hist, f_rating, f_fresh with reason strings, weighted score with renormalization and penalties, presets JSON, MMR diversity re-rank, wildcard slot. `MatcherService` on a worker thread with the triggers in §2.
  Done when: all MATCHER_SPEC §13 tests for layer 1 pass; 5,000 candidates scored in ≤ 30 ms (Release).

### Hardware-free polish

- [x] **P3-20 Full default keymap**
  All Phase 1–3 actions bound per `docs/KEYMAP.md`; keymap editor (click action → press key; conflicts shown); import/export keymap JSON; presets "Default", "Laptop compact".
  Done when: every action in `Actions.h` is bindable; export/import round-trip test.

- [x] **P3-21 Touch mode**
  Larger hit targets option, multi-touch (two faders at once), touch-drag jog.
  Done when: manual test on a Windows touchscreen (or simulated pointer events).

- [x] **P3-22 Dual-device cueing**
  Optional second output device for headphones (e.g. laptop jack + USB DAC for speakers). Second device callback consumes a ring buffer filled by the main callback; adaptive resampling (PI controller on buffer fill level) compensates clock drift.
  Done when: 60-minute run with two devices: buffer neither under- nor overflows; drift-corrected; no audible glitches.

- [x] **P3-23 Layouts**
  2-deck horizontal (default) and vertical waveform layouts; browser-enlarged layout; layout persisted.
  Done when: switch layouts while playing without glitches.

- [x] **P3-24 Next Track panel**
  UI from MATCHER_SPEC §6: scope, preset, intent ↓ → ↑ ⤨, key policy; top-10 list with score bars and reason chips; full breakdown on hover/long-press; energy mini-line and Camelot wheel. Keys N / Shift+N / Ctrl+N / Ctrl+Shift+N per KEYMAP.
  Done when: suggestions refresh within 100 ms of loading a track or changing intent; every control reachable by keyboard.

- [x] **P3-25 Automix smart mode**
  Automix option "smart": at each transition pick the best-scoring remaining track in scope instead of playlist order.
  Done when: a 10-track smart Automix runs unattended; chosen order is logged so it can be compared with plain order.

**MVP gate checklist (manual, on the owner's laptop):**

- [ ] 1-hour set using only keyboard + mouse, headphone cueing via split mode or two devices.
- [ ] No xruns in the debug overlay; recording of the set has no gaps.
- [ ] Hot cues, loops, beat jump, two FX, sampler loop and auto-transition used during the set.
- [ ] At least 5 tracks picked straight from the Next Track panel (N key), and the reasons shown made sense to the owner.
- [ ] App restart restores library, cues and settings.

---

## Phase 4 — Beat Maker (weeks 14–17)

Spec: `docs/BEATMAKER_SPEC.md`.

- [x] **P4-01 Model + serialization** — `beatmaker/Project`, `Pattern`, `DrumTrack`, `SynthTrack`, `Step`, `Note`; JSON `.ddjbeat` save/load with schema version. Done when: round-trip tests.
- [x] **P4-02 Event source** — `beatmaker/Sequencer` converts the active pattern/song into timed events for a window (PPQ 96, swing, micro-timing, probability with a seeded RNG, ratchets), feeding the shared `engine/Scheduler`. Done when: event-timing tests at 120/174 BPM with swing.
- [x] **P4-03 Voices** — `engine/Voices`: polyphonic sample voices (64) and synth voices (32), voice stealing, choke groups, per-voice gain/pan/filter, sends to the module's delay and reverb. Done when: 64 voices at 48 kHz within CPU budget; stealing tests.
- [x] **P4-04 Drum synths** — kick, snare, clap, closed/open hat, toms ×3, rim, cowbell, shaker per BEATMAKER_SPEC §4; parameters (tune, decay, tone, snap, drive); rendered live (not sampled). Done when: each sound has a parameter-sweep test (no NaN, bounded output) and a listening note.
- [x] **P4-05 Synths** — subtractive (2 osc + sub + noise, SVF LP/BP/HP with env, ADSR ×2, glide), 2-operator FM, sub-bass (sine + drive + glide). 10 presets each. Done when: tests + listening notes.
- [x] **P4-06 Step grid UI** — 8 tracks × 16/32/64 steps, velocity lanes, per-step probability/micro-timing/ratchet editing, mute/solo, track sound picker, swing knob, pattern A–H selector; keyboard editing (arrows, Enter, 1–8 tracks). Done when: everything editable by keyboard and mouse.
- [ ] **P4-07 Piano roll UI** — notes draw/move/resize, velocity, scale lock, chord stamp, quantize, arpeggiator per synth track. Done when: manual test; undo works.
- [ ] **P4-08 Undo/redo** — `juce::UndoManager` across Beat Maker edits. Done when: 100-step undo test.
- [ ] **P4-09 Song mode + live switching** — chain patterns, queue next pattern to switch on the next bar, pad grid triggers patterns. Done when: switching is sample-accurate on the bar.
- [ ] **P4-10 Mixer integration** — Beat Maker bus = its own mixer channel (strip, cue, crossfader, FX target); follows master BPM or its own tempo. Done when: beat crossfades against a deck.
- [ ] **P4-11 Live record** — record pad/keyboard hits into steps (quantized, overdub). Done when: manual test.
- [ ] **P4-12 Export** — render pattern/song offline to WAV; to sampler slot; to a library track. Done when: offline render bit-identical to real-time output for a deterministic pattern.
- [ ] **P4-13 User samples** — drag WAVs into drum tracks; sample folder `Documents\Devil-DJ\Samples`. Done when: missing-sample handling tested.

**Phase 4 gate:** build an 8-track beat with a bassline, play it under a synced track, switch patterns on the bar, render it to a sampler slot.

---

## Phase 5 — Code Beat (weeks 18–22) → Beta

Spec: `docs/CODEBEAT_SPEC.md`. The pattern language is written from scratch in plain JS in `js/codebeat/` and runs inside QuickJS (quickjs-ng, MIT).

### Language (JS, tested with `node --test`)

- [ ] **P5-01 Fraction + time spans** — `fraction.js` (BigInt-free rational with safe integer checks), `span.js` (begin/end, cycle splitting). Done when: tests.
- [ ] **P5-02 Pattern core** — `pattern.js`: `Pattern(query)`, events with `whole`/`part`/`value`, `pure`, `silence`, `fmap`, `withTime`, `fast`, `slow`, `early`, `late`, `rev`, `stack`, `slowcat` (`cat`), `fastcat` (`seq`), `appLeft/appRight/appBoth` for patterned arguments. Done when: spec examples in CODEBEAT_SPEC §3 pass as tests.
- [ ] **P5-03 Mini-notation parser** — hand-written recursive-descent parser for the grammar in CODEBEAT_SPEC §4 (sequence, `~`, `[ ]`, `< >`, `,`, `*`, `/`, `!`, `@`, `?`, `(k,n,r)`, `:n`), producing patterns with source locations per atom. Done when: grammar test table passes; clear error messages with column numbers.
- [ ] **P5-04 Controls** — `s/sound`, `n`, `note` (names and MIDI numbers), `gain`, `pan`, `speed`, `lpf`, `hpf`, `resonance`, `room`, `delay`, `crush`, `attack/decay/sustain/release`, `cut` (choke), `bank`. Done when: tests.
- [ ] **P5-05 Transformations tier 1** — `every`, `sometimes/sometimesBy/often/rarely`, `degradeBy`, `off`, `superimpose`, `iter`, `ply`, `palindrome`, `add/sub/mul`, `scale`, `struct`, `mask`, `euclid`. Done when: tests.
- [ ] **P5-06 Signals** — `sine`, `cosine`, `saw`, `square`, `tri`, `rand`, `irand`, `perlin` with `.range`, `.segment`. Seeded randomness (same seed → same pattern). Done when: tests.

### Runtime (C++)

- [ ] **P5-07 QuickJS integration** — add quickjs-ng via FetchContent; `codebeat/JsRuntime` (own thread, memory limit 64 MB, interrupt handler for time budgets: eval 200 ms, query 5 ms), no std/os modules, JS library embedded via `juce_add_binary_data`. Done when: sandbox tests (no file access, infinite loop interrupted).
- [ ] **P5-08 Evaluation** — user code evaluated as a script; its completion value (or `play(...)`) is the pattern; DJ hooks injected (`deckA.bpm`, `deckA.key`, `deckA.beat`, `master.bpm`); `setbpm()`, `hush()`. Errors returned with line/column. Done when: tests for good/bad code.
- [ ] **P5-09 Pattern bridge + scheduler** — every 25 ms the code-deck thread queries the next 100 ms of cycles, converts event onsets to sample times via MasterClock (1 cycle = 1 bar = 4 beats by default; `setcpb()` to change), and pushes `ScheduledEvent`s into the engine's event queue. Late events (missed window) are dropped and counted. Done when: event-timing test: kick on every beat lands ±1 sample at 128 BPM for 10 min; jitter stats logged.
- [ ] **P5-10 Hot swap** — Ctrl+Enter swaps the pattern at the next cycle boundary (option: immediately); scheduler continues from its cursor so nothing doubles or drops. Done when: swap test shows no duplicated or missing onset.
- [ ] **P5-11 Sound bank** — names map to Beat Maker drum synths (`kick`, `snare`, `clap`, `hat`, `ohat`, `tom`, `rim`, `cow`, …), synth waveforms (`sine`, `saw`, `square`, `tri`, `supersaw`, `subbass`), and user sample folders (`Samples\<name>\*.wav`, index via `:n`). Done when: unknown sound produces a warning, not silence-with-no-feedback.
- [ ] **P5-12 Code deck channel** — own mixer channel strip, cue, crossfader assign, FX target; reverb/delay send buses for `room`/`delay`. Done when: code deck crossfades against a track.

### Editor (C++/JUCE)

- [ ] **P5-13 Editor component** — `juce::CodeEditorComponent` with a custom tokeniser (JS + highlighted mini-notation strings), line numbers, multiple tabs/buffers saved to `Documents\Devil-DJ\Code`. Done when: buffers persist.
- [ ] **P5-14 Keys + feedback** — Ctrl+Enter evaluate, Ctrl+. hush, Ctrl+Shift+Enter evaluate immediately; inline error marker with message; last good pattern keeps playing; status line (cycle, cps, events/s, late events). Done when: manual test.
- [ ] **P5-15 Active highlighting** — the code is pre-tokenized to record each string literal's source offset; atoms carry locations; currently sounding atoms are highlighted. Done when: highlight follows a `s("kick snare hat*2")` pattern in time.
- [ ] **P5-16 Autocomplete + help** — function and sound-name completion, hover help from a JSON docs table, examples browser with 15 starter patterns (our own). Done when: manual test.
- [ ] **P5-17 Render** — render N cycles offline to a sampler slot or WAV. Done when: render matches real-time output for a deterministic pattern.

**Beta gate checklist:**

- [ ] Live-code a 4-layer pattern locked to Deck A's grid and key, crossfade from the track to the code deck and back.
- [ ] Introduce a syntax error mid-set: the deck keeps playing, error shown inline.
- [ ] Three outside testers complete a scripted session; feedback logged in `docs/BETA_FEEDBACK.md`.

---

## Phase 6 — Stems and smart features (weeks 23–27)

- [ ] **P6-01 ONNX Runtime integration** — add ONNX Runtime with the DirectML execution provider (prebuilt package), `analysis/InferenceSession` wrapper with CPU fallback. **Before merging, confirm the licence of every model weight file (owner decision).** Done when: a toy model runs on CPU and DirectML.
- [ ] **P6-02 Beat tracker v2 (optional upgrade)** — Beat This! (MIT code + weights) as an alternative analyzer: port its spectrogram front-end exactly from the reference repo and verify against reference outputs; choose per track by confidence. Done when: eval set accuracy beats v1 or v2 is dropped.
- [ ] **P6-03 Stem separation job** — HTDemucs ONNX in the background, chunked with overlap-add, 4 stems (vocals, drums, bass, other) stored as 16-bit FLAC in the cache; progress UI; "fast" vs "quality" setting. Done when: separation of a 5-min track completes and stems sum back to the original within −30 dB error; timing on the target laptop logged.
- [ ] **P6-04 Stem playback** — a deck plays up to 4 synchronized stem buffers (streamed from cache, 16-bit in RAM); per-stem mute/solo, level (stem ISO), and FX send; stem waveform view. Done when: 2 decks × 4 stems within CPU/RAM budget (log numbers).
- [ ] **P6-05 Drum Capture + Drum Swap** — capture 4/8/16/32 beats of the drums stem to a slot; Drum Swap banks (8 slots per deck) replace the deck's drums stem with a captured loop or a Beat Maker pattern, beat-synced; single/multi mode. Done when: swap is on the bar, no clicks.
- [ ] **P6-06 Phrase detection** — novelty curve from chroma + energy self-similarity, boundaries snapped to 8/16/32-bar grid, energy level per phrase; markers on waveform. Done when: eval-set boundary F-measure reported; markers editable.
- [ ] **P6-07 Smart cues** — suggest hot cues at phrase starts (first downbeat, energy rises/drops, outro) and learn the owner's habits from tracks she has cued (relative position to phrase/energy features). Done when: suggestions accepted/rejected UI; acceptance rate logged.
- [ ] **P6-08 Mix points** — mark MIX IN/MIX OUT on tracks; auto-transition and Automix use them. Done when: Automix honours marked points.
- [ ] **P6-09 Vocal presence** — per-phrase vocal activity from the vocal stem energy; shown on the waveform; stored as `vocal_intro` / `vocal_outro` in `track_features`. Done when: markers visible; values stored.
- [ ] **P6-10 Matcher v2 features** — MATCHER_SPEC §4.6–4.7: vocal clash, mixability, and energy/sound computed over the real mix-out region of A and mix-in region of B (from phrases and mix points). Done when: feature tests; suggestions change when A enters its outro.
- [ ] **P6-11 Choice logging + thumbs** — MATCHER_SPEC §7.1: impressions, outcomes (mixed ≥ 60 s, ejected), 👍/👎 keys; settings to disable, delete or export logs (CSV/JSON). Done when: a session produces correct log rows; export opens in Python/pandas.
- [ ] **P6-12 Personal ranker v1** — MATCHER_SPEC §7.2: pairwise logistic ranker with non-negative weights, prior towards the preset, blending α = n/(n+200), background training at exit; "What you care about" weights view with reset. Done when: synthetic-log test recovers the weight ordering; training < 1 s for 10k pairs.
- [ ] **P6-13 Matcher evaluation tool** — `tools/matcher_eval` per MATCHER_SPEC §10 (offline replay HitRate@1/5/10, MRR, ablations) writing `docs/reports/matcher-<date>.md`. Done when: first report generated from the owner's real history; presets retuned from it.

**Phase 6 gate:** mix two tracks using stems (drums from A, vocals from B), Drum Swap a Beat Maker pattern into a track, Automix uses phrase-aware mix points, and after two logged sessions the personal ranker beats the Balanced preset in the evaluation report (or the report explains why not).

---

## Phase 7 — Pro extras (weeks 28–31)

- [ ] **P7-01 4-deck mode** — decks C/D, 4-channel mixer layout, crossfader assign per channel. Done when: 4 decks + sampler + code deck play without xruns.
- [ ] **P7-02 MIDI Learn** — map any `CommandId` or continuous parameter to MIDI CC/note; soft takeover; LED feedback; mapping files; presets for a couple of common generic controllers. Done when: mapping round-trip and soft-takeover tests.
- [ ] **P7-03 Microphone** — mic input channel with EQ, talkover ducking, simple feedback reducer (notch on detected howl), mic to recording. Done when: manual test.
- [ ] **P7-04 Edit mode** — single-track edit timeline on the beatgrid: cut/copy/paste/delete phrases, extend intro/outro, palette of sections, render to a new library track. Done when: rendered edit is sample-accurate at cut points.
- [ ] **P7-05 Tempo sync with other apps** — evaluate Ableton Link (check its licence against D1 first) or MIDI clock out/in. Done when: one of them works with a DAW.
- [ ] **P7-06 ASIO** — if D7 allows: enable JUCE ASIO support behind a build flag. Done when: works with a common USB interface.
- [ ] **P7-07 Neural audio embeddings** — MATCHER_SPEC §8: evaluate the candidate models (**confirm weight licences first — owner decision**), export the winner to ONNX, background `EmbeddingJob` for whole/intro/outro, PCA to 128-d float16, f_sound switches to embeddings when present. Done when: evaluation report shows the effect on HitRate; fallback to v1 vector works.
- [ ] **P7-08 Energy arcs + Smart Sort** — MATCHER_SPEC §9.1–9.2: arc presets and custom curve; beam search + or-opt ordering into a new playlist; flow view with weakest transitions highlighted. Done when: n = 8 test within 2 % of brute force; 300-track playlist sorts in ≤ 2 s.
- [ ] **P7-09 Bridge** — MATCHER_SPEC §9.3: background kNN graph (`track_neighbors`), Dijkstra paths of 1–3 hops to a chosen target, UI "Get me to…" from the browser context menu. Done when: hand-built graph test; paths shown within 200 ms.
- [ ] **P7-10 Video out (stretch)** — second-window visualizer driven by beat phase and spectrum (no video files in v1). Done when: runs on a second monitor at 60 fps. Move to Backlog if the phase runs late.
- [ ] **P7-11 Phone remote (stretch)** — local web server (WebSocket) with a touch controller page for crossfader/pads. Done when: phone on the same Wi-Fi controls the crossfader with < 30 ms latency. Move to Backlog if the phase runs late.

---

## Phase 8 — Polish and release (weeks 32–35)

- [ ] **P8-01 Visual identity** — own logo, icon set, colour themes (dark/light), typography; no third-party DJ brand resemblance. Done when: owner approves.
- [ ] **P8-02 Onboarding** — first-run wizard: audio device + headphone mode, music folder import, keymap tour.
- [ ] **P8-03 Performance pass** — profile audio callback, UI frame time, startup time (< 3 s with 20k tracks), memory with 4 decks.
- [ ] **P8-04 Reliability pass** — fuzz file decoding with corrupt files, DB corruption recovery, unplugging the audio device mid-set (auto-recover).
- [ ] **P8-05 Accessibility + localization** — keyboard focus order, UI scale setting, strings in a table; English + Korean UI.
- [ ] **P8-06 Installer** — Inno Setup script, per-user install, file associations for `.ddjbeat`, uninstaller keeps the library unless asked.
- [ ] **P8-07 Code signing** — sign exe and installer (certificate or cloud signing service); verify SmartScreen behaviour.
- [ ] **P8-08 Updates** — update check against a release JSON on the website; optional auto-update library (licence check first).
- [ ] **P8-09 User guide** — `docs/user-guide/` (getting started, keymap, Beat Maker, Code Beat tutorial).
- [ ] **P8-10 Crash reporting** — opt-in upload of minidumps + logs.
- [ ] **P8-11 Release candidate** — 2-week beta with testers, fix-only branch, tag `v1.0.0`.

**1.0 gate:** signed installer on the website; clean install on a fresh Windows 11 VM; MVP + Beta checklists re-run on the release build.

---

## Backlog (after 1.0)

Streaming integrations (need partner agreements), cloud library sync, DVS (timecode vinyl), lighting/DMX, CDJ/USB export (proprietary formats — likely never), macOS port (JUCE makes it feasible), plugin hosting (VST3 is MIT since 3.8), collaborative B2B over network, AI-assisted pattern suggestions in Code Beat, more Code Beat functions (tier 2/3 in spec), Track Matcher ranker v2 (GBDT/MLP in ONNX) and learned embedding projection (MATCHER_SPEC §7.3), research: transition model pre-trained on public DJ-mix analyses (dataset licences first).

## Risk register (review at every gate)

| Risk | Signal to watch | Response |
| --- | --- | --- |
| Scope creep | Phase overruns > 1 week | Cut to the gate checklist; move extras to Backlog |
| Audio dropouts | xruns in soak tests | Larger default buffer, profile, move work off audio thread |
| Key-lock quality | Listening tests | Tune Signalsmith presets; consider commercial Rubber Band licence |
| Beatgrid accuracy | Eval report < 90 % | Improve v1 tracker; prioritise P6-02 |
| Licence contamination | CI license check | Remove dependency; owner decision on D1 |
| Stem CPU/RAM | P6-04 numbers | Offline-only stems, 16-bit storage, limit to 2 stem decks |
