# Devil-DJ — Default Keymap

Left hand plays Deck A, right hand plays Deck B. Bindings use **physical key positions** (scan codes on a US-layout keyboard), so they work with any input language, including the Korean IME. Every binding can be changed in the keymap editor and exported as JSON.

Hold **Ctrl + /** to see the current map drawn on a keyboard. To change keys before the keymap editor exists, put a `keymap.json` in `Documents\Devil-DJ\Keymaps` (Options > Open keymap folder creates the folder and a `keymap.example.json`), then Options > Reload keymap. Each entry moves one action to a new key, e.g. `{ "action": "deckA.Cue", "key": "C" }`; `"key": ""` removes a key. Action names are `deckA.` / `deckB.` + the deck action (Cue, Play, BendDown, KillLow, ...) or `global.` + the global action (CrossfaderCentre, Quantize, ...), as listed in `src/ui/Actions.h`.

Performance keys are active whenever no text field (search box, code editor, rename field) has focus. A unit test checks that the default map has no conflicts.

Implemented so far (P1-16): cue, play, pitch bend, tempo steps, filter sweep / reset, jump to start, EQ kills, key lock, headphone cue, load (Tab / Enter load the file selected in the browser to deck A / B, or open a file chooser if none is selected), browser Up / Down / Page Up / Page Down and Left (parent folder), crossfader glide / snap / centre, full screen (F11), debug overlay (F12), panic (Esc: releases holds and bends). Every other key below is already reserved for its action and does nothing until the feature exists.

## Decks

| Action | Deck A | Deck B |
| --- | --- | --- |
| Cue (CDJ-style; hold to preview) | Z | M |
| Play / pause | X | , |
| Sync on/off | C | . |
| Auto loop on/off (the size: 4 beats to start; halve / double change it when no loop plays) | V | / |
| Pads 1–4 (current pad mode) | Q W E R | U I O P |
| Pads 5–8 | A S D F | J K L ; |
| Pad alternate function (hot cue mode: delete) | Shift + pad | Shift + pad |
| Hot cues (P3-01): an empty pad sets one, a set pad jumps there (paused: plays while held); F1 switches A-H / I-P | pads | pads |
| Pad modes (P3-06): Beat Loop 1/4 1/2 1 2 / 4 8 16 32 beats (same pad again: exit); Beat Jump back / forward 1 2 / 4 8 beats (odd pads back); Key Shift 0 +1 +2 +3 / -4 -3 -2 -1 (Shift + pad: 0); Keyboard: hot cue A at those pitches; Slicer (P3-07): hold a pad to roll its slice, Shift + pad 1 / 2 / 3 = 8 / 16 / 32-beat domain; Pad FX (P3-12): each pad an effect preset (held, or latched), Shift + pad 1 / 2 = bank 1 / 2; Sampler (P3-13): the sampler's bank, Shift + pad 1-4 = bank 1-4 | pads | pads |
| Pitch bend − / + (hold) | 1 / 2 | 7 / 8 |
| Loop halve / double (looping: the loop; else the auto-loop size, 1/64 ... 512 beats) | 3 / 4 | 9 / 0 |
| Loop in / loop out (manual loop) | Ctrl+X / Ctrl+C | Ctrl+, / Ctrl+. |
| Beat jump back / forward (current jump size) | 5 / 6 | - / = |
| Fine jump back / forward 5 ms (repeats while held) | Ctrl+5 / Ctrl+6 | Ctrl+- / Ctrl+= |
| Beat jump size larger / smaller (1/8 ... 128 beats; also the size button on the deck) | Shift+6 / Ctrl+Shift+6 | Shift+= / Ctrl+Shift+= |
| Tempo − / + 0.1 % (repeats while held) | Shift+1 / Shift+2 | Shift+7 / Shift+8 |
| Filter sweep to low-pass / high-pass (hold) | Shift+3 / Shift+4 | Shift+9 / Shift+0 |
| Filter reset | Shift+5 | Shift+- |
| Jump to track start | Shift+Z | Shift+M |
| Reverse (hold; the REV button on the deck toggles it) | Shift+X | Shift+, |
| Capture the loop (or slicer range) to the sampler (P3-14; no default key - keymap.json `deckA.CaptureToSampler`; also the loop menu and the pad mode menu) | - | - |
| Release FX (P3-11): echo out / brake / spin back / filter sweep, held (no default keys - keymap.json: `deckA.ReleaseEchoOut`, `ReleaseBrake`, `ReleaseSpinBack`, `ReleaseSweep`; the FX panel has buttons) | - | - |
| Vinyl brake / spin-back (stop a playing deck; times and whether PAUSE brakes: Decks > Pause / stop) | Ctrl+Shift+D / Ctrl+Shift+F | Ctrl+Shift+L / Ctrl+Shift+; |
| Key Sync (transpose to the master's key; turns key lock on) | Shift+C | Shift+. |
| Key shift -1 / +1 semitone (turns key lock on) | Ctrl+1 / Ctrl+2 | Ctrl+7 / Ctrl+8 |
| Memory cue: store one at the playhead (the MEM button lists them) | Ctrl+Z | Ctrl+M |
| Call the previous / next memory cue (it becomes the CUE point) | Ctrl+3 / Ctrl+4 | Ctrl+9 / Ctrl+0 |
| Reloop / loop exit (looping: exit; else the last loop again from its start) | Shift+V | Shift+/ |
| Low / mid / high EQ kill (toggle) | Ctrl+A / Ctrl+S / Ctrl+D | Ctrl+J / Ctrl+K / Ctrl+L |
| Slip mode | Ctrl+Q | Ctrl+U |
| Key lock | Ctrl+W | Ctrl+I |
| Headphone cue (CUE button) | Ctrl+E | Ctrl+O |
| Make master deck | Ctrl+R | Ctrl+P |

## Mixer, FX and global

| Action | Key |
| --- | --- |
| Crossfader left / right (hold to glide) | G / H |
| Crossfader snap fully left / right | Shift+G / Shift+H |
| Crossfader to centre | B |
| Auto-transition from playing deck to the other / cancel | T / Shift+T |
| Load highlighted Next Track suggestion into the idle deck | N |
| Move highlight down the suggestion list | Shift+N |
| Preview highlighted suggestion in headphones (hold) | Ctrl+N |
| Cycle intent: hold → up → down → switch | Ctrl+Shift+N |
| Rate the last transition 👍 / 👎 (teaches the Track Matcher) | ' / Shift+' |
| Quantize on/off (resolution: Options > Quantize) | ` (backquote) |
| Pad mode: Hot Cue / Beat Loop / Beat Jump / Slicer (both decks; F1 again in Hot Cue: pads A-H / I-P; each deck's mode button picks its own) | F1 / F2 / F3 / F4 |
| Pad mode: Key Shift / Keyboard / Pad FX / Sampler | Shift+F1 / Shift+F2 / Shift+F3 / Shift+F4 |
| FX1 on/off (the slots with an effect) / slot 1's next effect | F5 / F6 |
| FX2 on/off / next effect | F7 / F8 |
| Cycle FX1 / FX2 target (deck A, deck B, master; the sampler, Beat Maker and code deck join when they exist) | Shift+F5 / Shift+F7 |
| Record start/stop | F9 |
| Automix on/off | F10 |
| Full screen | F11 |
| Debug overlay (CPU, xruns, late events) | F12 |
| Panic: release all loops, rolls, holds and FX | Esc |
| Show keymap overlay (hold) | Ctrl+/ |

Space is deliberately unbound in performance view to avoid accidental stops; it plays/stops the Beat Maker when that panel has focus.

## Beatgrid (P2-14)

| Action | Deck A | Deck B |
| --- | --- | --- |
| Set downbeat here (the nearest beat moves to the playhead and becomes beat 1) | Ctrl+Shift+Z | Ctrl+Shift+M |
| Grid 1 ms earlier / later (hold to repeat) | Ctrl+Shift+A / S | Ctrl+Shift+J / K |
| Downbeat one beat earlier / later | Ctrl+Shift+Q / W | Ctrl+Shift+U / I |
| BPM x2 / /2 | Ctrl+Shift+E / R | Ctrl+Shift+O / P |
| Tap tempo (4 taps or more, in time with the track) | Ctrl+Shift+X | Ctrl+Shift+, |
| Metronome on / off (clicks on the beats, in the headphones) | Ctrl+Shift+C | Ctrl+Shift+. |
| Lock / unlock the grid (analysis leaves a locked grid alone) | Ctrl+Shift+V | Ctrl+Shift+/ |

The same tools are on each deck's GRID button (and TAP); editing a grid locks it.

## Browser

| Action | Key |
| --- | --- |
| Move selection | Up / Down, Page Up / Page Down |
| Collapse / expand tree node | Left / Right |
| Focus search box | Ctrl+F (Enter or Esc returns to performance keys) |
| Bigger browser (over the waveforms) / back | Ctrl+B |
| Remove selected tracks from the shown playlist | Delete |
| Select several tracks | Ctrl+click / Shift+click |
| Load selected track to Deck A / B | Tab / Enter |
| Load to Deck C / D (4-deck mode) | Shift+Tab / Shift+Enter |
| Rate selected track(s) 1–5 / clear | Ctrl+Shift+1…5 / Ctrl+Shift+0 (or click a star; the same star again clears) |
| Preview selected track in headphones (hold) | Ctrl+Space |
| Switch bottom panel: Browser → Beat Maker → Code Beat | Ctrl+Tab |

## Beat Maker (when its panel has focus)

| Action | Key |
| --- | --- |
| Start / stop (quantized to the bar) | Space |
| Move step cursor | Arrow keys |
| Toggle step | Enter |
| Velocity − / + on cursor step | Shift+Down / Shift+Up |
| Select drum track 1–8 | 1–8 |
| Select pattern A–H | Ctrl+1…8 |
| Copy / paste / clear steps | Ctrl+C / Ctrl+V / Delete |
| Undo / redo | Ctrl+Z / Ctrl+Y |

## Code Beat (when the editor has focus)

| Action | Key |
| --- | --- |
| Evaluate (swap at next cycle) | Ctrl+Enter |
| Evaluate immediately | Ctrl+Shift+Enter |
| Hush (silence code deck) | Ctrl+. |
| Toggle console panel | Ctrl+` |
| Next / previous buffer | Ctrl+PageDown / Ctrl+PageUp |
| Autocomplete | Ctrl+Space |

## Mouse, trackpad and touch

| Gesture | Effect |
| --- | --- |
| Drag on zoomed waveform | Scratch (playing) or scrub (paused); release resumes |
| Scroll on zoomed waveform | Fine nudge |
| Ctrl + scroll on waveform | Zoom |
| Click on overview waveform | Seek (quantized if quantize is on) |
| Drag knob / fader vertically | Change value; Alt = fine |
| Scroll over knob / fader | Change value |
| Double-click knob / fader | Reset to default |
| Right-click any control | MIDI Learn / bind key / reset |
| Two-finger touch on two faders | Both move independently (multi-touch) |

Zoomed waveform gestures: grab-and-move (dragging left pushes the track forward, so the waveform follows the hand); one wheel notch nudges 5 ms; Ctrl + wheel zooms between 2 and 32 s visible.

## "Laptop compact" preset

For laptops where F-keys need Fn: pad modes move to Ctrl+Shift+Q/W/E/R (Hot Cue, Beat Loop, Beat Jump, Slicer) and FX on/off to Ctrl+Shift+A/S; everything else is unchanged.
