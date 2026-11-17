# Code Beat — Language and Runtime Spec

Code Beat is Devil-DJ's live-coding deck. It follows the *ideas* of the TidalCycles family (cyclic time, patterns as functions of time, a compact mini-notation), implemented from scratch for Devil-DJ. **Do not copy code or documentation text from Strudel or TidalCycles** (AGPL/GPL); this spec is the source of truth.

## 1. Goals and non-goals

Goals: musical patterns typed live, locked to the DJ master clock; errors never stop the sound; deterministic output (same code + same seed → same events); low CPU; small, readable implementation.

Non-goals for v1: visuals, MIDI out (tier 3), compatibility with other live-coding systems beyond familiar syntax, network collaboration.

## 2. Time model

- Time is measured in **cycles**. By default 1 cycle = 1 bar = 4 beats of the master clock (`setcpb(n)` changes beats per cycle).
- All pattern-time arithmetic uses exact rationals (`Fraction`), never floats.
- A **span** is `[begin, end)` in cycles.
- An **event** is `{ whole, part, value, context }`:
  - `whole`: the event's full span (may extend beyond the queried range); `null` for continuous signals.
  - `part`: the portion of `whole` inside the queried span.
  - `value`: a plain object of controls (`{ s: "kick", gain: 0.8 }`) or a primitive before controls are applied.
  - `context.locations`: source ranges of the atoms that produced it (for highlighting).
- **Onset rule**: an event triggers a sound only if `whole` exists and `part.begin === whole.begin` (the onset falls inside the query). The scheduler ignores other fragments.
- `Pattern` = an object with `query(state) → Event[]` where `state.span` is the queried span. Querying across cycle boundaries is allowed; core operators split at integer cycles where needed.

## 3. Core semantics (each line is a required test)

Notation: `name [begin, end)`; queries are over cycle 0 unless stated.

| Expression | Events |
| --- | --- |
| `pure("a")` | `a [0,1)` |
| `fastcat("a", "b")` (= `seq`) | `a [0,1/2)`, `b [1/2,1)` |
| `slowcat("a", "b")` (= `cat`) | cycle 0: `a [0,1)`; cycle 1: `b [1,2)`; cycle 2: `a [2,3)` |
| `stack("a", "b")` | `a [0,1)`, `b [0,1)` |
| `pure("a").fast(2)` | `a [0,1/2)`, `a [1/2,1)` |
| `pure("a").slow(2)` | cycle 0: whole `[0,2)` part `[0,1)` (onset); cycle 1: whole `[0,2)` part `[1,2)` (no onset) |
| `seq("a","b").rev()` | `b [0,1/2)`, `a [1/2,1)` |
| `seq("a","b").early(1/4)` | `b [1/4,3/4)`, `a [3/4,5/4)` (and the tail of the previous `a` with part `[0,1/4)`) |
| `seq("a","b","c","d").every(2, x => x.rev())` | cycle 0 reversed, cycle 1 normal (every(n) applies on cycles where `cycle mod n === 0`) |
| `s("kick").off(1/8, x => x.gain(0.5))` | `kick [0,1)`, `kick gain 0.5 [1/8,9/8)` |

Patterned arguments: any numeric argument may itself be a pattern or mini-notation string, e.g. `.fast("<1 2>")` — structure comes from the left (outer) pattern unless the function documents otherwise.

## 4. Mini-notation

Strings passed to pattern functions (`s`, `note`, `n`, `gain`, …) are parsed as mini-notation.

### 4.1 Grammar (EBNF)

```
stack      = sequence { "," sequence } ;
sequence   = step { ws step } ;
step       = term { modifier } ;
term       = atom | rest | "[" ws? stack ws? "]" | "<" ws? stack ws? ">" ;
atom       = word | number ;
rest       = "~" | "-" ;
modifier   = "*" factor
           | "/" factor
           | "!" [ integer ]            (* replicate: "a!3" = "a a a"; bare "!" repeats the previous step *)
           | "@" number                 (* elongate: relative weight in the sequence *)
           | "?" [ number ]             (* drop with probability (default 0.5), seeded *)
           | "(" factor "," factor [ "," factor ] ")"   (* euclid (pulses, steps, rotation) *)
           | ":" integer ;              (* sample index, sets n *)
factor     = number | "[" ws? stack ws? "]" | "<" ws? stack ws? ">" ;
word       = letter { letter | digit | "#" | "_" | "." } ;
number     = [ "-" ] digit { digit } [ "." digit { digit } ] ;
ws         = ( " " | "\t" | "\n" ) { " " | "\t" | "\n" } ;
```

