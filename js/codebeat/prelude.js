// The Code Beat prelude (CODEBEAT_SPEC §5.7, P5-08): puts the whole language on globalThis for the user's code,
// and keeps the session - what is playing, named slots, mutes, the DJ hooks - between evaluations.
//
// The C++ side (codebeat::CodeEvaluator) runs one evaluation as:
//   __beginEval()  ->  the user's script  (its completion value stored as __completion)  ->  __endEval()
// or __abortEval() when the script failed. __endEval works out the new pattern, test-queries it, and only then makes
// it the playing one: code with an error never replaces what is playing.

import { F, Fraction } from "./fraction.js";
import { Pattern, pure, silence, stack, seq, fastcat, cat, slowcat, timecat, reify, sortEvents } from "./pattern.js";
import { mini, MiniError } from "./mini.js";
import { controls, noteNameToMidi, ControlError, asControls } from "./controls.js";
import { run, euclid } from "./transforms.js";
import { signal, sine, cosine, saw, isaw, tri, square, rand, irand, perlin } from "./signals.js";
import { kScales } from "./scales.js";

// Errors keep enough of their stack to reach the user's line: mini-notation parsing alone is more than the default
// 10 frames deep.
Error.stackTraceLimit = 64;

// --- the session (lives across evaluations) ---

const kMaxBpm = 300, kMinBpm = 20;

function emptyDeck() {
    return Object.freeze({ bpm: 0, key: -1, keyName: "", beat: 0, bar: 0, phrase: 0, playing: false });
}

const session = {
    slots: new Map(),     // name -> pattern (unnamed play() calls get "$0", "$1" ...)
    muted: new Set(),
    active: silence,      // what plays now
    next: null,           // P5-10: what takes over at the next cycle line (null: nothing waiting)
    hasPattern: false,    // something was ever given
    cpb: 4,               // beats per cycle
    dj: { decks: [emptyDeck(), emptyDeck(), emptyDeck(), emptyDeck()], master: Object.freeze({ bpm: 120, beat: 0 }) },
};

let pending = null; // during an evaluation: what the code asked for

function inEval(what) {
    if (pending === null)
        throw new Error(`${what}() only works in the code being run`);
    return pending;
}

// --- program functions ---

function play(a, b) {
    const p = inEval("play");
    const [name, value] = b === undefined ? [`$${p.unnamed++}`, a] : [String(a), b];
    if (!(value instanceof Pattern) && typeof value !== "string")
        throw new TypeError(`play() needs a pattern, got ${value === undefined ? "nothing" : typeof value}`);
    const pat = reify(value);
    (p.slots ??= new Map()).set(name, pat);
    return pat;
}

function mute(name) {
    inEval("mute").mutes.push([String(name), true]);
}

function unmute(name) {
    inEval("unmute").mutes.push([String(name), false]);
}

// Silence: everything stops (play() after it in the same code still plays).
function hush() {
    const p = inEval("hush");
    p.slots = new Map();
    p.unnamed = 0;
    p.hushed = true; // silence is never kept waiting for the cycle line
}

function setbpm(bpm) {
    const x = Number(bpm);
    if (!(x >= kMinBpm && x <= kMaxBpm))
        throw new RangeError(`setbpm needs ${kMinBpm} to ${kMaxBpm} BPM, got ${bpm}`);
    inEval("setbpm").bpm = x;
}

// Beats per cycle (4 = one cycle is a bar of 4/4).
function setcpb(beats) {
    const x = Number(beats);
    if (!(x > 0 && x <= 16))
        throw new RangeError(`setcpb needs 1 to 16 beats per cycle, got ${beats}`);
    inEval("setcpb").cpb = x;
}

// --- the API on globalThis (plain, configurable properties: user code may shadow them with its own names) ---

const api = {
    pure, silence, seq, fastcat, cat, slowcat, stack, timecat, mini, run, euclid,
    signal, sine, cosine, saw, isaw, tri, square, rand, irand, perlin,
    ...controls,
    play, mute, unmute, hush, setbpm, setcpb,
    noteToMidi: noteNameToMidi,
    scales: Object.freeze(Object.keys(kScales)),
    Fraction: F,
};
for (const [name, value] of Object.entries(api))
    globalThis[name] = value;

// DJ hooks: read-only views of the decks and the master clock (refreshed by the C++ side before every evaluation
// and query).
for (let i = 0; i < 4; ++i)
    Object.defineProperty(globalThis, `deck${"ABCD"[i]}`, { get: () => session.dj.decks[i], configurable: true });
Object.defineProperty(globalThis, "master", { get: () => session.dj.master, configurable: true });

// --- the C++ side's entry points ---

globalThis.__beginEval = () => {
    pending = { slots: null, mutes: [], bpm: null, cpb: null, unnamed: 0, hushed: false };
    globalThis.__completion = undefined;
    return 0;
};

globalThis.__abortEval = () => {
    pending = null;
    globalThis.__completion = undefined;
    return 0;
};

