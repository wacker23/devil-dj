// Signals (CODEBEAT_SPEC §5.5): continuous patterns with no `whole` - a value at every moment, in [0, 1) over one
// cycle. A query gets one event whose part is the queried span, valued at the span's begin. So when a signal is
// used as a control (s("hat*8").gain(sine)), each event takes the signal's value at its own start.
//
//   sine.range(0.4, 1)    scaled to [lo, hi)
//   saw.segment(4)        sampled 4 times per cycle -> discrete events with wholes (0, 1/4, 1/2, 3/4)
//
// rand / irand / perlin are seeded (the pattern's seed, 0 by default; .seed(n) changes it): the same seed always
// gives the same values.

import { F } from "./fraction.js";
import { Event } from "./event.js";
import { Pattern, pure, reify, patterned } from "./pattern.js";
import { randomAt, hashString } from "./random.js";
import "./transforms.js"; // seed(), and the rest of the methods signals are used with

// A signal from f(t, state), t the begin of the query in cycles (a Fraction).
export function signal(f) {
    return new Pattern((state) => [new Event(null, state.span, f(state.span.begin, state))]);
}

const TAU = 2 * Math.PI;
const pos = (t) => t.cyclePos().toNumber();

export const saw = signal((t) => pos(t));
export const isaw = signal((t) => 1 - pos(t)); // 1 at the cycle start, falling
export const sine = signal((t) => (Math.sin(TAU * pos(t)) + 1) / 2);
export const cosine = signal((t) => (Math.cos(TAU * pos(t)) + 1) / 2);
export const tri = signal((t) => {
    const x = pos(t);
    return x < 0.5 ? 2 * x : 2 - 2 * x;
});
export const square = signal((t) => (pos(t) < 0.5 ? 0 : 1));

const kRandSalt = hashString("rand");
const kPerlinSalt = hashString("perlin");

// White noise: a new value at every moment (exact times, so the same time is always the same value).
export const rand = signal((t, state) => randomAt(state.controls.seed ?? 0, kRandSalt, t));

// Whole random numbers 0 .. n-1.
export function irand(n) {
    return patterned(n, (k) => {
        const nn = Number(k);
        if (!Number.isInteger(nn) || nn <= 0)
            throw new RangeError(`irand needs a whole number of 1 or more, got ${k}`);
        return rand.fmap((v) => Math.floor(v * nn));
    });
}

// Smooth noise: a random value at each whole cycle, eased (smoothstep) between neighbours.
export const perlin = signal((t, state) => {
    const seed = state.controls.seed ?? 0;
    const c = t.floor();
    const a = randomAt(seed, kPerlinSalt, c);
    const b = randomAt(seed, kPerlinSalt, c + 1);
    const x = pos(t);
    const ease = x * x * (3 - 2 * x);
    return a + (b - a) * ease;
});

const P = Pattern.prototype;

// Values (0..1) scaled to [lo, hi); lo and hi may be patterns (structure from this one).
P.range = function (lo, hi) {
    return this.fmap((v) => (l) => (h) => {
        const x = Number(v), a = Number(l), b = Number(h);
        if (!Number.isFinite(x) || !Number.isFinite(a) || !Number.isFinite(b))
            throw new RangeError(`range needs numbers, got ${v} in ${l}..${h}`);
        return a + x * (b - a);
    })
        .appLeft(reify(lo))
        .appLeft(reify(hi));
};

// Sampled n times per cycle: discrete events (with wholes) holding the value at the start of each segment.
P.segment = function (n) {
    return patterned(n, (k) => {
        const f = F(k);
        if (f.n <= 0)
            throw new RangeError(`segment needs a number above 0, got ${k}`);
        return pure((v) => v)._fast(f).appLeft(this);
    });
};

export const kSignals = { sine, cosine, saw, isaw, tri, square, rand, perlin };