Tier 2 additions (not in v1): polymeter `{a b c}%4`, `.` grouping, `|` random choice.

### 4.2 Semantics and required tests (cycle 0 unless stated)

| Mini-notation | Meaning / events |
| --- | --- |
| `"a b"` | `a [0,1/2)`, `b [1/2,1)` |
| `"a ~ b"` | `a [0,1/3)`, `b [2/3,1)` |
| `"a [b c]"` | `a [0,1/2)`, `b [1/2,3/4)`, `c [3/4,1)` |
| `"<a b>"` | cycle 0 `a [0,1)`, cycle 1 `b [1,2)` |
| `"a <b c>"` | cycle 0: `a`, `b`; cycle 1: `a`, `c` |
| `"a*2 b"` | `a [0,1/4)`, `a [1/4,1/2)`, `b [1/2,1)` |
| `"a/2"` | one event whole `[0,2)` (onset in cycle 0 only) |
| `"a, b c"` | `a [0,1)`, `b [0,1/2)`, `c [1/2,1)` |
| `"a!3 b"` | four steps: `a a a b`, each 1/4 |
| `"a@3 b"` | `a [0,3/4)`, `b [3/4,1)` |
| `"a(3,8)"` | onsets at steps 0, 3, 6 of 8: `[0,1/8)`, `[3/8,1/2)`, `[3/4,7/8)` |
| `"a(3,8,2)"` | the (3,8) rhythm rotated left by 2 steps: onsets at steps 1, 4, 6 |
| `"a?"` | `a` kept or dropped with p = 0.5 using the cycle-seeded RNG (deterministic per seed) |
| `"kick:3"` | `{ s: "kick", n: 3 }` |
| `"0 .5 -1"` | numbers `0`, `0.5`, `-1` (number atoms are values, not words) |

Euclidean rhythms use the Bjorklund algorithm (implement from the algorithm description; test (3,8), (5,8), (7,16), (4,12)).

### 4.3 Source locations

Each atom records `{ start, end }` offsets inside its string literal. The pre-tokenizer (§8.3) adds the literal's offset in the editor buffer, so events can be highlighted in the code.

### 4.4 Errors

Parser errors report `line:column` in the editor buffer and a short message, e.g. `2:14 unexpected ")" — did you forget "("?`. Unknown words are not parser errors (they are sound names); unknown sounds produce a runtime warning (§9).

## 5. Function reference (v1)

### 5.1 Pattern constructors

`pure(v)`, `silence`, `seq(...)` / `fastcat`, `cat(...)` / `slowcat`, `stack(...)`, `mini(str)`, `run(n)` (0..n−1), `euclid(k, n, r?)`.

### 5.2 Time

`fast(f)`, `slow(f)`, `early(t)`, `late(t)`, `rev()`, `iter(n)`, `palindrome()`, `ply(n)`, `hurry(f)` (fast + speed), `segment(n)` (sample a signal n times per cycle).

### 5.3 Structure and randomness

`every(n, fn)`, `whenCycle(predicate, fn)`, `sometimesBy(p, fn)`, `sometimes(fn)` (0.5), `often(fn)` (0.75), `rarely(fn)` (0.25), `degradeBy(p)`, `off(t, fn)`, `superimpose(fn)`, `layer(...fns)`, `struct("x ~ x x")`, `mask("1 0 1 1")`, `seed(n)`.

Randomness: a deterministic hash of `(seed, cycle, event position)` → [0,1). Default seed 0; `seed(n)` per pattern.

### 5.4 Arithmetic on values

`add(x)`, `sub(x)`, `mul(x)`, `div(x)`, `range(lo, hi)` (for signals), `scale("C:minor")` (maps scale degrees in `n` to `note`; root octave 3; scales: major, minor, dorian, phrygian, lydian, mixolydian, locrian, harmonicMinor, melodicMinor, majPent, minPent, blues, chromatic).