// The result as "key=value" lines: playing, changed, swap, slots, muted, bpm, cpb, message.
// P5-10: the new pattern takes over at the next cycle line (swap=cycle) - or at once with `now` (Ctrl+Shift+Enter),
// when the code hushed, or for the very first pattern (swap=now). A pattern still waiting is replaced by the newer one.
globalThis.__endEval = (now = 0) => {
    const p = inEval("__endEval");
    const completion = globalThis.__completion;
    globalThis.__completion = undefined;
    const muted = new Set(session.muted);
    for (const [name, on] of p.mutes)
        on ? muted.add(name) : muted.delete(name);

    let slots = session.slots;
    let changed = true;
    let message = "";
    if (p.slots !== null)
        slots = p.slots;
    else if (completion instanceof Pattern || typeof completion === "string")
        slots = new Map([["$0", reify(completion)]]);
    else if (p.mutes.length > 0)
        message = "mutes changed";
    else {
        changed = false;
        message = session.hasPattern ? "no pattern in this code: the last one keeps playing"
            : "nothing to play yet: end the code with a pattern, or call play(...)";
    }
    const active = stack(...[...slots].filter(([name]) => !muted.has(name)).map(([, pat]) => pat));

    // Test-query the first two cycles so errors that only show when the pattern runs (a bad control value, a broken
    // function) are reported now, while the old pattern still plays.
    active.queryArc(0, 2);

    pending = null;
    session.slots = slots;
    session.muted = muted;
    const swapNow = Boolean(now) || p.hushed || !session.hasPattern;
    if (changed) {
        if (swapNow) {
            session.active = active;
            session.next = null;
        } else
            session.next = active;
    }
    session.hasPattern = session.hasPattern || slots.size > 0;
    if (p.cpb !== null)
        session.cpb = p.cpb;
    const playing = [...slots.keys()].some((name) => !muted.has(name));
    return [
        `playing=${playing ? 1 : 0}`,
        `changed=${changed ? 1 : 0}`,
        `swap=${!changed ? "" : swapNow ? "now" : "cycle"}`,
        `slots=${[...slots.keys()].join(",")}`,
        `muted=${[...muted].join(",")}`,
        `bpm=${p.bpm ?? ""}`,
        `cpb=${p.cpb ?? ""}`,
        `message=${message}`,
    ].join("\n");
};

// The DJ state: { decks: [{ bpm, key, keyName, beat, bar, phrase, playing } x4], master: { bpm, beat } }.
globalThis.__setDj = (state) => {
    const decks = [0, 1, 2, 3].map((i) => Object.freeze({ ...emptyDeck(), ...(state?.decks?.[i] ?? {}) }));
    session.dj = { decks, master: Object.freeze({ bpm: 120, beat: 0, ...(state?.master ?? {}) }) };
    return 0;
};

// The newest pattern (the one waiting for the cycle line, else the one playing): its events over [begin, end), one
// per line (tests, the console).
globalThis.__show = (begin, end) =>
    sortEvents((session.next ?? session.active).queryArc(F(begin), F(end))).map((e) => e.show()).join("\n");

// The onsets in [bn/bd, en/ed) cycles for the scheduler (P5-09), one per line:
//   "<whole begin n>/<d> <whole end n>/<d>|key=value;key=value|start-end,start-end"
// (the controls; then the source locations of the atoms, for highlighting). Plain values become controls (a word
// a sound, a number n); characters that would break the format are replaced in strings.
globalThis.__query = (bn, bd, en, ed) => {
    const begin = new Fraction(bn, bd), end = new Fraction(en, ed);
    let events;
    // A pattern waiting for the cycle line: the old one up to the line, the new one from it (P5-10). The window
    // pieces join exactly, so no onset is played twice or missed; the new pattern's events that began before the
    // line (fragments) have no onset here and stay silent.
    const line = begin.ceil();
    if (session.next !== null && end.gt(line)) {
        const old = begin.lt(line) ? session.active.queryArc(begin, new Fraction(line)) : [];
        session.active = session.next;
        session.next = null;
        events = [...old, ...session.active.queryArc(new Fraction(line), end)];
    } else
        events = session.active.queryArc(begin, end);
    const lines = [];
    for (const e of events) {
        if (!e.hasOnset())
            continue;
        let value;
        try {
            value = asControls(e.value);
        } catch {
            continue; // a value that can't be played (e.g. a boolean) makes no sound
        }
        const fields = [];
        for (const [k, v] of Object.entries(value)) {
            if (typeof v === "number" && Number.isFinite(v))
                fields.push(`${k}=${v}`);
            else if (typeof v === "string")
                fields.push(`${k}=${v.replace(/[;|=\n]/g, "_")}`);
        }
        const locs = e.context.locations.map((l) => `${l.start}-${l.end}`).join(",");
        lines.push(`${e.whole.begin.n}/${e.whole.begin.d} ${e.whole.end.n}/${e.whole.end.d}|${fields.join(";")}|${locs}`);
    }
    return lines.join("\n");
};

globalThis.__session = session; // read-only use by the scheduler bridge (P5-09) and tests

export { session, MiniError, ControlError };
