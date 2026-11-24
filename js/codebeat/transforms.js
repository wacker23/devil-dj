// Transformations, tier 1 (CODEBEAT_SPEC §5.2-5.4): structure, randomness and arithmetic on patterns. All are
// methods on Pattern; numeric arguments may be patterns or mini-notation (`patterned`).

import { F, Fraction } from "./fraction.js";
import { Span } from "./span.js";
import { Event, combineContexts } from "./event.js";
import { Pattern, State, pure, silence, stack, fastcat, reify, patterned } from "./pattern.js";
import { randomAt, hashString } from "./random.js";
import { euclidSteps } from "./euclid.js";
import { asControls, ControlError } from "./controls.js";
import { parseScale, degreeToNote } from "./scales.js";

const P = Pattern.prototype;
const mod = (a, n) => ((a % n) + n) % n;

function toInt(name, v) {
    const x = Number(v);
    if (!Number.isInteger(x))
        throw new RangeError(`${name} needs a whole number, got ${v}`);
    return x;
}

function toChance(name, v) {
    const x = Number(v);
    if (!(x >= 0 && x <= 1))
        throw new RangeError(`${name} needs a chance between 0 and 1, got ${v}`);
    return x;
}

// --- by cycle ---

// For each cycle c, the pattern make(c) over that cycle (no time shift).
function byCycle(make) {
    return new Pattern((state) => make(state.span.begin.floor()).query(state)).splitQueries();
}

// fn applied in the cycles where predicate(cycle) holds.
P.whenCycle = function (predicate, fn) {
    const changed = fn(this);
    return byCycle((c) => (predicate(c) ? changed : this));
};

// fn applied every n cycles: on cycles 0, n, 2n ... (cycle mod n === 0).
P.every = function (n, fn) {
    return patterned(n, (k) => {
        const nn = toInt("every", k);
        if (nn <= 0)
            return this;
        return this.whenCycle((c) => mod(c, nn) === 0, fn);
    });
};

// Each cycle starts one 1/n further in: cycle 1 starts at 1/n, ... (wrapping every n cycles).
P.iter = function (n) {
    return patterned(n, (k) => {
        const nn = toInt("iter", k);
        if (nn <= 0)
            return this;
        return byCycle((c) => this._early(new Fraction(mod(c, nn), nn)));
    });
};

// Forwards, then backwards: odd cycles reversed.
P.palindrome = function () {
    const back = this.rev();
    return byCycle((c) => (mod(c, 2) === 1 ? back : this));
};

// --- layering ---

P.superimpose = function (fn) {
    return stack(this, fn(this));
};

P.layer = function (...fns) {
    return stack(...fns.flat().map((f) => f(this)));
};

// A copy shifted later by t and changed by fn, on top of the original.
P.off = function (t, fn) {
    return stack(this, fn(this.late(t)));
};

// --- squeezing ---

// Each event of the outer pattern holds a pattern; that pattern's cycle 0... is squeezed into the event's whole.
P.squeezeJoin = function () {
    return new Pattern((state) => {
        const out = [];
        for (const outer of this.query(state)) {
            if (outer.whole === null)
                continue;
            const w = outer.whole;
            const width = w.duration();
            if (width.n === 0)
                continue;
            const toInner = (t) => t.sub(w.begin).div(width);
            const toOuter = (t) => w.begin.add(t.mul(width));
            const inner = reify(outer.value);
            for (const e of inner.query(state.setSpan(outer.part.withTime(toInner)))) {
                const mapped = e.withSpan((s) => s.withTime(toOuter));
                const part = mapped.part.intersection(outer.part);
                if (part !== null)
                    out.push(new Event(mapped.whole, part, mapped.value, combineContexts(mapped.context, outer.context)));
            }
        }
        return out;
    });
};

// Each event played n times in its own time.
P.ply = function (n) {
    return patterned(n, (k) => {
        const f = F(k);
        if (f.n <= 0)
            return silence;
        return this.fmap((v) => pure(v)._fast(f)).squeezeJoin();
    });
};

// --- randomness (seeded: same code, same seed -> same result) ---

// Events chosen by the hash at their start: `salt` keeps different calls apart.
function chosen(e, state, salt, p) {
    return randomAt(state.controls.seed ?? 0, salt, e.wholeOrPart().begin) < p;
}

// Drops events with chance p.
P.degradeBy = function (p) {
    return patterned(p, (x) => {
        const chance = toChance("degradeBy", x);
        const salt = hashString(`degradeBy:${chance}`);
        return new Pattern((state) => this.query(state).filter((e) => !chosen(e, state, salt, chance)));
    });
};

P.degrade = function () {
    return this.degradeBy(0.5);
};

