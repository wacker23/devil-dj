# Devil-DJ Dev Log

One entry per finished task, newest first. Keep each entry to a few lines: what changed, numbers measured, anything surprising.

Template:

```
## YYYY-MM-DD · P0-00 Task title
- What changed:
- Tests / measurements:
- Notes / follow-ups:
```

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
