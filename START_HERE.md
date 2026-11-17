# Start here — building Devil-DJ with Claude Code

These files are the full plan. Put them in a new repository, open it with Claude Code, and work through `docs/PLAN.md` one task at a time.

## 1. Install the tools (Windows 10/11, once)

| Tool | Why | Notes |
| --- | --- | --- |
| Visual Studio 2022 Community | C++ compiler, debugger | Workload: **Desktop development with C++** (includes CMake and the Windows SDK) |
| Git | Version control | Default options |
| vcpkg | SQLite, TagLib, Catch2 | `git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg`, run `bootstrap-vcpkg.bat`, set the `VCPKG_ROOT` environment variable to `C:\dev\vcpkg` |
| Node.js 20+ | Code Beat JS tests | LTS installer |
| Python 3.11+ | Test-signal generator | Tick "Add to PATH"; then `pip install numpy soundfile` |
| Claude Code | Your coding agent | Follow the official install guide for Windows |

## 2. Create the repository

```powershell
mkdir C:\dev\devil-dj
# copy CLAUDE.md, START_HERE.md, README.md and the docs\ folder into C:\dev\devil-dj
cd C:\dev\devil-dj
git init
git add .
git commit -m "Plan and specs"
```

Create a **private** GitHub repository and push it (`git remote add origin ...`, `git push -u origin main`), so CI can build installers from Phase 0 onward.

## 3. First Claude Code session

Open a terminal in `C:\dev\devil-dj` and start Claude Code. Paste these prompts one at a time.

**Prompt 1 — orientation**

```
Read CLAUDE.md, docs/PLAN.md and docs/ARCHITECTURE.md. Summarize the project in
five bullet points and list the Phase 0 tasks. Don't write any code yet.
```

**Prompt 2 — first task**

```
Do task P0-01 only. Show me your plan first and wait for my OK. After that,
implement it, tick the box in docs/PLAN.md, add a DEVLOG entry and commit.
```

**Prompt 3 — the build**

```
Do P0-02 and P0-03. When the build works, tell me exactly how to run the exe
and what I should see.
```

## 4. The everyday loop

```
Do the next unchecked task in the current phase of docs/PLAN.md.
Plan first, then implement, build, run all tests, tick the box,
add a DEVLOG entry and commit with the task ID.
```

Useful extra prompts:

- "Explain the code you just wrote as if I'm new to C++."
- "Run the soak test from P1-20 and tell me the numbers."
- "Before we leave Phase N, go through its gate checklist with me."
- "Something sounds wrong: <describe it>. Find the cause, add a test that fails, then fix it."

## 5. Tips

- One task per session keeps Claude Code focused; start a new session when the conversation gets long.
- Always test audio on your own laptop with headphones; CI cannot hear clicks.
- Prepare a private folder of about 50 of your own tracks with known BPM and key, and set `DEVILDJ_EVAL_DIR` to it before Phase 2. Never commit music.
- Answer the open decisions in `docs/PLAN.md` (D1 open vs closed source is the important one) before Phase 5 at the latest.
- If a task turns out bigger than planned, ask Claude Code to split it into sub-tasks in `docs/PLAN.md` before continuing.
