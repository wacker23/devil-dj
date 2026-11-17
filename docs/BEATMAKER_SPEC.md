# Beat Maker — Spec

The Beat Maker is a groovebox inside Devil-DJ. Its patterns play on the master clock through their own mixer channel and share the engine's `Scheduler` and `Voices` with Code Beat.

## 1. Data model

```cpp
namespace devildj::beatmaker {

constexpr int kPpq = 96;                    // ticks per quarter note (beat)

struct Step {
    bool on = false;
    std::uint8_t velocity = 100;            // 1..127
    std::uint8_t probability = 100;         // 0..100 %
    std::int8_t microTiming = 0;            // −50..+50 % of a step
    std::uint8_t ratchet = 1;               // 1..4 hits within the step
};

struct DrumTrack {
    std::string name;
    SoundRef sound;                         // drum synth id + params, or user sample path
    std::vector<Step> steps;                // size = pattern length
    float volumeDb = 0, pan = 0.5f, delaySend = 0, reverbSend = 0;
    std::uint8_t chokeGroup = 0;            // 0 = none
    bool mute = false, solo = false;
};

struct Note { int startTick; int lengthTicks; std::uint8_t pitch; std::uint8_t velocity; };

struct SynthTrack {
    std::string name;
    SynthPreset preset;                     // engine type + parameters
    std::vector<Note> notes;
    ArpSettings arp;                        // off by default
    float volumeDb = 0, pan = 0.5f, delaySend = 0, reverbSend = 0;
    bool mute = false, solo = false;
};

struct Pattern {
    std::string name;                       // "A".."H"
    int lengthSteps = 16;                   // 16, 32 or 64
    int stepsPerBeat = 4;                   // 16th notes
    std::vector<DrumTrack> drums;           // up to 8
    std::vector<SynthTrack> synths;         // up to 4
};

struct Project {
    int schemaVersion = 1;
    bool followMaster = true; double ownBpm = 120.0;
    float swing = 0.0f;                     // 0..0.75, applied to every 2nd step
    std::array<Pattern, 8> patterns;
    std::vector<int> song;                  // pattern indices in order; empty = loop current
    std::uint32_t seed = 1;                 // probability RNG
};
}
```

File format: `.ddjbeat` = JSON of `Project` (+ `samples` array of relative paths). Unknown fields are preserved on load/save.

## 2. Sequencer → events

`Sequencer::emit(beatBegin, beatEnd)` is called from a Beat Maker timer every 25 ms with a 100 ms look-ahead window in beats (same cursor logic as Code Beat, `CODEBEAT_SPEC.md` §8.1, with an exact `Fraction` cursor):

1. For each step whose nominal tick lies in the window: apply swing (delay odd steps by `swing × stepTicks / 2`), then micro-timing.
2. Probability: `hash(seed, patternIndex, track, step, loopCount) < probability`.
3. Ratchets: split the step into `ratchet` equal hits, velocity decays 10 % per hit.
4. Solo/mute rules: if any track is soloed, only soloed tracks play.
5. Synth notes: note-on at `startTick`, duration `lengthTicks`; arpeggiator expands held chords to a rate (1/8, 1/16, 1/32) and mode (up, down, up-down, random, as played).
6. Convert beats → samples via `MasterClock`, push `ScheduledEvent`s.

Pattern switching: the queued pattern starts at the next bar (`beat mod 4 == 0`) or at the end of the current pattern (setting). Song mode advances at pattern end.

## 3. Voices

Uses `engine::Voices` (64 sample voices, 32 synth voices, 16 drum-synth voices per source). Choke groups: a new hit in a group releases the other voices in that group with a 5 ms fade (open/closed hat).

## 4. Drum synths (our own DSP, rendered live)

All drum voices: output through a soft clipper (drive parameter) and a per-voice SVF.

