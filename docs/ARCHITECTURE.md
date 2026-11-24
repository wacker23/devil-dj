# Devil-DJ — Architecture

This document is the contract between modules. Changing a public interface or the threading model here requires the owner's approval (see `CLAUDE.md`).

## 1. Threads

| Thread | Owner | Does | Must never |
| --- | --- | --- | --- |
| Audio (device callback) | `engine::AudioEngine` | Decks, voices, mixer, FX, master clock, recording tap | Allocate, lock, block, do I/O, call QuickJS |
| Headphone device callback (P2-25, only with the separate-headphones layout) | `engine::CueDevice` | Read the cue from `AudioEngine::separateCueOutput()` (a `dsp::DriftBuffer`), resampled to its own clock | The same as the audio thread; touch anything but that buffer |
| UI (JUCE message thread) | `app`, `ui` | Components, input, painting at VBlank | Do analysis, DB writes, decoding |
| Track loader pool (2) | `engine::TrackLoader` | Decode files into `TrackBuffer` | Touch UI components |
| Analysis pool (cores − 1, low priority) | `analysis::AnalysisService` | Waveform, BPM/grid, key, loudness, phrase, stems | Run at normal priority while a deck plays |
| DB writer (1) | `library::Database` | All SQLite writes (serialized) | Block the UI waiting on a write |
| Code deck (1) | `codebeat::CodeDeck` | QuickJS eval + pattern queries every 25 ms | Run longer than its time budget |
| Matcher worker (1, below normal priority) | `matcher::MatcherService` | Candidate query, scoring, ranker training at exit, Smart Sort, kNN graph | Write to the DB except through the DB writer |
| Pattern scheduler (inside Code deck + Beat Maker timers) | `engine::Scheduler` producers | Push `ScheduledEvent`s ahead of time | Push events in the past |
| Recorder writer (1) | `engine::Recorder` | Drain record FIFO to disk | — |
| Housekeeping timer (UI thread, 10 Hz) | `engine::AudioEngine` | Free objects from the garbage queue, read xrun counters | — |

Communication:

```
UI / workers ──EngineCommand──▶ SpscQueue ──▶ Audio thread
Audio thread ──EngineState───▶ TripleBuffer ──▶ UI (read at VBlank)
Producers ──ScheduledEvent──▶ SpscQueue (one per producer) ──▶ Audio thread
Audio thread ──old pointers──▶ GarbageQueue ──▶ housekeeping frees them
Audio thread ──samples───────▶ Recorder FIFO ──▶ writer thread
```

Each producer thread gets its **own** SPSC queue into the engine (UI commands, loader, code deck, Beat Maker), so every queue stays single-producer/single-consumer.

## 2. Module dependency rules

```
app ──▶ ui ──▶ engine, library, analysis, beatmaker, codebeat, matcher
matcher ──▶ library (queries, choice log), util
codebeat ──▶ engine (Scheduler, Voices), util
beatmaker ──▶ engine (Scheduler, Voices), dsp, util
analysis ──▶ dsp, library (results), util
engine ──▶ dsp, util
library ──▶ util
dsp ──▶ util
```

`engine` and `dsp` never include JUCE GUI modules. `dsp` has no knowledge of decks or tracks — pure signal processing, unit-testable in isolation.

## 3. Engine

### 3.1 Callback flow (per block)

1. `ScopedRealtimeContext guard;` (debug allocation check)
2. Drain command queues (bounded: at most 256 commands per block).
3. Drain scheduled-event queues into the sorted `EventBuffer` (fixed capacity, insertion by timestamp).
4. `MasterClock::beginBlock(numSamples)` → block start/end beat positions.
5. Execute quantized actions whose target sample falls inside this block (split the block at those sample offsets).
6. Render each source into its channel buffer: decks (with stretcher), sampler slots, Beat Maker bus, code deck bus (voices triggered from `EventBuffer` at exact sample offsets).
7. Channel strips (trim, EQ/isolator, filter, color FX, fader) → FX units on their targets → crossfader → master + cue buses.
8. Limiter, meters, output routing (stereo / 4-channel / split mode), recorder tap.
9. Publish `EngineState` to the triple buffer, update CPU load and xrun counters.

All buffers are pre-allocated in `audioDeviceAboutToStart` for the maximum block size.

### 3.2 Commands

```cpp
namespace devildj::engine {

using DeckId = std::uint8_t;   // 0..3

struct LoadTrack    { DeckId deck; const TrackBuffer* buffer; TrackMeta meta; }; // ownership via registry, see 3.6
struct Play         { DeckId deck; bool quantized; };
struct Pause        { DeckId deck; };
struct CuePress     { DeckId deck; };
struct CueRelease   { DeckId deck; };
struct Seek         { DeckId deck; double samplePos; };
struct SetParam     { ParamId param; float value; };                  // every knob/fader: smoothed on the audio thread
struct HotCue       { DeckId deck; std::uint8_t slot; HotCueOp op; }; // trigger / release / set / delete
struct Loop         { DeckId deck; LoopOp op; Fraction beats; };
struct BeatJump     { DeckId deck; Fraction beats; };
struct SetSync      { DeckId deck; bool on; };
struct SetMaster    { std::optional<DeckId> deck; double internalBpm; };
struct FxCommand    { std::uint8_t unit; FxOp op; std::uint8_t slot, index; float value; }; // P3-08: per slot
struct FxSetEffect  { std::uint8_t unit, slot; dsp::fx::Effect* effect; };       // handed over by pointer
struct SamplerCmd   { std::uint8_t bank, slot; SamplerOp op; const SampleBuffer* buffer; };
struct Transition   { DeckId from, to; TransitionPlan plan; };
struct Record       { RecordOp op; };

using EngineCommand = std::variant<LoadTrack, Play, Pause, CuePress, CueRelease, Seek, SetParam,
                                   HotCue, Loop, BeatJump, SetSync, SetMaster, FxCommand,
                                   SamplerCmd, Transition, Record /* ... */>;

static_assert(std::is_trivially_copyable_v<EngineCommand>);
}
```

