# Devil-DJ Dev Log

One entry per finished task, newest first. Keep each entry to a few lines: what changed, numbers measured, anything surprising.

Template:

```
## YYYY-MM-DD · P0-00 Task title
- What changed:
- Tests / measurements:
- Notes / follow-ups:
```

## 2026-11-18 · P0-01 Repository skeleton
- What changed: folder tree from CLAUDE.md (src/*, js/codebeat, tests, tools, installer, cmake, docs/reports; empty folders hold `.gitkeep`), `.gitignore`, `.gitattributes` (LF text, binary audio), `.editorconfig`, `.clang-format` (LLVM base, Allman braces, 4-space, 120 cols), `src/util/StyleExample.h`.
- Tests / measurements: `clang-format --dry-run --Werror src/util/StyleExample.h` passes (clang-format 23.1.5 from `pip install --user clang-format`; dev tool only, not shipped).
- Notes / follow-ups: owner's machine has Visual Studio **2026** (v18) without the C++ workload; P0-02 presets will target "Visual Studio 18 2026" instead of 2022.

## 2026-11-18 · Planning: Track Matcher
- Added docs/MATCHER_SPEC.md and tasks P2-24, P3-19 (rewritten), P3-24, P3-25, P6-09…P6-13, P7-07…P7-09. Old P7-07/P7-08 (video, phone remote) are now P7-10/P7-11 (stretch).

## 2026-11-17 · Planning
- Plan, architecture and module specs written. Framework decided: C++20 + JUCE 8 (D2).
- Next: Phase 0, task P0-01.
