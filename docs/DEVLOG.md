# Devil-DJ Dev Log

One entry per finished task, newest first. Keep each entry to a few lines: what changed, numbers measured, anything surprising.

Template:

```
## YYYY-MM-DD · P0-00 Task title
- What changed:
- Tests / measurements:
- Notes / follow-ups:
```

## 2026-11-18 · P0-11 Settings store
- What changed: `app/Settings` wraps `juce::PropertiesFile` at `%APPDATA%\Devil-DJ\settings.xml`. Typed keys `SettingKey<T>{name, default}` (bool/int/double/juce::String) declared in one place, `app/SettingsKeys.h` (`mainWindowState`, `showDebugMenu`). `get`/`set`/`reset`/`isStored`, `Settings::Listener` notified only when a value really changes, autosave 500 ms after a change + on destruction, `save()` for immediate writes. MainWindow and Main use it; the Debug menu can now also be switched on in Release with the `showDebugMenu` setting. Startup logs the settings path.
- Tests / measurements: 7 Settings tests (defaults for test keys and for every app key, survive restart incl. Korean text, immediate save, listeners on real changes only, explicit default is stored, real path is `%APPDATA%\Devil-DJ\settings.xml`). Manual: Release app resized to 1400×900 at (300,200), closed, reopened → same rect. ctest 57/57 Debug, 51/51 Release, 0 warnings.
- Notes / follow-ups: the tests compile `src/app/Settings.cpp` directly because JUCE modules add their sources to every target that links them, so a second static library linking JUCE would duplicate symbols. Before engine/ui code needs JUCE in several targets (P1-02), set up one shared JUCE static library that the app and tests both link.

## 2026-11-18 · P0-10 Logging + crash dumps
- What changed: `util/Log` — async logger (callers format, a writer thread does all file I/O), `logInfo/logWarning/logError/logDebug` with `std::format`, file `<dir>/devil-dj.log`, new file per session (previous → `.1.log`), size rotation (4 MB × 5 files by default), bounded queue (10k lines, overflow counted), messages before `start()` are kept, `flush()`/`stop()` drain everything. In a `ScopedRealtimeContext` messages are dropped before formatting (counted). `util/CrashHandler` — `SetUnhandledExceptionFilter` + `std::set_terminate`: writes `devil-dj-crash-<date>-<time>-<pid>.dmp` (MiniDumpWriteDump, normal + indirect memory + thread info) and a `[FATAL]` line via the lock-free `Log::writeEmergency`; crash path doesn't allocate. App: logs to `%LOCALAPPDATA%\Devil-DJ\logs` from startup to shutdown; temporary **Debug** menu (Open log folder, Crash test on UI / worker thread), shown in Debug builds or with `--debug-menu`.
- Tests / measurements: 8 Log tests (format, pre-start queue, drain on stop, session rotation, size rotation keeps 3 files ≤ limit, 4 threads × 1000 lines no loss, real-time drop, emergency write) + 2 crash tests (in-process `writeMinidump` = valid `MDMP`; test exe re-launches itself into a hidden test that crashes on a worker thread → exit 0xC0000005, dump present, FATAL line names it). Manual: clicked both Debug-menu crash items (Debug build UI thread, Release `--debug-menu` worker thread): ~375 KB dump + FATAL log line each; normal close logs "shut down cleanly". ctest 50/50 Debug, 44/44 Release, 0 warnings.
- Notes / follow-ups: dbghelp is now linked in all configs (system library). With a debugger attached the debugger gets the exception instead. Hide the Debug menu behind a setting before 1.0. Opt-in upload of dumps is P8-10.

