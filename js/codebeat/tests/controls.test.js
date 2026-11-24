import { test } from "node:test";
import assert from "node:assert/strict";
import { seq, cat, sortEvents } from "../pattern.js";
import { mini } from "../mini.js";
import {
    s, sound, n, note, gain, pan, speed, lpf, cutoff, hpf, resonance, attack, decay, sustain, release, legato, room,
    delay, crush, cut, bank, noteNameToMidi, ControlError, kControls,
} from "../controls.js";

const values = (p, b = 0, e = 1) => sortEvents(p.queryArc(b, e)).map((x) => x.value);
const show = (p, b = 0, e) => p.show(b, e);

test("s / sound: a sound per event, with :n", () => {
    assert.deepEqual(show(s("kick hat")), ["s:kick [0, 1/2)", "s:hat [1/2, 1)"]);
    assert.deepEqual(values(sound("kick:3 hat")), [{ s: "kick", n: 3 }, { s: "hat" }]);
    assert.deepEqual(values(s("<bd sd>"), 1, 2), [{ s: "sd" }]);
    assert.deepEqual(values(s(seq("bd:2", "sd"))), [{ s: "bd", n: 2 }, { s: "sd" }]);
});

test("note: names and MIDI numbers", () => {
    assert.equal(noteNameToMidi("c3"), 48);
    assert.equal(noteNameToMidi("a4"), 69);
    assert.equal(noteNameToMidi("c#3"), 49);
    assert.equal(noteNameToMidi("cs3"), 49);
    assert.equal(noteNameToMidi("eb"), 51); // octave 3 by default
    assert.equal(noteNameToMidi("bb2"), 46);
    assert.equal(noteNameToMidi("b"), 59);
    assert.equal(noteNameToMidi("C4"), 60);
    assert.equal(noteNameToMidi("c-1"), 0);
    assert.equal(noteNameToMidi("e##3"), 54);
    assert.equal(noteNameToMidi("h3"), null);
    assert.equal(noteNameToMidi("kick"), null);
    assert.deepEqual(values(note("c3 e3 g3")), [{ note: 48 }, { note: 52 }, { note: 55 }]);
    assert.deepEqual(values(note("60 64.5")), [{ note: 60 }, { note: 64.5 }]);
    assert.deepEqual(values(note(seq(60, "a4"))), [{ note: 60 }, { note: 69 }]);
    assert.throws(() => note("kick").queryArc(0, 1), /note needs a note name/);
});

test("every control sets its own field", () => {
    const cases = [
        [n, "3", { n: 3 }], [gain, "0.8", { gain: 0.8 }], [pan, "0.25", { pan: 0.25 }], [speed, "-1", { speed: -1 }],
        [lpf, "800", { lpf: 800 }], [cutoff, "800", { lpf: 800 }], [hpf, "200", { hpf: 200 }],
        [resonance, "0.3", { resonance: 0.3 }], [attack, "0.01", { attack: 0.01 }], [decay, "0.2", { decay: 0.2 }],
        [sustain, "0.5", { sustain: 0.5 }], [release, "1", { release: 1 }], [legato, "2", { legato: 2 }],
        [room, "0.4", { room: 0.4 }], [delay, "0.3", { delay: 0.3 }], [crush, "4", { crush: 4 }], [cut, "1", { cut: 1 }],
        [bank, "tr8", { bank: "tr8" }],
    ];
    for (const [f, arg, expected] of cases)
        assert.deepEqual(values(f(arg)), [expected], JSON.stringify(expected));
    // Plain values work as well as strings.
    assert.deepEqual(values(gain(0.5)), [{ gain: 0.5 }]);
});

test("values outside the range are kept inside it", () => {
    assert.deepEqual(values(gain(3)), [{ gain: 1.5 }]);
    assert.deepEqual(values(gain(-1)), [{ gain: 0 }]);
    assert.deepEqual(values(pan(2)), [{ pan: 1 }]);
    assert.deepEqual(values(speed(9)), [{ speed: 4 }]);
    assert.deepEqual(values(lpf(5)), [{ lpf: 20 }]);
    assert.deepEqual(values(crush(0)), [{ crush: 1 }]);
    assert.deepEqual(values(note(200)), [{ note: 127 }]);
});

test("values that aren't numbers are errors that name the control", () => {
    assert.throws(() => gain("loud").queryArc(0, 1), (e) => e instanceof ControlError && /gain needs a number, got "loud"/.test(e.message));
    assert.throws(() => n("1.5").queryArc(0, 1), /n needs a whole number, got 1.5/);
    assert.throws(() => cut(0.5).queryArc(0, 1), /cut needs a whole number/);
    assert.throws(() => s(3).queryArc(0, 1), /s needs a sound name, got 3/);
});

test("methods: structure from the left, controls merged", () => {
    assert.deepEqual(show(s("kick hat").gain(0.8)), ["s:kick gain:0.8 [0, 1/2)", "s:hat gain:0.8 [1/2, 1)"]);
    // A patterned control: each left event takes the value active during it.
    assert.deepEqual(values(s("hat*4").gain("1 0.5")), [
        { s: "hat", gain: 1 }, { s: "hat", gain: 1 }, { s: "hat", gain: 0.5 }, { s: "hat", gain: 0.5 },
    ]);
    // Chains, and the later value wins.
    assert.deepEqual(values(note("c3").s("saw").lpf(1200).gain(0.5).gain(0.7)), [{ note: 48, s: "saw", lpf: 1200, gain: 0.7 }]);
    // Alternating per cycle.
    assert.deepEqual(values(s("bd").speed("<1 2>"), 1, 2), [{ s: "bd", speed: 2 }]);
    // Structure stays the left's: two onsets, not three.
    assert.equal(s("bd sd").pan("0 0.5 1").onsets(0, 1).length, 2);
});

test("controls on plain mini-notation: words are sounds, numbers are n", () => {
    assert.deepEqual(values(mini("kick hat").gain(0.5)), [{ s: "kick", gain: 0.5 }, { s: "hat", gain: 0.5 }]);
    assert.deepEqual(values(mini("0 2").s("piano")), [{ n: 0, s: "piano" }, { n: 2, s: "piano" }]);
    assert.deepEqual(values(mini("bd:2").gain(1)), [{ s: "bd", n: 2, gain: 1 }]);
});

test("controls combine with the pattern core", () => {
    const p = cat(s("bd*2"), s("sd").fast(3)).gain(0.9);
    assert.equal(p.onsets(0, 1).length, 2);
    assert.equal(p.onsets(1, 2).length, 3);
    assert.deepEqual(show(s("bd sd").rev()), ["s:sd [0, 1/2)", "s:bd [1/2, 1)"]);
    // Source locations survive: the sound's and the control value's.
    const [e] = s("kick").gain("0.5").queryArc(0, 1);
    assert.equal(e.context.locations.length, 2);
});

test("the control table lists every name the plan asks for", () => {
    for (const name of ["s", "n", "note", "gain", "pan", "speed", "lpf", "hpf", "resonance", "room", "delay", "crush",
        "attack", "decay", "sustain", "release", "cut", "bank", "legato"])
        assert.ok(kControls[name], name);
});