`ParamId` is a flat enum of every continuous parameter (`Deck0Tempo`, `Ch0EqLow`, `Crossfader`, `Fx1Slot0Wet`, …) so MIDI Learn and the keymap address parameters uniformly.

### 3.3 Engine state (published each block)

```cpp
struct DeckState {
    bool loaded, playing, keyLock, sync, isMaster, slip, looping, reverse;
    double positionSamples;        // what the listener hears (latency-compensated)
    double slipPositionSamples;
    double rate;                   // effective playback rate
    double bpm;                    // effective BPM after tempo
    float tempoPercent;
    std::int8_t transposeSemis;
    double loopIn, loopOut;
    float peakL, peakR, rms;
    std::uint64_t trackId;
};

struct EngineState {
    std::uint64_t blockCounter;
    double hostTimeSeconds;        // juce::Time::getMillisecondCounterHiRes()/1000 at publish
    double sampleRate;
    std::array<DeckState, 4> decks;
    double masterBpm, masterBeat;
    std::array<ChannelMeter, 8> channels;  // 4 decks, sampler, beatmaker, codedeck, mic
    float masterPeakL, masterPeakR, limiterGainReductionDb;
    float cpuLoad;                 // 0..1 of block budget, smoothed
    std::uint32_t xruns, lateEvents;
    bool recording; double recordSeconds;
};
```

The UI extrapolates playheads: `pos + rate * sampleRate * (now - hostTimeSeconds)`.

### 3.4 Deck

```
TrackBuffer ──▶ PlayHead (double pos, rate) ──▶ [key lock?] Stretcher : Resampler ──▶ deck out
                    ▲                                    │
            loops, cues, slip, jog                 latency compensation
```

- Position is a `double` in source samples. Rate = tempo × pitch bend × jog override × sync correction.
- Without key lock: `dsp::Resampler` (cubic Hermite default; 16-tap windowed sinc in "high quality" setting).
- With key lock: `dsp::Stretcher` (Signalsmith Stretch). The stretcher consumes `rate × blockSize` input samples per block; its input/output latency is subtracted from the reported position so waveform and sound match. Scratching and reverse bypass the stretcher.
- Every discontinuity (jump, loop wrap, hot cue) uses a 2 ms equal-power crossfade between old and new read positions (two read heads for the fade duration).
- Slip: a second "shadow" playhead always advances at the normal rate; when slip ends, the main playhead jumps (with fade) to the shadow.
  As built (P3-04): `Deck` keeps a set of slip reasons (loop, held hot cue, scratch, pause from playing). The first one to start (slip on, deck playing) puts the shadow at the playhead; while any is active the shadow advances by the normal rate (tempo x bend x sync correction) each sample, also while paused; when the last one ends the playhead jumps there through the 2 ms crossfade (paused: play starts there). A seek, back cue or slip off drops the shadow (you stay where you went). The engine marks a hot cue pressed while playing with slip on as held (`slipHoldPad`) and returns on its release, calling off its quantized jump if it hasn't happened yet. Reverse (P3-05) adds its own reason.
- Reverse and stops as built (P3-05): `Deck::setReverse` negates the rate (key lock bypassed, crossfaded in and out like a scratch); loops wrap from start back to end; the start of the track stops the deck. `brake(s)` / `spinBack(s)` are stop ramps (brake: rate x (1 - t/T); spin-back: to -3x in 60 ms, then x (1 - u)^2 to 0) that end paused; play cancels them; the ramp is a slip pause from its first sample. The engine's `Stop{kind}` and `SetStopStyle` (how TogglePlay stops, the times); quantized jumps run at once while reversed or stopping. A deck leads the master clock with its slip shadow while slipping, and not at all while reversed or braking without slip (the clock runs on at its tempo); sync's phase follow pauses for a deck that is reversed, stopping or slipping.
- Slicer as built (P3-07): the engine keeps each deck's slicer domain (8 / 16 / 32 grid beats from the bar at or before the track's timeline - the slip shadow while slipping) and shifts it on by its length as the timeline passes its end (placed anew after a jump). A held slicer pad is a slip hold that works with slip off (`Deck::beginSlicerHold`); its slice loops from the next slice boundary (a queued jump with the loop, so the roll is in phase with the timeline); the last pad up exits the loop and returns to the shadow.
- Quantized actions are stored as `PendingAction{targetSample, action}` (fixed array of 16 per deck) and executed when the block reaches the target.