// fn applied to events chosen with chance p, the rest as they were.
P.sometimesBy = function (p, fn) {
    return patterned(p, (x) => {
        const chance = toChance("sometimesBy", x);
        const salt = hashString(`sometimesBy:${chance}:${String(fn)}`);
        const changed = fn(this);
        return new Pattern((state) => [
            ...this.query(state).filter((e) => !chosen(e, state, salt, chance)),
            ...changed.query(state).filter((e) => chosen(e, state, salt, chance)),
        ]);
    });
};

P.sometimes = function (fn) {
    return this.sometimesBy(0.5, fn);
};

P.often = function (fn) {
    return this.sometimesBy(0.75, fn);
};

P.rarely = function (fn) {
    return this.sometimesBy(0.25, fn);
};

// The random choices of this pattern made with another seed.
P.seed = function (n) {
    const value = toInt("seed", n);
    return new Pattern((state) => this.query(new State(state.span, { ...state.controls, seed: value })));
};

// --- structure ---

const kFalse = new Set([false, 0, "0", "f", "false", "~", null, undefined]);
const truthy = (v) => !kFalse.has(v);

// The rhythm of a boolean pattern ("x ~ x x", "1 0 1 1", "t f"), the values of this one.
P.struct = function (pattern) {
    const shape = reify(pattern).filterValues(truthy);
    return shape.fmap(() => (v) => v).appLeft(this);
};

// This pattern's events, only where the boolean pattern is true (cut to its true parts).
P.mask = function (pattern) {
    const m = reify(pattern);
    return this.fmap((v) => (b) => ({ v, b }))
        .appLeft(m)
        .filterValues(({ b }) => truthy(b))
        .fmap(({ v }) => v);
};

// The pattern played on a euclidean rhythm of k pulses over n steps, rotated left by r.
P.euclid = function (k, n, r = 0) {
    return patterned(k, (kk) =>
        patterned(n, (nn) =>
            patterned(r, (rr) => {
                const steps = euclidSteps(toInt("euclid pulses", kk), toInt("euclid steps", nn), toInt("euclid rotation", rr));
                return this.struct(fastcat(...steps.map((on) => (on ? pure(true) : silence))));
            }),
        ),
    );
};

// --- arithmetic on values ---

// A number added to a value: a plain number directly; a control object on its pitch (note, else n); an object
// onto its matching fields ({ note: 12 } adds to note only).
function arith(name, op) {
    const apply = (left, right) => {
        if (typeof left === "number" && typeof right === "number")
            return op(left, right);
        if (typeof left === "string" && Number.isFinite(Number(left)) && typeof right === "number")
            return op(Number(left), right);
        const l = asControls(left);
        if (typeof right === "number") {
            const field = "note" in l ? "note" : "n" in l ? "n" : null;
            if (field === null)
                throw new ControlError(`${name}: nothing to ${name} ${right} to (the events have no note or n)`);
            return { ...l, [field]: op(l[field], right) };
        }
        if (right !== null && typeof right === "object") {
            const out = { ...l };
            for (const [k, x] of Object.entries(right))
                out[k] = typeof x === "number" ? op(l[k] ?? 0, x) : x;
            return out;
        }
        throw new ControlError(`${name} needs numbers, got ${JSON.stringify(right)}`);
    };
    return function (other) {
        return this.fmap((l) => (r) => apply(l, r)).appLeft(reify(other));
    };
}

P.add = arith("add", (a, b) => a + b);
P.sub = arith("sub", (a, b) => a - b);
P.mul = arith("mul", (a, b) => a * b);
P.div = arith("div", (a, b) => {
    if (b === 0)
        throw new ControlError("div by zero");
    return a / b;
});

// Scale degrees (n, or plain numbers) to notes in a scale: n("0 2 4").scale("C:minor") -> note 48, 51, 55.
P.scale = function (name) {
    return this.fmap((v) => (scaleName) => {
        const sc = parseScale(scaleName);
        if (typeof v === "number")
            return { note: degreeToNote(sc, v) };
        const c = asControls(v);
        if (typeof c.n !== "number")
            throw new ControlError(`scale needs scale degrees in n, e.g. n("0 2 4").scale("${scaleName}")`);
        const { n, ...rest } = c;
        return { ...rest, note: degreeToNote(sc, n) };
    }).appLeft(reify(name));
};

// Faster, and pitched up as much: fast(f) with speed multiplied by f.
P.hurry = function (f) {
    return patterned(f, (x) => {
        const factor = F(x);
        return this._fast(factor).fmap((v) => {
            const c = asControls(v);
            return { ...c, speed: (c.speed ?? 1) * factor.toNumber() };
        });
    });
};

// --- constructors ---

// 0 1 2 ... n-1 in one cycle.
export function run(n) {
    return patterned(n, (k) => {
        const nn = toInt("run", k);
        return nn <= 0 ? silence : fastcat(...Array.from({ length: nn }, (_, i) => i));
    });
}

// The euclidean rhythm as a boolean pattern: true on the pulses.
export function euclid(k, n, r = 0) {
    return pure(true).euclid(k, n, r);
}

export { Span };