Note names: `c d e f g a b` with `#`/`s` sharp and `b` flat, optional octave; default octave 3; `c3` = MIDI 48, `a4` = MIDI 69 (440 Hz).

### 5.5 Signals (continuous, no `whole`)

`sine`, `cosine`, `saw`, `isaw`, `tri`, `square`, `rand`, `irand(n)`, `perlin` — all in [0,1) over one cycle; use with `.range(lo, hi)` and `.segment(n)` or as patterned arguments: `s("hat*8").gain(sine.range(0.4, 1))`.

### 5.6 Controls (set fields on event values)

| Control | Meaning | Range / unit |
| --- | --- | --- |
| `s(name)` / `sound` | sound name (+ `:n`) | see §10 |
| `n(x)` | sample index or scale degree | integer |
| `note(x)` | pitch | note name or MIDI number |
| `gain(x)` | level | 0–1.5 (1 = unity) |
| `pan(x)` | stereo position | 0 left … 1 right |
| `speed(x)` | sample playback rate | −4…4 (negative = reverse) |
| `lpf(x)` / `cutoff` | low-pass cutoff | Hz |
| `hpf(x)` | high-pass cutoff | Hz |
| `resonance(x)` | filter resonance | 0–1 |
| `attack/decay/sustain/release` | envelope | seconds / level |
| `legato(x)` | duration relative to event length | 0–4 |
| `room(x)` | reverb send | 0–1 |
| `delay(x)` | delay send (tempo-synced 3/16 by default) | 0–1 |
| `crush(x)` | bit crush depth | 1–16 bits |
| `cut(x)` | choke group | integer |

### 5.7 Program functions

`play(pattern)` / `play(name, pattern)` (named slots, all slots stack), `mute(name)`, `unmute(name)`, `hush()`, `setbpm(x)` (only when the code deck is clock master), `setcpb(n)`, `print(x)` (to the console panel).

DJ hooks (read-only, refreshed before every query): `deckA`…`deckD` = `{ bpm, key, keyName, beat, bar, phrase, playing }`, `master = { bpm, beat }`.

## 6. Mapping events to the engine

For each onset event the bridge builds a `ScheduledEvent` (see `ARCHITECTURE.md` §3.7):

- `sampleTime = clock.sampleAtCycle(whole.begin) + userNudgeSamples`
- `durationSeconds = clock.secondsBetween(whole.begin, whole.end) * legato`
- `soundIndex` resolved from `s` + `n` via the `SoundBank` on the code-deck thread (never on the audio thread)
- unspecified controls take defaults (gain 1, pan 0.5, speed 1, no filter, no sends)
- `highlightToken` indexes a small table of source locations published to the editor

## 7. JS library layout (`js/codebeat/`)

```
package.json        {"type": "module"}  (for node --test only)
fraction.js         exact rationals
span.js             spans, cycle splitting
event.js            Event class, onset test
pattern.js          Pattern class, core operators, patterned arguments
mini.js             tokenizer + recursive-descent parser → patterns with locations
euclid.js           Bjorklund
signals.js          continuous patterns
transforms.js       every, off, sometimes, iter, ply, ...
controls.js         s, n, note, gain, ... (+ note-name parsing)
scales.js           scale tables
random.js           deterministic hash RNG
prelude.js          builds the global API object, play/hush/named slots
tests/*.test.js     node --test; the same tests run inside QuickJS in a Catch2 test
```

Plain modern JS (ES2023), no dependencies, no build step. Modules are loaded in QuickJS through a module loader that resolves names from embedded binary data.

## 8. Runtime (C++)

### 8.1 Code deck thread loop

```
loop every 25 ms:
    snapshot = engine clock snapshot (sampleTime0, beat0, bpm, sampleRate)    // from EngineState
    if pending code: evaluate (budget 200 ms); on success set pendingPattern, else report error
    nowCycle   = cycleAt(snapshot, engine.currentSampleTime())
    if cursor == none or |cursor - nowCycle| > 0.25: cursor = rationalize(nowCycle); log re-anchor
    endCycle   = rationalize(cycleAt(snapshot, now + 100 ms), 1/96000)
    if pendingPattern and swap boundary b = ceil(cursor) <= endCycle:
        emit(activePattern, cursor, b); activePattern = pendingPattern; emit(activePattern, b, endCycle)
    else:
        emit(activePattern, cursor, endCycle)
    cursor = endCycle
```