- Stems as built (P6-04): a deck can have its track's `StemBuffer` (drums, bass, other, vocals; 16-bit stereo at the device rate, the track's length, from the stem cache by `engine::loadStems` on a loader thread). The deck mixes them where it reads: every read head (playhead, jump tails, the key-lock feed) reads the four stems through the resampler with a gain each, so they stay in sample-exact step through loops, scratch, slip, reverse and key lock. Each stem has a level, mute, solo (any solo: only soloed stems) and an FX send; gains ramp over 20 ms. While every stem is at its default the deck reads the track itself (bit-exact; the stems add back to it within 16-bit rounding, so switching is seamless). Stems with the FX send off go to a second (dry) output; with key lock a second stretcher, primed with the same history, stretches it. `SetStem{deck, stem, Level | Mute | Solo | Fx | Reset, value}`; a new track resets them. `DeckState` gains stemsLoaded, stemsId, stemLevel[4], stemMute / stemSolo / stemFx (bit = stem), stemsActive (reading the stems), stemsDry.

### 3.5 Master clock, sync and quantize

- `MasterClock` holds `bpm` and `beat` (double). Source: the master deck (`beat` = that deck's grid beat at its playhead) or internal tempo when nothing is master.
- As built (P2-18, `engine/MasterClock`): `beginBlock(numSamples, decks)` runs at the start of each render chunk, before the decks render, with each deck's playing / grid / synced flags, grid beat at its playhead and BPM as heard (grid BPM x rate). Modes: Auto (the leading deck while it plays, else the first playing synced deck with a grid), Deck n, Internal (commands `SetClockSource`, `SetInternalTempo`). A deck that takes over keeps the clock's bar count: its beat plus the whole bars (multiple of 4) nearest to where the clock was. With no deck leading the clock runs on from its last beat at the internal tempo, which takes over the last leading deck's tempo, so the beat never jumps. The engine counts an absolute sample clock (`blockStart()` / `blockEnd()`, `EngineState::sampleClock`); `beatAt(sample)` / `sampleAt(beat)` are linear in the current block. `EngineState` carries masterBpm, masterBeat (end of the last block), masterSource (-1 = internal), clockMode, internalBpm. Until P2-19 every deck counts as synced.
- `sampleAt(beat)` / `beatAt(sample)` convert within the current block using the current BPM; producers schedule at most 150 ms ahead, so tempo changes cause negligible error.
- **Sync**: follower rate = `masterBpm / trackBpm` (×2 or ÷2 if closer to 1). Phase error `e = wrap(masterBeatFrac − deckBeatFrac)` in [−0.5, 0.5). Correction: `rate *= 1 + clamp(k·e, ±0.005)` with k ≈ 0.02 per block; on sync-press while playing, phase is snapped in one jump (with fade) if |e| > 0.1 beat.
- As built (P2-19): `AudioEngine::followMaster` runs right after the clock advances, before the decks render. Each synced deck that doesn't lead: tempo = master BPM x q / grid BPM with q in {1/2, 1, 2} (the one nearest rate 1), set through `Deck::setSyncTempo` (the fader range widens if needed); phase error e = wrap(masterBeat x q - deckBeat); when sync starts (SYNC pressed, play on a synced deck) and |e| is over 2 ms the deck jumps into phase once (`seek`, with its fade); otherwise `Deck::setRateCorrection(1 + clamp(e seconds / 0.05 s, +-0.5 %))`. Sync lock: SetTempo / NudgeTempo / ResetTempo on a synced follower set the leader's tempo (or the internal tempo) so the follower gets the tempo asked for, and every synced deck moves with it. Auto prefers a playing synced deck as the leader, else any playing deck with a grid. `DeckState` gains synced, master, phaseErrorMs.
- **Quantize**: target = next grid point at the chosen resolution after "now + 1 block".
- As built (P2-20): `SetQuantize{on, beats}` (1/8, 1/4, 1/2, 1) is engine-wide; `Play` and `Seek` also take `quantized` for one command. Each deck has up to 16 queued actions, run in order: Play (at a master-clock beat: the clock's next grid point when the command arrives, if another deck leads; else immediately) and Jump (when the playhead crosses its own next grid point, to a grid point plus the overshoot, so the phase is kept to the sub-sample). `renderDeckWithActions` renders the deck up to the sample an action falls due, runs it, renders the rest. A cue set while paused, and a quantized seek while paused, snap to the nearest grid point at once. Pause calls a waiting play off; a new track clears the queue. Hot cues, loop in and beat jump (P2-21, P3) queue the same Jump.

- Hot cues as built (P3-01): the engine holds each deck's 16 slots (`SetHotCues`, buffer samples; length > 0 = a hot loop) and acts on `HotCue{deck, slot, Press | Release | Delete}`: an empty slot is set at the playhead (quantize: the nearest grid point); a set one, playing, jumps there (quantize: the P2-20 queue, at the next grid point, phase kept); paused, it plays from it while held (release: back and paused; Play takes over). `DeckState::hotCueStart / hotCueLength / hotCuePreview` report them; the app (`app/HotCueStore`) loads a track's cues from `cues` at the deck's rate after the track, and once the engine shows them writes every change it sees (a new cue gets its pad's colour) through the library's writer thread. Colours and labels live in the app and the library only. A queued jump whose grid point an earlier jump already passed runs at once, without overshoot.

- Loops as built (P3-03): `Deck` holds one loop (`setLoop` / `exitLoop`); while active, playing forward and not scratching, `render` splits the block at the sample the playhead reaches the end and wraps to start + the overshoot (modulo the length) through the deck's 2 ms jump crossfade - also with key lock, where the split re-primes the stretcher. A seek outside the loop ends it. The engine's `Loop{op}`: auto (toggle, the size in grid beats 1/64-512, from the grid point at or before the playhead with quantize on), in / out (in on the grid point before, out on the nearest), halve / double (no loop: the auto-loop size), reloop (the last loop from its start), exit, set (a stored loop, and a jump to its start). Beat jump while looping moves the loop and the playhead. A hot loop pressed loops (quantized: the queued jump carries the loop and starts it there); an empty pad (or the memory key) while looping stores the loop.

### 3.5b Separate headphone device (P2-25)

Output layout `SeparateCueDevice`: the main device gets the master on 1/2; after each render chunk the engine writes the cue bus into `dsp::DriftBuffer` (lock-free, one writer / one reader, 32 k frames, nothing allocated after construction). `engine::CueDevice` opens a second device of the same driver and its callback reads the buffer: 4-point cubic interpolation at the ratio writer rate / reader rate x (1 + c), where c = 0.01 x the relative fill error (smoothed over ~2 s), clamped to +-0.5 %, keeps the fill near its target (1.5 x both block sizes + 128 frames, ~20 ms at 48 kHz / 256): the two clocks may drift a few hundred ppm. The reader starts silent, plays once the target of fresh audio is there, and goes silent and primes again if it runs dry; the writer drops a block that doesn't fit. So the cue lags the master by the target plus the second device's own latency.

### 3.6 Ownership of big buffers

- `TrackBuffer` and `SampleBuffer` are created by loader threads and registered in `BufferRegistry` (UI-thread object holding `std::shared_ptr`s).
- The engine receives raw `const T*` plus a registry id. When the engine stops using a buffer it pushes the id to `GarbageQueue`; the housekeeping timer drops the registry's `shared_ptr`. The audio thread never decrements a reference count.
- `StemBuffer` (P6-04) is owned the same way (`BufferRegistry::addStems`, same ids, same release path): `LoadStems{deck, track, stems, id}` is taken only while `track` is on the deck (else the id comes straight back); the deck's stems go back with its track when a new one replaces it.

### 3.7 Scheduler and voices

```cpp
struct ScheduledEvent {
    std::int64_t sampleTime;       // absolute engine sample clock
    SourceId source;               // BeatMaker, CodeDeck, Sampler sequencer
    VoiceKind kind;                // Sample, DrumSynth, Synth
    std::uint16_t soundIndex;      // index into SoundBank (resolved off the audio thread)
    float note, velocity, gain, pan, speed, cutoffHz, hpfHz, resonance;
    float attack, decay, sustain, release, durationSeconds;
    float roomSend, delaySend, crush;
    std::uint8_t chokeGroup;
    std::uint32_t highlightToken;  // for Code Beat active highlighting
};
```

- `engine::Voices` has fixed pools (64 sample voices, 32 synth voices, 16 drum-synth voices per source). Allocation is by free list; stealing = oldest released, then oldest playing.
- Events start at their exact sample offset within the block. Events with `sampleTime` earlier than the block start are dropped and counted as `lateEvents`.

### 3.8 Mixer graph

```
Deck A ─▶ Strip ─┐
Deck B ─▶ Strip ─┤                 ┌─▶ Master bus ─▶ Limiter ─▶ meters ─▶ out 1/2 (or split L)
Deck C ─▶ Strip ─┼─▶ Crossfader ───┤
Deck D ─▶ Strip ─┤   (A/THRU/B)    └─▶ Record tap
Sampler ▶ Strip ─┤
BeatMkr ▶ Strip ─┤
CodeDk ─▶ Strip ─┤
Mic ───▶ Strip ─┘ (thru)
Cue bus = Σ (pre-fader channels with CUE on) mixed with master (after master gain) by cue/master knob ─▶ headphone level ─▶ Limiter ─▶ out 3/4 (or split R)
```

Strip = trim → EQ/Isolator → Color FX (filter etc.) → [FX unit insert if targeted] → fader. FX units targeting "master" process the master bus before the limiter.

Stems past the effects (P6-04): a deck's dry stems (FX send off) get their own `ChannelStrip` with the channel's trim / EQ / filter settings (copied each block while it runs, reset when it starts) and join the channel after its FX units (color FX, pad FX, release FX, the units on the deck), before the meter, cue send and fader.

Color FX as built (P3-10): `dsp::fx::ColorFx` per channel, right after the strip's filter (before the FX units); the engine routes each channel's FILTER parameter to the strip's filter (character Filter) or to its color FX (the strip's filter then goes to its centre over 20 ms); `SetColorFx{type}` chooses for all channels; the state's channel `filter` is the knob either way.