## 2026-11-18 · P0-09 RealtimeGuard
- What changed: `util/RealtimeGuard.{h,cpp}`. In Debug (`DEVILDJ_REALTIME_GUARD=1`, set by CMake for the Debug config only) all 16 replaceable global `operator new/delete` forms (plain, array, nothrow, aligned, sized) check a `thread_local` depth counter; `ScopedRealtimeContext` increments it (nestable). A violation is counted, then the handler runs with the guard off: default handler logs one stack trace per process (CaptureStackBackTrace + DbgHelp symbols, to stderr and OutputDebugString) and `__debugbreak()`s if a debugger is attached (our own jassertfalse equivalent, so util doesn't depend on JUCE). `setRealtimeViolationHandler()` lets tests count instead. `devildj_util` is now a static library (linked before the CRT, so its operator new wins). Release: empty inline stubs, `kRealtimeGuardEnabled == false`.
- Tests / measurements: 7 Debug tests (new/delete, vector growth, aligned/nothrow, outside scope ignored, nesting, per-thread, handler may allocate) + 1 Release test (compiled out, no counting). Hidden demo `DevilDJTests.exe "[.realtime-demo]"` prints the trace; frame #00 is the offending caller. ctest 40/40 Debug, 34/34 Release, 0 warnings.
- Notes / follow-ups: the Release `ScopedRealtimeContext` has a user-provided ctor so `ScopedRealtimeContext guard;` doesn't raise C4101. The app links util once the engine exists (P1-02); the trace goes to util/Log after P0-10.

## 2026-11-18 · P0-08 Lock-free primitives
- What changed: header-only `devildj::util` target with `SpscQueue<T, Capacity>` (power-of-two ring, monotonic acquire/release indices, cached opposite index, cache-line separated; `tryPush` copy/move, `tryPop(T&)` / `tryPop()` → optional), `TripleBuffer<T>` (3 slots, atomic middle index + fresh bit; `write`, `writeBuffer`+`publish`, `read`, `hasNewData`), `GarbageQueue<Capacity>` (audio thread queues `{deleter, ptr}` pairs via `tryRelease`/`tryReleaseWith`; housekeeping calls `collect()`; full queue → false + `failedReleases()` counter; destructor collects the rest). Removed the P0-01 `StyleExample.h`.
- Tests / measurements: 16 new tests. SpscQueue stress 10M items over 2 threads, no loss/reorder: 0.02–0.04 s Release, 0.24 s Debug. TripleBuffer 2M publishes, no torn reads or going backwards. GarbageQueue 200k objects released/collected across threads, none leaked. Stress tests repeated 20× in Release: all pass. ctest 33/33 in Debug and Release, 0 warnings.
- Notes / follow-ups: MSVC C4324 (padding from `alignas`) is disabled locally in SpscQueue.h because the padding is the point. Housekeeping timer that calls `collect()` comes with the engine (P1-02).

## 2026-11-18 · P0-07 License checker
- What changed: `tools/check_licenses.py` (stdlib only) collects FetchContent names from `cmake/Dependencies.cmake`, `vcpkg.json` dependencies, and every port installed in the build's vcpkg tree (catches transitive zlib/utfcpp), then checks each against a new machine-readable "Manifest packages" table in `docs/LICENSES.md`. Fails on unlisted packages, forbidden families (GPL/AGPL/LGPL/NC/SSPL/...) or licences outside the allowed set. Registered in ctest as `tools.check_licenses` and `tools.check_licenses.unittests`.
- Tests / measurements: 11 Python unit tests (incl. fake GPL row, unlisted vcpkg/FetchContent/transitive package, commented-out declaration). Manual: appending `fake-gpl-lib` to vcpkg.json makes `ctest` fail; restoring it passes. ctest 17/17 in Debug and Release.
- Notes / follow-ups: CI (P0-06) was skipped for now (no GitHub repo yet, D3); since the check is a ctest test, CI picks it up automatically once it exists.

## 2026-11-18 · P0-05 Test harness
- What changed: `tests/` has one folder per src module (app, engine, dsp, analysis, library, beatmaker, codebeat, matcher, ui, util), each with a `*SmokeTests.cpp` tagged `[smoke][<module>]`. One `DevilDJTests` exe (`juce_add_console_app`, links Catch2WithMain + juce_core + SQLite + xxHash). `enable_testing()` + `catch_discover_tests(... DISCOVERY_MODE PRE_TEST)` registers each TEST_CASE with ctest. The P0-04 link tests moved into `tests/library/`.
- Tests / measurements: `ctest --preset windows-x64-debug` and `-release`: 15/15 pass in ~0.5 s. 0 build warnings.
- Notes / follow-ups: the smoke tests check assumptions later code relies on (lock-free atomics, trivially copyable command variant, seeded RNG, UTF-8 Korean strings, SQLite FTS5 + thread safety). Real module tests replace or join them as each module gets code.

## 2026-11-18 · P0-04 vcpkg manifest
- What changed: `vcpkg.json` (catch2, sqlite3 with fts5, taglib, xxhash; baseline 0df93a05), preset uses `$env{VCPKG_ROOT}` toolchain with triplet `x64-windows-static-md` (static libs, no DLLs to ship). `find_package` calls in `cmake/Dependencies.cmake`. `tests/DevilDJTests` links Catch2 + SQLite + xxHash; the app links `TagLib::tag` and shows the TagLib version in a temporary build-info line.
- Tests / measurements: 3 test cases / 7 assertions pass in Debug and Release (SQLite in-memory query, FTS5 compiled in, xxHash determinism). Debug + Release build with 0 warnings. vcpkg package build added ~2 min to the first configure.
- Notes / follow-ups: vcpkg cloned to `C:\devcpkg`, `VCPKG_ROOT` set for the user. TagLib pulls in zlib (Zlib) and utfcpp (BSL-1.0); both added to LICENSES.md. Tests aren't registered with ctest yet (P0-05).

## 2026-11-18 · P0-03 App target
- What changed: `src/app/` Main.cpp (JUCEApplication, single instance, dark LookAndFeel_V4), MainWindow (resizable, native title bar, min 960×600, saves/restores window state), MainComponent (empty, draws the name). Window state lives in `%APPDATA%\Devil-DJ\settings.xml` via a plain `juce::PropertiesFile` until P0-11 wraps it.
- Tests / measurements: launched Debug exe, resized to 1100×700 at (150,120), closed, relaunched → restored at 1100×699 (1 px frame rounding). Exits with code 0.
- Notes / follow-ups: exe path matches CLAUDE.md: `build\windows-x64\srcpp\DevilDJ_artefacts\Debug\Devil-DJ.exe`.

## 2026-11-18 · P0-02 CMake + presets
- What changed: top-level CMakeLists (C++20, `devildj::compile_options` with `/W4 /permissive- /utf-8 /MP`, `DEVILDJ_WARNINGS_AS_ERRORS` option for CI), CMakePresets (configure `windows-x64`, build + test presets debug/release), `cmake/Dependencies.cmake` with JUCE 8.0.15 via FetchContent (shallow).
- Tests / measurements: configure 95 s first time (JUCE download + juceaide). Debug and Release build with 0 warnings. MSVC 19.50 (VS 2026 Community), CMake 4.1.1 bundled, Windows SDK 10.0.26100.
- Notes / follow-ups: generator is "Visual Studio 18 2026" instead of 2022 (owner's machine). CI (P0-06) needs a runner image with VS 2026, or a second preset for 2022. VS's CMake isn't on PATH by default; use a Developer PowerShell or add `...\Common7\IDE\CommonExtensions\Microsoft\CMake\CMakein`.

## 2026-11-18 · P0-01 Repository skeleton
- What changed: folder tree from CLAUDE.md (src/*, js/codebeat, tests, tools, installer, cmake, docs/reports; empty folders hold `.gitkeep`), `.gitignore`, `.gitattributes` (LF text, binary audio), `.editorconfig`, `.clang-format` (LLVM base, Allman braces, 4-space, 120 cols), `src/util/StyleExample.h`.
- Tests / measurements: `clang-format --dry-run --Werror src/util/StyleExample.h` passes (clang-format 23.1.5 from `pip install --user clang-format`; dev tool only, not shipped).
- Notes / follow-ups: owner's machine has Visual Studio **2026** (v18) without the C++ workload; P0-02 presets will target "Visual Studio 18 2026" instead of 2022.

## 2026-11-18 · Planning: Track Matcher
- Added docs/MATCHER_SPEC.md and tasks P2-24, P3-19 (rewritten), P3-24, P3-25, P6-09…P6-13, P7-07…P7-09. Old P7-07/P7-08 (video, phone remote) are now P7-10/P7-11 (stretch).

## 2026-11-17 · Planning
- Plan, architecture and module specs written. Framework decided: C++20 + JUCE 8 (D2).
- Next: Phase 0, task P0-01.