`emit(p, c0, c1)` queries `p` over `[c0, c1)` (budget 5 ms), keeps onset events, maps them to `ScheduledEvent`s and pushes them to the code deck's SPSC queue. The cursor is an exact `Fraction`, so consecutive windows never overlap or leave gaps.

### 8.2 Sandbox

- quickjs-ng runtime per code deck; `JS_SetMemoryLimit(64 MB)`, `JS_SetMaxStackSize(1 MB)`.
- `JS_SetInterruptHandler` enforces deadlines (eval 200 ms, query 5 ms). On a query timeout the window is skipped (silence), the pattern is marked "too slow", and the previous pattern is restored.
- No `std`/`os` modules, no `setTimeout`, no file or network access. `print()` goes to the console panel.

### 8.3 Pre-tokenizer for highlighting

Before evaluation, a small tokenizer (C++) scans the buffer for string literals (`"…"`, `'…'`, and backtick literals without `${}`) that are in argument position (not object keys). Each is rewritten to `__mini("…", <offset>)`, which returns a `MiniString` object (`toString()` gives the text). All API functions accept strings or `MiniString`; the parser adds `offset` to atom locations. Covered by tests including comments, escaped quotes and object literals.

### 8.4 Hot swap rules

- Ctrl+Enter: swap at the next cycle boundary (default) — `pendingPattern` as in §8.1.
- Ctrl+Shift+Enter: swap at the next window (immediate).
- Errors: the previous pattern keeps playing; the error is shown inline; nothing is swapped.
- `hush()` / Ctrl+.: active pattern becomes `silence` immediately; voices release with their envelopes.

## 9. Diagnostics shown to the user

Status line: cycle number, cycles per minute, events/s, late events, last eval time. Console panel: errors, `print()` output, warnings (unknown sound `"kik"` — did you mean `"kick"`?, query too slow, re-anchor after a tempo jump).

## 10. Sound bank names (v1)

| Name | Source |
| --- | --- |
| `kick`, `bd` | Beat Maker kick synth |
| `snare`, `sd` | snare synth |
| `clap`, `cp` | clap synth |
| `hat`, `hh` / `ohat`, `oh` | closed / open hat synth |
| `tom`, `lt`, `mt`, `ht` | toms |
| `rim`, `rs` / `cow`, `cb` / `shaker`, `sh` | percussion synths |
| `sine`, `saw`, `square`, `tri`, `supersaw`, `subbass`, `fm` | synth voices (use with `note`) |
| any folder name | `Documents\Devil-DJ\Samples\<name>\*.wav`, sorted; `:n` picks the file (wraps) |

## 11. Starter examples (write our own; these are placeholders for the examples browser)

```javascript
// 1. Four on the floor
s("kick*4, ~ clap ~ clap, hat*8").gain("1 0.7")

// 2. Follow the DJ: bass in the key of deck A
n("0 ~ 3 [5 3]").scale(deckA.keyName).s("subbass").lpf(600)

// 3. Euclid percussion with variation
stack(
  s("kick(3,8)"),
  s("rim(5,8,2)").sometimes(x => x.speed(2)),
  s("hat*16").gain(saw.range(0.3, 0.9))
)

// 4. Named slots you can mute live
play("drums", s("kick*4, hat*8"))
play("keys",  note("<[c3,e3,g3] [a2,c3,e3]>").s("saw").lpf(1200).room(0.3))
```

## 12. Testing

- JS unit tests for every row in §3 and §4.2, every function in §5 (at least one test each), RNG determinism, and parser error messages.
- The same test files run inside QuickJS from a Catch2 test (`tests/codebeat/QuickJsParity.cpp`) to catch engine differences.
- Engine tests: scheduler timing (±1 sample over 10 min at 128 BPM), hot-swap continuity, sandbox limits, late-event counting under an artificially blocked thread.