Release FX as built (P3-11): `dsp::fx::ReleaseFx` per channel after the color FX: held = its character on the music, let go = the dry cut and the effect ending it over the beat length; once it has ended the engine pauses the deck and resets it when the deck is silent (or when play starts). It keeps the last 16 s of the channel (also while idle) for brake / spin-back to play from; idle = untouched.

Sampler as built (P3-13): `engine::Sampler` - 32 slots (4 banks x 8), 8 voices each with its own stretcher; one-shot / loop / gate; sync stretches a sample to the master tempo from its BPM (the library's, or a power-of-two number of beats over its length), primed so what is heard starts on its first frame; a quantized press waits for the master clock's next grid point (the engine turns the beat into a sample offset after the clock has advanced). The sampler is mixer channel `kSamplerChannel` (= 4, after the decks): the same `mixChannel` path as a deck (strip, color FX, pad FX, release FX, the FX units targeting Sampler, meter, cue, fader, crossfader). Sample buffers come from the `BufferRegistry` and are handed back through the release queue when replaced.

Sequencer as built (P3-15): `engine::Sequencer` holds one sequence (1 / 2 / 4 bars, up to 128 events of beat / slot / press-or-release, fixed storage) pinned to the master clock's bars: an event at beat b fires wherever the clock is at b mod the length, at its exact sample in the block (`process` before the sampler renders). While recording, the engine's sampler-pad commands are recorded at the beat they sound (the quantized one when quantize is on), on the 1/16 grid; one recorded in the current pass isn't fired again in it. The app (`app/SequenceStore`) keeps 8 per sampler bank as JSON in the settings, loads one with `SequencerCommand Clear` + `SequencerEvents` chunks and takes back what was recorded from the engine state.

Auto-transition as built (P3-17): `StartTransition` plans it on the audio thread: the incoming deck gets sync on, its low EQ killed and a quantized play queued for the start beat (the next 16-beat boundary of the master clock unless given); once a block, after the clock advances, `runTransition` sets the lanes from the progress p = (beat - start) / length: the crossfader from where it was to the incoming deck's side (or, without opposite A / B sides, the outgoing fader down), the low EQ swap at p = 0.5, the style's high-pass sweep on the outgoing deck or its release-FX echo out from p = 0.75; at p = 1 the outgoing deck stops (the echo out stops it itself) and its EQ / filter / fader come back. `CancelTransition` stops it where it is.

Pad FX as built (P3-12): each channel has 8 more `FxSlot`s (one per pad of the current bank) after the color FX, before the release FX; the app (`app/PadFxPresets`) installs the bank's effects behind both decks' pads (`installPadEffect` / `PadFxSetEffect`, owned and handed back like the units' effects) and a press is `PadFxCommand` On / Off - or `ReleaseFxCommand` for a release preset. The presets are JSON in the settings.

FX as built (P3-08): `dsp::fx::Effect` (prepare / reset / process the wet signal in place with a `BeatContext` from the master clock / `hasTail` / beats / normalised parameters); `dsp::fx::FxSlot` wraps one: out = (1 - wet x m) in + wet x g x fx(m x in), the send m ramped over 20 ms, g = 1 with a tail (it rings out after the send closes, until 50 ms below -100 dBFS or 30 s) and g = m without; an Off slot doesn't touch the signal (bit-exact). `dsp::fx::FxUnit`: 3 slots in series (multi) or slot 0 alone (single). The engine has 2 units with a target each (deck 1-4 after the strip's filter and before the fader - the meter and the headphone cue hear it - the sampler and code deck once they exist, the master before master gain); changing the target resets the unit. Effects are created and prepared on the message thread (`AudioEngine::installEffect`, which owns them), handed over by pointer, and the replaced one comes back through `releasedEffects` for `collectEffects()` to delete; `prepare` prepares them again for a new device.

### 3.9 Recorder

Record tap copies the master (post-limiter) into a lock-free FIFO sized for 2 s; a writer thread writes WAV 24-bit or FLAC with `juce::AudioFormatWriter::ThreadedWriter`. Overflow is counted and reported (never blocks the audio thread).

As built (P3-16): `engine::RecordTap` (a lock-free SPSC ring of stereo frames, 2 s at 192 kHz, allocated once; dropped frames counted) is filled after the limiters with the master or the headphones; `engine::Recorder` is its own writer thread (not ThreadedWriter: it reads the tap directly) writing 24-bit WAV or FLAC, with auto start at the first sample above -60 dBFS and auto split after 20 s below it (at the exact sample). The app writes a `.txt` tracklist and a `.cue` sheet next to each finished file from the session's history (`app/RecordingSheets`).

## 4. DSP design notes

| Block | Design |
| --- | --- |
| Parameter smoothing | One-pole or linear ramps over 20 ms; all user-facing gains in dB internally |
| EQ mode | RBJ biquads: low shelf 70 Hz (Q 0.7), peak 1 kHz (Q 0.7), high shelf 13 kHz (Q 0.7); range −26…+6 dB |
| Isolator mode | Linkwitz-Riley 8th order (Butterworth 4th order squared, 48 dB/oct) at 250 Hz and 2.5 kHz with the matching 4th-order all-pass on the low band, so bands sum flat; per-band gain −∞…+6 dB. (Was LR4; LR4 only takes ~40 dB off a killed band at its centre, P1-10 requires > 60 dB.) |
| Channel filter | TPT (topology-preserving) state-variable filter; knob < 0.5 → LP 20 kHz→60 Hz (exponential), > 0.5 → HP 20 Hz→10 kHz; resonance setting; dead zone ±2 % around centre |
| Crossfader curves | Smooth: `gA = cos(x·π/2)`, `gB = sin(x·π/2)`; Linear; Cut: full level until 5 % from the far end |
| Limiter | 1.5 ms look-ahead, peak detector with 5 ms release, ceiling −0.3 dBFS, true-peak 4× oversampled detector in "high quality". Gain = per-sample requirement (ceiling / peak), min-held over look-ahead + 1 samples, released, then averaged over the look-ahead, so the output provably stays under the ceiling; latency = look-ahead (72 samples at 48 kHz), published as `masterLatencySamples` |
| Resampler | Cubic Hermite (default), 16-tap Kaiser-windowed sinc (high quality) |
| Stretcher | `dsp::Stretcher` interface; `SignalsmithStretcher` with `presetDefault(channels, sampleRate)`; transposition in semitones; latency exposed via `getLatencySamples()` |
| Reverb | 8-line feedback delay network, Householder matrix, damping LP per line, pre-delay; tempo-free |
| Delays | Fractional delay lines (Hermite interpolation), tempo-synced times from MasterClock, LP/HP in feedback, ping-pong option |
| Roll / Freeze | Capture N beats into a ring, loop it quantized while held, fade 3 ms at boundaries |
| Loudness | ITU-R BS.1770-4 K-weighting + gating (offline, analysis only) |

## 5. Analysis pipeline

```
AnalysisJob(trackId) ──▶ decode (mono 22.05 kHz + full-rate stereo for waveform)
   ├─▶ WaveformBuilder (150 pts/s, 3 bands, peaks+RMS) ──▶ cache file
   ├─▶ BeatTracker v2 (onset flux → tempogram → DP beats → constant fit, steady / live → downbeat) ──▶ beatgrid
   ├─▶ KeyDetector (chroma → 24 profiles) ──▶ key, confidence
   ├─▶ Loudness (BS.1770) ──▶ LUFS, auto-gain dB
   ├─▶ [Phase 6] Phrase, vocal presence, stems (ONNX)
   └─▶ results ──▶ DB writer (transaction) ──▶ change notification ──▶ UI
```

Each analyzer is a pure function `Result analyze(const AnalysisInput&, const Params&)` with no global state, so it is unit-testable with generated signals. Results carry `analysisVersion`; bump it when an algorithm changes.

`analysis::AnalysisService` (P2-12): a queue of track ids and cores - 1 workers (at most 4) at the lowest thread priority. The app hands it a decoder function (engine::decodeTrack) so the analysis module doesn't depend on the engine. Per track: decode at the file's rate, the mono 22.05 kHz signal (sinc resampler), the analyzers (`void(const AnalysisInput&, AnalysisResult&)`), then results are written in batches (up to 25 tracks or ~1 s per transaction) with `analysis_version = kAnalysisVersion`; a failed track gets `-kAnalysisVersion` and isn't retried until the version changes. BPM and beatgrid are not overwritten when the user locked them. Imports enqueue new tracks; at start every present track with an older version is queued; the browser refreshes at most ~2.5 times a second while results stream in.

Beat tracker v2 (after the owner's eval set, analysis version 4): the tempogram peak is chosen with half the beat lag counted too (against 2/3-tempo errors on eighth-note hi-hats), the octave rules unchanged. The constant grid's outlier gate tightens pass by pass (15 % of a period down to 20 ms). The beats are split into straight pieces (15 ms): when at least 40 % sit in pieces at one exact tempo the track is steady and the constant grid is refitted on the beats in phase with them (the other pieces are slips of the beat chain). Otherwise, when the constant grid keeps under half the beats within 25 ms, the chain is followed again with a looser DP (tightness 100 instead of 400) and a tempo-following grid (up to 16 anchors, as few as keep the beats within ~15 ms) is used if it keeps at least 12 % more of them on the grid; its BPM is the average (beats over time). `beatgridJson` writes one anchor per tempo change.

`analysis::detectKey` (P2-15, `analysis/KeyDetector`): 8192-point FFT every 2048 samples on the mono 22.05 kHz signal (Hann), spectral peaks between C2 and C7 with parabolic frequency refinement; the tuning is the magnitude-weighted circular mean of the peaks' deviation from A = 440 and is taken out before the peaks go into 36 bins per octave and fold to 12 pitch classes (centre third of a semitone 1, outer thirds 0.5). Each frame's chroma is normalized and weighted by the frame's RMS (frames under -60 dBFS skipped), the sum is correlated (Pearson) with the Krumhansl-Kessler major / minor profiles at all 12 roots. Output: key 0..23, confidence (correlation x lead over the runner-up / 0.15, both clamped to 0..1), tuning in cents. `keyAnalyzer` writes musical_key / key_confidence (analysis version 3); nothing is written when no key is found, so a tag's key stays. Display: `util/MusicalKey.h` (musical or Camelot, setting `keyNotation`).

`analysis::integratedLoudness` (P2-16, `analysis/Loudness`): BS.1770-4 on the full-rate stereo (a mono file is measured as it plays, duplicated on both channels). K-weighting = high shelf + high pass, derived for any rate by the bilinear transform from the analog design (equal to the standard's 48 kHz coefficients); mean squares per 100 ms step, 400 ms blocks every 100 ms, absolute gate -70 LUFS, relative gate -10 LU. `loudnessAnalyzer` writes tracks.loudness_lufs (analysis version 5); auto_gain_db stays empty: the app turns the loudness into a trim for the user's target when a deck loads (Auto Gain: trim = target - LUFS within the trim's +-12 dB; settings `autoGain`, `autoGainTarget`). A track loaded before its analysis gets the trim when the result arrives, if the deck is stopped and the trim wasn't touched.

Track Matcher features v1 (P2-24, `analysis/TrackFeatures`, `analysis/FeatureNormalizer`): one pass over the mono 22.05 kHz signal (1024-point FFT every 256 samples) gives the energy inputs per 16-bar window of the grid (30 s without one) - loudness dB, onset rate, low ratio < 150 Hz, centroid -, the 32-d sound vector (MFCC 1-13 mean / sd, centroid, roll-off, flatness, onset rate, low / high ratios) and the 48-d groove (accents on the 16 sixteenths of a bar in 3 bands, from the grid the beat analyzer just found). `featureAnalyzer` stores them raw in `track_features` (schema v2; analysis version 6). When the analysis queue empties after writing features, `normalizeLibraryFeatures` recomputes every track's 0..1 energy (window percentiles of the weighted z-score over all of the library's windows; whole = mean, intro = first, outro = last, curve as float16) and the library statistics in `feature_stats`.

## 6. Library

- SQLite in WAL mode at `%APPDATA%\Devil-DJ\library.db`. One writer thread; UI reads through a read-only connection.
- Repositories return plain structs; UI models (`TrackTableModel`) cache pages of 200 rows.
- Repositories (`TrackRepository`, `PlaylistRepository`, `CueRepository`, `TagRepository`) work on the connection they are given: a reader connection for reads, the writer connection inside a `Database::write` job for writes. Writes note what they changed (`Change{table, id, removed}`); after each job the writer thread hands the list to `Database` change listeners (rolled-back work is dropped). Listeners run on the writer thread and must post to their own thread (the UI posts to the message thread). Connections refuse to run on the audio thread (Debug, via RealtimeGuard).
- FTS5 virtual table `tracks_fts(title, artist, album, comment)` kept in sync by triggers.

## 7. Database schema (v1)

```sql
CREATE TABLE tracks (
  id INTEGER PRIMARY KEY,
  path TEXT NOT NULL UNIQUE,
  content_hash TEXT NOT NULL,
  title TEXT, artist TEXT, album TEXT, genre TEXT, year INTEGER, comment TEXT,
  duration_ms INTEGER, sample_rate INTEGER, bitrate INTEGER, file_size INTEGER,
  bpm REAL, bpm_locked INTEGER DEFAULT 0,
  musical_key INTEGER,            -- 0..23 (0 = C major ... 23 = B minor), NULL = unknown
  key_confidence REAL,
  loudness_lufs REAL, auto_gain_db REAL,
  rating INTEGER DEFAULT 0, color INTEGER,
  play_count INTEGER DEFAULT 0, last_played INTEGER,
  date_added INTEGER NOT NULL,
  analysis_version INTEGER DEFAULT 0,
  missing INTEGER DEFAULT 0
);
CREATE INDEX idx_tracks_bpm ON tracks(bpm);
CREATE INDEX idx_tracks_key ON tracks(musical_key);

CREATE TABLE beatgrids (
  track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
  anchors_json TEXT NOT NULL,     -- [{"beat":0,"sample":12345,"bpm":128.0,"bar_beat":1}, ...]
  locked INTEGER DEFAULT 0
);

CREATE TABLE cues (
  id INTEGER PRIMARY KEY,
  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  kind INTEGER NOT NULL,          -- 0 hot cue, 1 memory cue, 2 memory loop, 3 hot loop, 4 mix in, 5 mix out
  slot INTEGER,                   -- 0..15 for hot cues
  start_sample INTEGER NOT NULL, length_samples INTEGER DEFAULT 0,
  label TEXT, color INTEGER
);
CREATE INDEX idx_cues_track ON cues(track_id);

CREATE TABLE playlists (
  id INTEGER PRIMARY KEY,
  parent_id INTEGER REFERENCES playlists(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  kind INTEGER NOT NULL,          -- 0 folder, 1 playlist, 2 smart, 3 history
  rule_json TEXT,                 -- smart playlists
  sort_order INTEGER DEFAULT 0,
  created_at INTEGER NOT NULL
);

CREATE TABLE playlist_tracks (
  playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  position INTEGER NOT NULL,
  PRIMARY KEY (playlist_id, position)
);

CREATE TABLE tag_categories (id INTEGER PRIMARY KEY, name TEXT NOT NULL, sort_order INTEGER);
CREATE TABLE tags (id INTEGER PRIMARY KEY, category_id INTEGER REFERENCES tag_categories(id), name TEXT NOT NULL);
CREATE TABLE track_tags (track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,
                         tag_id INTEGER REFERENCES tags(id) ON DELETE CASCADE,
                         PRIMARY KEY (track_id, tag_id));

CREATE TABLE history (id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL,
                      track_id INTEGER REFERENCES tracks(id), played_at INTEGER NOT NULL, deck INTEGER,
                      previous_track_id INTEGER REFERENCES tracks(id));   -- transition source for the Track Matcher

CREATE TABLE analysis_artifacts (
  track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,
  kind TEXT NOT NULL,             -- 'waveform', 'stems', 'phrases', 'vocal'
  path TEXT NOT NULL, version INTEGER NOT NULL,
  PRIMARY KEY (track_id, kind)
);

CREATE VIRTUAL TABLE tracks_fts USING fts5(title, artist, album, comment, content='tracks', content_rowid='id');
```

History (P2-23, `library::HistoryRepository`): one session per app run, created with its first play: a History playlist named "YYYY-MM-DD HH:MM" (its `id` is `history.session_id`), its tracks in play order. Each play adds the track to the playlist and a `history` row whose `previous_track_id` is the session's last track (the transition). Queries: entries(session), sessions(), transitions(), transitionCount(A, B), playedAfter(A); tracklist / exportTracklist (UTF-8 text with a BOM). The app's `PlayTracker` counts a library track as played after 60 s of playing since its load (once per load), checked 4 times a second.

Migrations live in `library/Migrations.cpp` as an ordered list of SQL strings; `PRAGMA user_version` stores the applied version.

Schema v2 (P2-24, in `library/Migrations.cpp`; also `feature_stats` and an index on history transitions) adds `track_features`, `track_embeddings`, `choice_log`, `matcher_models` and `track_neighbors` — defined in `docs/MATCHER_SPEC.md` §11. History rows also record the previous track so transitions can be counted.

## 8. Files and folders

| What | Where |
| --- | --- |
| Settings | `%APPDATA%\Devil-DJ\settings.xml` |
| Library DB + backups | `%APPDATA%\Devil-DJ\library.db`, `...\backups\` |
| Analysis cache (waveforms, stems) | `%LOCALAPPDATA%\Devil-DJ\cache\<content_hash>.*` |
| Logs + crash dumps | `%LOCALAPPDATA%\Devil-DJ\logs\` |
| User content | `Documents\Devil-DJ\{Samples,Beats,Code,Recordings,Keymaps}` |

Waveform cache format `.wfm` (P2-17, `analysis/WaveformCache`): header (magic `DDJW`, version u16, sample rate u32, points/s u16, count u32, samples per point u32, overview points u16) + 6 × u8 arrays (low/mid/high × peak/RMS) + overview block (8 × u8: low/mid/high × peak/RMS, full-band peak/RMS). Levels are stored as sqrt(level) × 255. Written through a temporary file and a rename. The analysis service writes one per analyzed track at the file's rate; the track loader reads it as a load starts (`onWaveformPreview`: the decks draw it until the new buffer is on the deck), re-labels it for the deck's rate (same 150 points/s) and uses it for the loaded track; with no cache (or one that doesn't fit the decoded length) it builds the waveform and writes the cache.

## 9. UI architecture

- `MainComponent` hosts layout areas: top (deck A, mixer, deck B), centre (waveforms), bottom (browser / Beat Maker / Code Beat tabs), side panels (sampler, FX, next-track).
- Library browser (`ui::LibraryBrowser`): tree (Collection, Playlists, Smart playlists, History, Folders) + a `juce::TableListBox` fed by `ui::TrackTableModel`, which reads only the ordered id list on a sort / source change and the rows a page of 200 at a time when first painted (32 pages kept). It owns the UI's read-only connection and refreshes from the database change notifications (posted to the message thread). Imports run on a worker thread (`MainComponent::importToLibrary`). Search / filters (`ui::TrackFilter`) become one query: a join on `tracks_fts MATCH` (quoted prefix words) plus BPM / key / rating / colour / genre / tag conditions. Reader connections use memory-mapped reads, a 32 MB page cache and in-memory sorting. Smart playlists keep their rules as JSON in `playlists.rule_json` (`library::SmartRules`: field / operator / value, all or any); `library::toSql` turns them into a condition on `tracks t` each time the playlist is shown, so it is always current (track, tag and rule changes refresh it).
- All user actions go through `ui::ActionDispatcher::perform(CommandId, ActionPhase)`; components, `KeyboardController`, MIDI Learn and the phone remote all call it.
- Keys: `ui::PhysicalKeyHook` (a `WH_KEYBOARD` hook on the UI thread only) reads the hardware scan code of each key message before the IME or JUCE see it, and `ui::KeyboardController` maps scan code + modifiers to a `CommandId` per focus context (Main, Beat Maker, Code Beat; a focused text field takes all keys). JUCE also detaches the IME from windows whose focused component isn't a text input.
- Views never hold engine pointers; they read `EngineState` (latest) and send `EngineCommand`s.
- `juce::VBlankAttachment` drives waveform and meter repaints; other components repaint on change.
- Look and feel: `ui::DevilLookAndFeel` with design tokens (colours, radii, type scale) in one header so themes can switch.

## 10. Code Beat runtime

See `docs/CODEBEAT_SPEC.md`. Summary: the pattern library is JS (`js/codebeat/`) embedded as binary data, evaluated in quickjs-ng on the Code deck thread; C++ queries `pattern.queryArc(t0, t1)` and converts events to `ScheduledEvent`s.

## 11. Performance budgets (target laptop, 48 kHz, 256-sample buffer = 5.33 ms)

| Item | Budget |
| --- | --- |
| Audio callback, 2 decks key lock + 2 FX + sampler | ≤ 35 % of buffer time (p99) |
| Audio callback, 4 decks + stems on 2 | ≤ 60 % (p99) |
| UI frame (2 waveforms + meters) | ≤ 8 ms (p95) |
| Track load → playable (analyzed track, 6 min MP3) | ≤ 1.5 s |
| Analysis v1 per 5-min track (one worker) | ≤ 10 s |
| App start with 20k tracks | ≤ 3 s to usable UI |
| Track Matcher: score 5,000 candidates | ≤ 30 ms |
| Smart Sort: 300-track playlist | ≤ 2 s |
| RAM: 2 decks loaded (6-min tracks, float stereo) | ≈ 260 MB + UI |

## 12. Errors and diagnostics

- No exceptions cross thread boundaries; workers return `Result<T, Error>` (`util/Result.h`).
- User-facing errors (file can't decode, device lost) show as non-blocking toasts; details go to the log.
- Device loss: engine keeps state, UI shows "audio device lost", auto-reconnects when the device returns.
