// Controls (CODEBEAT_SPEC §5.6): functions that set fields on event values.
//
//   s("kick hat")             a pattern of { s: "kick" }, { s: "hat" }
//   s("kick hat").gain(0.8)   the same events with gain: 0.8 added (structure from the left)
//   note("c3 e3 g3")          { note: 48 }, ... (note names become MIDI numbers)
//
// Every argument may be a value, a pattern or mini-notation. Values are checked and kept inside the ranges in §5.6
// (a gain of 3 plays as 1.5), so a typo can't blow the speakers; a value that isn't a number at all is an error.

import { Pattern, reify } from "./pattern.js";
import "./mini.js"; // strings are mini-notation

export class ControlError extends TypeError {
    constructor(message) {
        super(message);
        this.name = "ControlError";
    }
}

// --- note names ---

const kPitchClass = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };

// "c3" = 48, "a4" = 69, "c#3" / "cs3" = 49, "eb" = 51 (octave 3 when none is given), "bb2" = 46. null if it isn't one.
export function noteNameToMidi(name) {
    const m = /^([a-g])([#sb]*)(-?\d+)?$/i.exec(String(name));
    if (!m)
        return null;
    let pc = kPitchClass[m[1].toLowerCase()];
    for (const c of m[2].toLowerCase())
        pc += c === "b" ? -1 : 1;
    const octave = m[3] === undefined ? 3 : Number(m[3]);
    return 12 * (octave + 1) + pc;
}

// --- value checks ---

function toNumber(name, v) {
    if (typeof v === "number" && Number.isFinite(v))
        return v;
    if (typeof v === "string" && v.trim() !== "" && Number.isFinite(Number(v)))
        return Number(v);
    throw new ControlError(`${name} needs a number, got ${JSON.stringify(v)}`);
}

const clamp = (lo, hi) => (name, v) => Math.min(hi, Math.max(lo, toNumber(name, v)));

const integer = (lo, hi) => (name, v) => {
    const x = toNumber(name, v);
    if (!Number.isInteger(x))
        throw new ControlError(`${name} needs a whole number, got ${x}`);
    return Math.min(hi, Math.max(lo, x));
};

function soundName(name, v) {
    if (typeof v === "string" && v !== "")
        return v;
    throw new ControlError(`${name} needs a sound name, got ${JSON.stringify(v)}`);
}

function pitch(name, v) {
    if (typeof v === "number" && Number.isFinite(v))
        return Math.min(127, Math.max(0, v));
    if (typeof v === "string") {
        const midi = noteNameToMidi(v);
        if (midi !== null)
            return Math.min(127, Math.max(0, midi));
        if (v.trim() !== "" && Number.isFinite(Number(v)))
            return Math.min(127, Math.max(0, Number(v)));
    }
    throw new ControlError(`${name} needs a note name (c3, f#4, eb) or a MIDI number, got ${JSON.stringify(v)}`);
}

// name -> how its value is checked. Aliases share the field of the name they point to.
export const kControls = {
    s: soundName,
    n: integer(-1e6, 1e6),
    note: pitch,
    gain: clamp(0, 1.5),
    pan: clamp(0, 1),
    speed: clamp(-4, 4),
    lpf: clamp(20, 20000),
    hpf: clamp(20, 20000),
    resonance: clamp(0, 1),
    attack: clamp(0, 10),
    decay: clamp(0, 10),
    sustain: clamp(0, 1),
    release: clamp(0, 10),
    legato: clamp(0, 4),
    room: clamp(0, 1),
    delay: clamp(0, 1),
    crush: clamp(1, 16),
    cut: integer(0, 1e6),
    bank: soundName,
};

export const kAliases = { sound: "s", cutoff: "lpf" };

// One event value -> the control object for `field`.
function controlValue(field, v) {
    const check = kControls[field];
    // A value that is already a control object ("kick:3" gives { s, n }) passes through, its fields checked.
    if (v !== null && typeof v === "object" && !Array.isArray(v)) {
        const out = {};
        for (const [k, x] of Object.entries(v))
            out[k] = kControls[k] ? kControls[k](k, x) : x;
        return out;
    }
    if (field === "s" && typeof v === "string") {
        // "kick:3" written in a plain string (not parsed by mini-notation): the sample number too.
        const m = /^(.+):(\d+)$/.exec(v);
        if (m)
            return { s: m[1], n: Number(m[2]) };
    }
    return { [field]: check(field, v) };
}

// The control as a pattern: s("kick hat"), gain(sine), note([48, 52]) ...
export function control(field) {
    return (arg) => reify(arg).fmap((v) => controlValue(field, v));
}

// Two values merged: the right's fields over the left's. A plain left value becomes a control first: a string a
// sound (s), a number a sample / degree (n) - so mini("kick hat").gain(0.5) works.
export function mergeValues(left, right) {
    const base = asControls(left);
    return { ...base, ...right };
}

export function asControls(v) {
    if (v !== null && typeof v === "object" && !Array.isArray(v))
        return v;
    if (typeof v === "string")
        return controlValue("s", v);
    if (typeof v === "number")
        return { n: v };
    throw new ControlError(`Can't add controls to ${JSON.stringify(v)}`);
}

// The functions, and the same names as methods: s("kick").gain(0.5).speed("<1 2>").
export const controls = {};
for (const name of [...Object.keys(kControls), ...Object.keys(kAliases)]) {
    const field = kAliases[name] ?? name;
    const make = control(field);
    controls[name] = make;
    Pattern.prototype[name] = function (arg) {
        return this.fmap((left) => (right) => mergeValues(left, right)).appLeft(make(arg));
    };
}

export const { s, sound, n, note, gain, pan, speed, lpf, cutoff, hpf, resonance, attack, decay, sustain, release,
    legato, room, delay, crush, cut, bank } = controls;
