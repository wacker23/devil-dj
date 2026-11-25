# Devil-DJ — full manual test (end of Phase 6)

A step-by-step test of everything built in Phases 0–6, for the owner to run on their own laptop. Each step says what to do and what should happen. Work top to bottom: later parts reuse what earlier parts set up.

**How to record results.** Keep a copy of the results form at the end (or a text file). For every step write **P** (pass), **F** (fail) or **S** (skipped), and for a fail one line on what happened instead. You don't need to explain passes. Send me the fails: I fix them task by task.

**When something goes wrong.** Note the step number and the time, then keep going if you can. F12 shows live numbers (CPU, xruns). The app writes a log and, if it crashes, a crash dump to `%LOCALAPPDATA%\Devil-DJ\logs` (see "Logs" below): tell me the step and I'll ask for the file.

**Time needed.** Parts A–G: about 4–5 hours (can be split over several days). Part H (logged sets): two real sets of 45 minutes or more, on different days. Part I: 15 minutes.

---

## Before you start

- [ ] **0.1** Build the latest version: in the repo folder run
  `cmake --build --preset windows-x64-release` (and `windows-x64-debug` if you use the Debug app).
  The app: `.\build\windows-x64\src\app\DevilDJ_artefacts\Release\Devil-DJ.exe`.
- [ ] **0.2** Have ready: your headphones (and, for one test, a second audio device such as a USB headset); the music folder `C:\Users\sophi\Music\DJ_collection`; a short M3U playlist file if you have one; an XML library export from other DJ software if you have one; 30 minutes of quiet for the listening tests.
- [ ] **0.3** Back up your library first: start the app, Options > **Back up library now...**, note where it saved. (Part B tests restore; this is your safety copy.)
- [ ] **0.4** The stem model is in `Documents\Devil-DJ\Models\htdemucs_core.onnx` (I copied it there in P6-03). Check the file is there (about 174 MB).

## Part A — Start-up and settings (Phases 0–1)