| Sound | Recipe | Parameters |
| --- | --- | --- |
| Kick | Sine oscillator with exponential pitch envelope (start 4–8× base, 20–80 ms), amplitude envelope (decay 100–1,500 ms), short filtered noise click at the start | tune (35–80 Hz), decay, pitch amount, click, drive |
| Snare | Two detuned sine/triangle bodies (150–250 Hz) with fast decay + band-passed white noise with its own decay | tune, tone (body/noise mix), snap (noise decay), decay |
| Clap | Band-passed noise (≈1–1.5 kHz) with 3–4 rapid retriggered envelopes 8–12 ms apart, then a longer tail | tone, spread, decay |
| Closed / open hat | Six square oscillators at inharmonic ratios summed ("metallic"), band-pass + high-pass ≈7 kHz, short (closed) or long (open) decay; choke group shared | tune, tone, decay |
| Toms (low/mid/high) | Sine with small downward pitch envelope + a touch of noise | tune, decay, pitch amount |
| Rim | Short high sine burst + band-passed click | tune, decay |
| Cowbell | Two square oscillators (≈540 / 800 Hz ratio) through a band-pass, two-stage decay | tune, decay |
| Shaker | High-passed noise with a slightly slow attack | tone, decay |

Each sound ships with 3–5 presets (e.g. "Tight", "Boomy", "Distorted"). Kits = named sets of 8 track sounds ("Club", "Minimal", "Breaks", "Trap", "Lo-fi").

## 5. Synths

| Engine | Structure | Notes |
| --- | --- | --- |
| Subtractive | 2 oscillators (saw/square/tri/sine, detune, octave) + sub (−1 oct square/sine) + noise → TPT SVF (LP/BP/HP 12/24 dB, cutoff, resonance, env amount, key tracking) → VCA; ADSR for filter and amp; glide; 8-voice unison "supersaw" mode | PolyBLEP oscillators to avoid aliasing |
| FM | 2 operators (carrier + modulator), ratio, index with its own envelope, feedback | Bells, plucks, basses |
| Sub-bass | Sine + optional 2nd harmonic, drive, glide, short attack click | 808-style long bass with pitch glide |

Presets: 10 per engine, stored as JSON in `Documents\Devil-DJ\Beats\Presets`.

## 6. UI

- **Step grid**: rows = drum tracks, columns = steps (16 visible, page for 32/64), bar/beat shading, playing-step highlight; click toggles, drag paints, Alt-drag sets velocity; per-step popover for probability, micro-timing, ratchet.
- **Track header**: name, sound picker, mute/solo, volume, pan, sends, choke group.
- **Piano roll**: per synth track; grid snap (1/4…1/32, triplets), draw/move/resize notes, velocity lane, scale lock (highlight in-scale rows, optional snapping), chord stamp (maj, min, 7, maj7, min7, sus2, sus4), quantize, duplicate pattern section.
- **Pattern bar**: A–H, copy/paste/clear, length, swing, follow-master/BPM, play/stop (quantized to bar), song mode editor.
- **Keyboard**: arrows move the cursor, Enter toggles, 1–8 select track, Shift+arrows adjust velocity, Ctrl+C/V copy steps, Space start/stop Beat Maker only (when its panel has focus).

## 7. Output and hand-off

- Mixer channel "BEATS" with strip, cue, crossfader assign, FX target.
- Render: pattern or song → WAV (offline, faster than real time, same DSP code path → bit-identical for deterministic settings), → sampler slot, → library track (imported and analyzed).
- Drum Swap source (Phase 6): a pattern can replace a deck's drums stem in sync.

## 8. Tests

- Sequencer timing at 90/120/128/174 BPM with swing 0/0.5 and micro-timing ±50 % (event sample positions exact).
- Probability determinism with a fixed seed.
- Choke group behaviour.
- Drum synth sweeps: no NaN/Inf, peak ≤ +6 dBFS before the channel strip.
- Serialization round-trip and forward-compatibility (unknown fields kept).
- Offline render == real-time render (deterministic pattern).