- [ ] **A1** Start the app. *Expect:* the window opens within a few seconds; menus Decks, Mixer, Options; two decks, the mixer, two waveforms, the browser.
- [ ] **A2** Resize and move the window, close the app, start it again. *Expect:* same size and place.
- [ ] **A3** Options > **Audio settings...**: pick your laptop's output, 48 kHz, buffer 256. *Expect:* no error; the status overlay (F12) shows the device, the buffer and a low CPU load.
- [ ] **A4** Debug menu (if shown) > **Test tone 440 Hz on/off**. *Expect:* a clean steady tone; off again.
- [ ] **A5** F12 (status overlay) open for a minute with nothing playing. *Expect:* xruns stay 0.
- [ ] **A6** Hold **Ctrl+/**. *Expect:* the keymap drawn on a keyboard; release hides it.

## Part B — Library and analysis (Phase 2)

- [ ] **B1** Decks > **Add music folder to the library...** > `DJ_collection`. *Expect:* the tracks appear in the browser (about 100); the activity line shows "Analyzing n of …" and counts up.
- [ ] **B2** Wait for analysis to finish (a few minutes; you can go on with B3–B9 meanwhile). *Expect:* BPM, key and duration filled for every track; the activity line clears.
- [ ] **B3** Search: Ctrl+F, type part of an artist name, Enter. *Expect:* the list filters as you type; Enter/Esc gives the keys back to the decks.
- [ ] **B4** Click column headers (BPM, Key, Rating). *Expect:* the list sorts; again reverses.
- [ ] **B5** Make a playlist (right-click a track > Add to playlist > New playlist with these tracks...), add 5 tracks, drag one to reorder, Delete removes one from the playlist (not from the library).
- [ ] **B6** Make a smart playlist (in the playlist tree: new smart playlist) for "BPM between 120 and 128". *Expect:* the matching tracks, updated when ratings or tags change.
- [ ] **B7** Rate 3 tracks (Ctrl+Shift+1…5, or click the stars), give one a colour and a tag (right-click > Colour / Tags). *Expect:* shown in the list; survive a restart.
- [ ] **B8** Decks > **Import playlist (M3U / M3U8)...** (if you have one). *Expect:* a playlist with the tracks that are in your library; a count of missing ones.
- [ ] **B9** Decks > **Import library from other software (XML)...** (if you have an export). *Expect:* playlists, cues and grids imported for tracks found on disk; a summary.
- [ ] **B10** Rename one file in Explorer while the app is closed, start it, Decks > **Check for missing files**. *Expect:* it shows under "Missing files" in red. Decks > **Relocate missing files...**, pick its folder. *Expect:* found again (matched by name and size). Rename it back afterwards if you like.
- [ ] **B11** Options > **Restore library from a backup...** with the backup from 0.3. *Expect:* asks, restarts, the library as it was at 0.3 (then redo B5–B7 only if you need them later; nothing else depends on them).
- [ ] **B12** Close and start the app. *Expect:* starts in a few seconds with the whole library; nothing re-analyzed.

## Part C — Decks and mixer (Phases 1–2)

Load with **Tab** (deck A) / **Enter** (deck B) from the browser, or drag a track onto a deck.

- [ ] **C1** Load a track on A. *Expect:* title, artist, BPM, key, the overview waveform and the big waveform; ready in about a second.
- [ ] **C2** X play / pause; Z cue (paused at the cue point; hold Z to preview, release returns). *Expect:* no clicks on start/stop.
- [ ] **C3** Tempo: drag the tempo slider; Shift+1 / Shift+2 (0.1 % steps); 1 / 2 pitch bend while held. Decks > Tempo range (6 / 10 / 16 / 50 %). *Expect:* the BPM shown follows; bend returns when released.
- [ ] **C4** Key lock (Ctrl+W or the KEY LOCK button) at +8 % tempo. *Expect:* the pitch stays the same; off: it rises with the tempo.
- [ ] **C5** Drag the big waveform while playing (scratch), and paused (scrub); mouse wheel on it (nudge); Ctrl+wheel (zoom). *Expect:* the sound follows the hand without stutter; release resumes.
- [ ] **C6** Mixer: TRIM, HI / MID / LOW, FILTER, the channel fader. Ctrl+A / S / D kill low / mid / high. Mixer > **Isolator EQ** and back. *Expect:* each does what it says, no clicks.
- [ ] **C7** Load B, play both. Crossfader with G / H (hold glides), B centre; Mixer > Crossfader curve: Smooth / Linear / Cut. *Expect:* smooth handover; Cut switches sharply.
- [ ] **C8** Headphones: CUE on deck B (Ctrl+O), CUE/MST knob, PHONES level. Options > Audio settings: an output layout that routes the cue (split, or a second device). *Expect:* deck B in the headphones only, the master in the speakers.
- [ ] **C9** With a second audio device: Options > Audio settings > the separate headphone device. *Expect:* cue on the second device in sync, no crackle for 5 minutes.
- [ ] **C10** Sync: C on deck B while A plays. *Expect:* B's tempo matches A, beats line up, and stay lined up for a minute. Ctrl+R / Ctrl+P makes a deck the master.
- [ ] **C11** Quantize (` key) on; beat jump 5 / 6 (A) and - / = (B); jump size Shift+6. *Expect:* jumps land on the beat, still in sync.
- [ ] **C12** Key Sync (Shift+C) on deck A. *Expect:* A transposes to B's key (key lock turns on); Ctrl+1 / Ctrl+2 move a semitone.
- [ ] **C13** Beatgrid: on a track whose grid looks off, GRID button tools (downbeat here, earlier / later, ×2 / ÷2) or the Ctrl+Shift keys; tap tempo (Ctrl+Shift+X, 4+ taps); metronome (Ctrl+Shift+C). *Expect:* the beat lines move, the grid locks (analysis leaves it alone), the metronome clicks on the beats in the headphones.
- [ ] **C14** Options > Auto Gain on. Load a quiet track and a loud one. *Expect:* similar loudness (the trim was set on load).
- [ ] **C15** Esc (panic) while holding a bend and a loop. *Expect:* everything released.

## Part D — Performance tools (Phase 3)

- [ ] **D1** Hot cues (F1, pads Q W E R / A S D F for deck A): an empty pad sets a cue at the playhead; the pad jumps there; Shift+pad deletes; right-click a pad: colour, name, sort. F1 again: pads I–P. *Expect:* cues kept after reloading the track and after a restart.
- [ ] **D2** Memory cues: Ctrl+Z stores one; Ctrl+3 / Ctrl+4 call previous / next; the MEM button lists them. *Expect:* the CUE point moves there.
- [ ] **D3** Loops: V auto loop (4 beats), 3 / 4 halve / double, Ctrl+X / Ctrl+C loop in / out, Shift+V reloop / exit. *Expect:* seamless loops on the beat.
- [ ] **D4** Slip (Ctrl+Q), then loop for a few bars and exit; scratch and release. *Expect:* playback continues where the track would have been.
- [ ] **D5** Reverse (hold Shift+X, or REV); brake Ctrl+Shift+D; spin-back Ctrl+Shift+F. *Expect:* each sounds right and ends paused (brake / spin-back).
- [ ] **D6** Pad modes: F2 Beat Loop, F3 Beat Jump, F4 Slicer (hold a pad: the slice rolls; let go: back in time), Shift+F1 Key Shift, Shift+F2 Keyboard. *Expect:* each as described in KEYMAP.md.
- [ ] **D7** FX panel (FX tab, left of the effects): FX1 on deck A, pick an effect, F5 on/off, F6 next effect, WET and the beat length; Shift+F5 cycles the target. Try 4 different effects. *Expect:* in time with the beat, tails ring out after off.
- [ ] **D8** Color FX: the FILTER button in the middle of the mixer picks what the channels' FILTER knobs do; pick another effect, turn a FILTER knob. *Expect:* that effect instead of the filter; back to FILTER gives the filter again.
- [ ] **D9** Release FX: in the FX panel, hold ECHO OUT / BRAKE / SPIN / SWEEP. *Expect:* the effect takes the track out; let go ends it.
- [ ] **D10** Pad FX (Shift+F3): hold pads. *Expect:* each pad's effect while held; right-click a pad to edit its preset.
- [ ] **D11** Sampler (SAMPLER tab): load a sample into a slot, play it (Shift+F4 pad mode, or the panel), ONE / LOOP / GATE, SYNC. *Expect:* plays in time when synced.
- [ ] **D12** Capture: in a loop, capture it to the sampler (loop "..." menu). *Expect:* a new sampler slot loops in time.
- [ ] **D13** Sequencer: SAMPLER tab > REC, play sampler pads over a bar, SEQ plays it back; Sequence Call pad mode (pad mode menu). *Expect:* the pattern repeats in time.
- [ ] **D14** Recording (REC tab, F9): record 2 minutes of mixing, stop. *Expect:* a WAV (or the chosen format) in the folder the panel shows; plays back without gaps.
- [ ] **D15** Auto-transition: A plays, B loaded; T. *Expect:* B comes in on a phrase line, synced; the crossfader and EQ move by themselves; T again (or Shift+T) cancels. Mixer > Auto-transition (T) sets length and style.
- [ ] **D16** Automix (AUTOMIX tab, F10): select 5 tracks in the browser, QUEUE, AUTOMIX on. *Expect:* plays them one into the next, unattended, every transition on the beat. Try REPEAT, RANDOM, and SMART (it picks the best next track each time).
- [ ] **D17** Next Track panel (Options > Layout > Next Track panel): with A playing, suggestions with reasons. N loads the highlighted one on the idle deck; Shift+N moves down; hold Ctrl+N previews in the headphones; Ctrl+Shift+N cycles the intent; the preset and scope boxes. *Expect:* sensible suggestions with reason chips; the list changes with the intent.
- [ ] **D18** Options > Touch mode. *Expect:* everything larger; fingers (if you have a touch screen) work on pads and faders, two faders at once.
- [ ] **D19** Options > Layout: waveforms across / upright; Ctrl+B enlarged browser. *Expect:* the layout changes and is kept after a restart.
- [ ] **D20** Options > **Edit keymap...**: move one action to another key, SAVE, use it; DEFAULT puts it back. *Expect:* the new key works at once.

## Part E — Beat Maker, transitions, Spotify (Phase 4)

Ctrl+Tab switches the bottom panel: Browser → Beat Maker → MIX → CODE.

- [ ] **E1** Beat Maker: click steps for kick, snare, hats (8 drum tracks); Space starts / stops on the bar. *Expect:* plays locked to the master tempo.
- [ ] **E2** Change sounds (Shift+Left / Right), velocity (Shift+Up / Down), probability and micro-timing (Alt+arrows), ratchet (R), swing (- / =), mute / solo (M / S).
- [ ] **E3** Tab to the synth view: draw a bassline in the piano roll (click to add, drag to move / resize), chord stamp (C), scale (K), arpeggiator (A).
- [ ] **E4** Ctrl+Z / Ctrl+Y undo / redo several edits.
- [ ] **E5** Patterns A–H (Ctrl+1…8): make B different from A; switch while playing (on the next bar); song mode (B) with A A B A.
- [ ] **E6** The Beat Maker's mixer channel: fader, EQ, CUE, FX target (Shift+F5 to Beat Maker). *Expect:* like a deck channel.
- [ ] **E7** Live record (Ctrl+R in the Beat Maker), play drum pads 1–8. *Expect:* hits land on the nearest steps.
- [ ] **E8** Export (Ctrl+E): pattern to WAV, pattern to the sampler (a loop), song to the library. *Expect:* files in `Documents\Devil-DJ\Beats\Renders`; the sampler slot loops in time; the library track gets analyzed.
- [ ] **E9** User samples: Ctrl+O on a drum track (or drop a WAV on a row). *Expect:* the sample plays on that track.
- [ ] **E10** Transition editor (MIX panel) with A playing and B loaded: A (Auto) plans it; edit a lane point (arrows), change a shape (S), length (L), preset (P); Space previews; Ctrl+S saves it as a preset. Then T plays it. *Expect:* the transition follows the curves; the FX lane's effect (F) and loop out (O) work.
- [ ] **E11** Saved transitions in Automix: save a transition for a pair, queue that pair in Automix. *Expect:* Automix plays your saved one, starting where you placed it. (Regression check for a bug fixed in P6-08: it must start at the planned bar, not 32 beats before the end.)
- [ ] **E12** Spotify (Options > Spotify): sign in, Import playlists. *Expect:* your Spotify playlist as a Devil-DJ playlist made of your own files that match; a list of unmatched titles. (No Spotify audio is ever played.)
- [ ] **E13** **Phase 4 gate:** build an 8-track beat with a bassline, play it under a synced track, switch patterns on the bar, render it to a sampler slot.

## Part F — Code Beat (Phase 5)

- [ ] **F1** CODE panel (Ctrl+Tab): EXAMPLES > open a starter; Ctrl+Enter evaluates. *Expect:* plays in time with the master; the code being heard lights up.
- [ ] **F2** Change a number, Ctrl+Enter. *Expect:* the change comes in on the next cycle without a gap. Ctrl+Shift+Enter: at once.
- [ ] **F3** Type a mistake (delete a bracket), Ctrl+Enter. *Expect:* the music keeps playing the last good pattern; the error is marked in the line.
- [ ] **F4** Ctrl+Space in a name and inside `s("...")`. *Expect:* completions (names; sound names in strings). Hover a name: help with an example.
- [ ] **F5** Ctrl+. (hush). *Expect:* silence, the editor stays.
- [ ] **F6** Tabs: make a second buffer, close the app, start it. *Expect:* both buffers back (saved in `Documents\Devil-DJ\Code`).
- [ ] **F7** RENDER: to a WAV (4 / 8 cycles) and to a sampler loop. *Expect:* the file / loop sounds like what played.
- [ ] **F8** The code deck's channel: fader, EQ, crossfader, FX target CodeDeck.
- [ ] **F9** **Beta gate (your part):** live-code a 4-layer pattern locked to deck A's grid and key; crossfade from the track to the code deck and back; introduce a syntax error mid-set: the deck keeps playing, the error shown inline. (The beta gate's "three outside testers" is not part of this test.)

## Part G — Stems and smart features (Phase 6)

- [ ] **G1** Stem separation: in the library, right-click 3 tracks > **Separate stems** > Fast. *Expect:* "Stems: <title> n %" in the activity line; each takes about half the track's length; Show stems in Explorer opens the FLAC folder (drums, bass, other, vocals). Try one with Quality (about 3× slower).
- [ ] **G2** Cancel: start one more, right-click > Separate stems > Cancel stem jobs. *Expect:* it stops; nothing half-written is left.
- [ ] **G3** Stem playback: load a separated track. *Expect:* the DRUMS / BASS / OTHER / VOCALS buttons on the deck light up (dimmed for tracks without stems).
- [ ] **G4** Click VOCALS (mute), Shift+click DRUMS (solo), drag BASS up / down (level), Ctrl+click (back to 100 %), right-click OTHER (FX send off → "DRY"). Keys: Alt+Q/W/E/R mute, Alt+Shift solo, Alt+A/S/D/F FX send, Alt+Z reset (deck B: U/I/O/P, J/K/L/;, M). *Expect:* clean, click-free changes; with FX on, a stem with FX send off stays dry.
- [ ] **G5** With stems muted, use a loop, scratch, key lock at +6 %, slip. *Expect:* the stems stay together and in time.
- [ ] **G6** WAVE button: the stems drawn as four lanes in the waveform; a muted stem dim.
- [ ] **G7** Drum Swap: pad mode menu > DRUM SWAP. Shift+pad 1: captures 8 beats of drums from the bar. Load another separated track, press pad 1. *Expect:* "NEXT" on the pad, then on the next bar the captured drums replace the track's drums, in time, no click; pad 1 again: the original drums back on the next bar.
- [ ] **G8** Drum Swap from the Beat Maker: right-click pad 2 > A Beat Maker pattern > A. *Expect:* your beat replaces the drums on the next bar. Right-click > Multi, press pads 1 and 2: both layered. Restart the app: the slots are still there.
- [ ] **G9** Phrases: after analysis finishes, look at the big waveform. *Expect:* blue lines with bar counts (8 / 16 / 32) at section changes, an energy strip along the bottom. Note how many of 5 tracks look right.
- [ ] **G10** Correct phrases on **10 tracks** (this is the ground truth for the phrase evaluation): right-click the waveform at a bar > A phrase starts at this bar / Move the nearest phrase line here / Remove the nearest phrase line. *Expect:* lines snap to bars; the edits survive a restart and a re-analysis.
- [ ] **G11** Smart cues: load a track with no hot cues (Hot Cue pad mode). *Expect:* empty pads show "? DROP", "? BREAK"…, and dashed flags on the waveform. Press one pad (it becomes a real cue), Shift+pad another (rejected, gone). Right-click a pad > take all / reject all. *Expect:* the F12 overlay line "Smart cues: n of m accepted".
- [ ] **G12** Mix points: Alt+X / Alt+C on deck A sets MIX IN / MIX OUT at the nearest bar (also right-click the waveform). *Expect:* green IN and red OUT flags; Alt+Shift+X / C clears.
- [ ] **G13** Automix with mix points: set a MIX OUT on track 1 a minute before its end, and a MIX IN 16 bars in on track 2; queue both; Automix. *Expect:* the transition starts at track 1's MIX OUT bar, track 2 comes in from its MIX IN.
- [ ] **G14** Vocal presence: a separated track with vocals. *Expect:* magenta "VOX" strips at the top of the phrases with singing; an instrumental shows none.
- [ ] **G15** Next Track and A's outro: play a track and watch the suggestions; let it play into its last phrase (or its MIX OUT region). *Expect:* the list refreshes when A enters its outro (often different tracks at the top).
- [ ] **G16** Thumbs: after a transition, ' (good) or Shift+' (bad), or the ✓ / ✗ buttons in the Next Track panel. *Expect:* a status line "Thumbs up on the last transition".
- [ ] **G17** Choice log: Options > Track Matcher log > Export as CSV. Then in a terminal, in the repo folder:
  `python tools/check_choice_export.py "<folder with the export renamed to choices.csv and choices.json>"`
  *Expect:* "ok", and a count of your logged choices.
- [ ] **G18** Options > Track Matcher log > **What you care about...**. *Expect:* bars per feature, "n choices logged"; TRAIN NOW works once you have choices; RESET goes back to the preset.
- [ ] **G19** **Phase 6 gate, part 1:** mix two tracks using stems — drums from A, vocals from B (mute A's vocals and B's drums, blend).
- [ ] **G20** **Phase 6 gate, part 2:** Drum Swap a Beat Maker pattern into a track (G8 counts).
- [ ] **G21** **Phase 6 gate, part 3:** Automix uses phrase-aware mix points (G13 counts).

## Part H — Logged sets and the Track Matcher evaluation (Phase 6 gate, P6-13)

These need real DJing. Make sure Options > Track Matcher log > **Log my choices** is ticked.

- [ ] **H1** **Set 1** (45 minutes or more): DJ normally with two decks. For most transitions, pick from the Next Track panel (N, or click), sometimes pick something else from the browser — both are useful. Play each mixed-in track at least a minute. After good / bad transitions press ' / Shift+'. Close the app at the end (the personal ranker trains then).
- [ ] **H2** **Set 2** on another day, the same way. Close the app at the end.
- [ ] **H3** What you care about (Options > Track Matcher log): *Expect:* "Learnt from n choices", bars different from the preset outline.
- [ ] **H4** Run the evaluation, in the repo folder (close the app first):
  `.\build\windows-x64\tools\matcher_eval\Release\matcher_eval.exe`
  *Expect:* a summary in the terminal and a report `docs\reports\matcher-<date>.md` with HitRate and MRR per preset, the personal ranker's row, the ablations and a tuned-weights suggestion.
- [ ] **H4b** Phrase detection on your corrections (G10), in the repo folder with the app closed:
  `.uild\windows-x64	ests\DevilDJTests_artefacts\Release\DevilDJTests.exe "[.phraseeval]"`
  *Expect:* one line per corrected track and a total "F" (1.0 = the detector agreed with every line you set). Write down the total.
- [ ] **H5** **Phase 6 gate, part 4:** in the report, is the Personal row's MRR higher than Balanced's? Write down both numbers. (If not, the report's notes may say why — e.g. too few choices.)
- [ ] **H6** Retune: run `matcher_eval.exe --write-presets`, start the app, pick **Tuned** in the Next Track panel's preset box. Use it in your next set and tell me if the suggestions feel better or worse.
- [ ] **H7** Send me the report file (`docs\reports\matcher-<date>.md`) — it holds no audio and no file names, only numbers and weights.

## Part I — Persistence, reliability and the MVP gate

- [ ] **I1** Close and start the app. *Expect:* library, playlists, cues, grids, phrases, mix points, settings, layout, keymap, code buffers, Drum Swap slots all as you left them.
- [ ] **I2** Debug menu > Soak test > Run for 60 minutes, with tracks on A and B (leave the laptop plugged in). *Expect:* the report at the end: 0 xruns.
- [ ] **I3** During one of the sets: F12 now and then. *Expect:* xruns 0 (write down any number above 0 and what you were doing).
- [ ] **I4** **MVP gate checklist** (from PLAN.md), during set 1 or 2:
  - [ ] 1-hour set using only keyboard + mouse, headphone cueing via split mode or two devices.
  - [ ] No xruns in the debug overlay; the recording of the set has no gaps (record it with F9).
  - [ ] Hot cues, loops, beat jump, two FX, a sampler loop and an auto-transition used during the set.
  - [ ] At least 5 tracks picked straight from the Next Track panel (N key), and the reasons shown made sense.
  - [ ] App restart restores library, cues and settings.

## Logs

The app's log files and crash dumps are in `%LOCALAPPDATA%\Devil-DJ\logs` (paste that into Explorer's address bar); Debug > **Open log folder** opens it too (the Debug menu shows in Debug builds, or start the app with `--debug-menu`). The library is `%APPDATA%\Devil-DJ\library.db`. My tools can't see those folders on your laptop, so paste the relevant log lines (or attach the file) when you report a fail.

---

## Results form

Copy this, fill it in, and send me the F lines.

```
Date(s): ____________   App build (git log -1 --oneline): ____________
Audio device / buffer: ____________

Part A: A1 _ A2 _ A3 _ A4 _ A5 _ A6 _
Part B: B1 _ B2 _ B3 _ B4 _ B5 _ B6 _ B7 _ B8 _ B9 _ B10 _ B11 _ B12 _
Part C: C1 _ C2 _ C3 _ C4 _ C5 _ C6 _ C7 _ C8 _ C9 _ C10 _ C11 _ C12 _ C13 _ C14 _ C15 _
Part D: D1 _ D2 _ D3 _ D4 _ D5 _ D6 _ D7 _ D8 _ D9 _ D10 _ D11 _ D12 _ D13 _ D14 _ D15 _ D16 _ D17 _ D18 _ D19 _ D20 _
Part E: E1 _ E2 _ E3 _ E4 _ E5 _ E6 _ E7 _ E8 _ E9 _ E10 _ E11 _ E12 _ E13 (Phase 4 gate) _
Part F: F1 _ F2 _ F3 _ F4 _ F5 _ F6 _ F7 _ F8 _ F9 (Beta gate, owner's part) _
Part G: G1 _ G2 _ G3 _ G4 _ G5 _ G6 _ G7 _ G8 _ G9 (__ of 5 right) G10 _ G11 _ G12 _ G13 _ G14 _ G15 _ G16 _ G17 _ G18 _ G19 _ G20 _ G21 _
Part H: H1 _ H2 _ H3 _ H4 _ H4b (phrase F ____) H5 (Personal MRR ____ vs Balanced ____) H6 _ H7 _
Part I: I1 _ I2 (xruns __) I3 (xruns __) I4 MVP gate: _ _ _ _ _

Fails (step: what happened instead):
-
```
