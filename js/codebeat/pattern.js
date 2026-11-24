// Patterns (CODEBEAT_SPEC §2-3): a pattern is a function from a queried time span to the events active in it.
// Everything here is exact (Fractions); nothing keeps state between queries, so the same query always gives the
// same events.

import { F, Fraction } from "./fraction.js";
import { Span } from "./span.js";
import { Event, combineContexts } from "./event.js";

export class State {
    constructor(span, controls = {}) {
        this.span = span;
        this.controls = controls; // read-only values for the query (DJ hooks, seed) - used by later operators
        Object.freeze(this);
    }

    setSpan(span) {
        return new State(span, this.controls);
    }
}

// Strings given where a pattern is expected: plain atoms until the mini-notation parser (mini.js) installs itself.
let stringToPattern = (s) => pure(s);
export function setStringParser(parse) {
    stringToPattern = parse;
}

export function isPattern(x) {
    return x instanceof Pattern;
}

// Anything to a pattern: patterns stay, strings (and the editor's string literals) are parsed, other values are pure.
export function reify(x) {
    if (x instanceof Pattern)
        return x;
    if (typeof x === "string")
        return stringToPattern(x);
    if (x !== null && typeof x === "object" && x.isMiniString === true)
        return stringToPattern(x);
    return pure(x);
}

export class Pattern {
    constructor(query) {
        if (typeof query !== "function")
            throw new TypeError("A pattern is made from a query function");
        this.query = query;
    }

    // --- querying ---

    // The events over [begin, end) (any time-like values).
    queryArc(begin, end, controls = {}) {
        return this.query(new State(new Span(begin, end), controls));
    }

    // Only the events that start inside [begin, end) - what the scheduler plays (§2 onset rule).
    onsets(begin, end, controls = {}) {
        return this.queryArc(begin, end, controls).filter((e) => e.hasOnset());
    }

    // Splits each query at cycle boundaries, so the inner query never spans two cycles.
    splitQueries() {
        return new Pattern((state) => state.span.spanCycles().flatMap((span) => this.query(state.setSpan(span))));
    }

    // --- changing time and values ---

    withQuerySpan(f) {
        return new Pattern((state) => this.query(state.setSpan(f(state.span))));
    }

    withQueryTime(f) {
        return this.withQuerySpan((span) => span.withTime(f));
    }

    withEventSpan(f) {
        return new Pattern((state) => this.query(state).map((e) => e.withSpan(f)));
    }

    withEventTime(f) {
        return this.withEventSpan((span) => span.withTime(f));
    }

    // Time changed both ways: queries through `toInner`, the events' spans back through `toOuter`.
    withTime(toInner, toOuter) {
        return this.withQueryTime(toInner).withEventTime(toOuter);
    }

    withValue(f) {
        return new Pattern((state) => this.query(state).map((e) => e.withValue(f)));
    }

    fmap(f) {
        return this.withValue(f);
    }

    withEvents(f) {
        return new Pattern((state) => f(this.query(state)));
    }

    filterEvents(pred) {
        return new Pattern((state) => this.query(state).filter(pred));
    }

    filterValues(pred) {
        return this.filterEvents((e) => pred(e.value));
    }

    onsetsOnly() {
        return this.filterEvents((e) => e.hasOnset());
    }

    // --- applying a pattern of functions to a pattern of values ---

    // Structure from both: where events of the two overlap, the overlap (wholes: their intersection).
    appBoth(other) {
        const right = reify(other);
        return new Pattern((state) => {
            const fs = this.query(state);
            const xs = right.query(state);
            const out = [];
            for (const ef of fs)
                for (const ex of xs) {
                    const part = ef.part.intersection(ex.part);
                    if (part === null)
                        continue;
                    let whole = null;
                    if (ef.whole !== null && ex.whole !== null) {
                        whole = ef.whole.intersection(ex.whole);
                        if (whole === null)
                            continue;
                    }
                    out.push(new Event(whole, part, ef.value(ex.value), combineContexts(ef.context, ex.context)));
                }
            return out;
        });
    }

    // Structure from the left (this): each of its events takes the value(s) of the right pattern active during it.
    appLeft(other) {
        const right = reify(other);
        return new Pattern((state) => {
            const out = [];
            for (const ef of this.query(state))
                for (const ex of right.query(state.setSpan(ef.wholeOrPart()))) {
                    const part = ef.part.intersection(ex.part);
                    if (part !== null)
                        out.push(new Event(ef.whole, part, ef.value(ex.value), combineContexts(ef.context, ex.context)));
                }
            return out;
        });
    }

    // Structure from the right.
    appRight(other) {
        const right = reify(other);
        return new Pattern((state) => {
            const out = [];
            for (const ex of right.query(state))
                for (const ef of this.query(state.setSpan(ex.wholeOrPart()))) {
                    const part = ex.part.intersection(ef.part);
                    if (part !== null)
                        out.push(new Event(ex.whole, part, ef.value(ex.value), combineContexts(ef.context, ex.context)));
                }
            return out;
        });
    }

    // --- patterns of patterns ---

    // The outer pattern picks which inner pattern plays; the events and their structure come from the inner one,
    // cut to the outer event's part. Used for patterned arguments: fast("<1 2>").
    innerJoin() {
        return new Pattern((state) => {
            const out = [];
            for (const outer of this.query(state)) {
                const inner = reify(outer.value);
                for (const e of inner.query(state.setSpan(outer.part))) {
                    const part = e.part.intersection(outer.part);
                    if (part !== null)
                        out.push(new Event(e.whole, part, e.value, combineContexts(e.context, outer.context)));
                }
            }
            return out;
        });
    }

    // Structure from the outer pattern: each outer event keeps its own whole and part and takes the value(s) of the
    // inner pattern at the moment its whole begins.
    outerJoin() {
        return new Pattern((state) => {
            const out = [];
            for (const outer of this.query(state)) {
                const inner = reify(outer.value);
                const at = outer.wholeOrPart().begin;
                for (const e of inner.query(state.setSpan(new Span(at, at))))
                    out.push(new Event(outer.whole, outer.part, e.value, combineContexts(e.context, outer.context)));
            }
            return out;
        });
    }

    // --- time (each argument may be patterned) ---

    fast(factor) {
        return patterned(factor, (f) => this._fast(f));
    }

    _fast(factor) {
        const f = F(factor);
        if (f.n === 0)
            return silence;
        if (f.n < 0)
            return this._fast(f.neg()).rev();
        return this.withTime((t) => t.mul(f), (t) => t.div(f));
    }

    slow(factor) {
        return patterned(factor, (f) => {
            const x = F(f);
            return x.n === 0 ? silence : this._fast(F(1).div(x));
        });
    }

    // Shifts the pattern earlier by t cycles (what was at t now plays at 0).
    early(offset) {
        return patterned(offset, (t) => this._early(t));
    }

    _early(offset) {
        const t = F(offset);
        return this.withTime((x) => x.add(t), (x) => x.sub(t));
    }

    late(offset) {
        return patterned(offset, (t) => this._early(F(t).neg()));
    }

    // Reverses each cycle.
    rev() {
        const pat = this;
        return new Pattern((state) => {
            const span = state.span;
            const cycle = span.begin.sam();
            const nextCycle = span.begin.nextSam();
            // t in this cycle -> its mirror image: cycle + (nextCycle - t).
            const reflect = (t) => cycle.add(nextCycle.sub(t));
            const reflectSpan = (s) => new Span(reflect(s.end), reflect(s.begin));
            return pat
                .query(state.setSpan(reflectSpan(span)))
                .map((e) => e.withSpan(reflectSpan));
        }).splitQueries();
    }

    // --- shorthands for the constructors ---

    stack(...others) {
        return stack(this, ...others);
    }

    seq(...others) {
        return fastcat(this, ...others);
    }

    cat(...others) {
        return slowcat(this, ...others);
    }

    // --- inspection (tests, the console) ---

    // The events of one cycle (or [begin, end)) as text, in time order then value.
    show(begin = 0, end = undefined) {
        const b = F(begin);
        const e = end === undefined ? b.add(1) : F(end);
        return sortEvents(this.queryArc(b, e)).map((ev) => ev.show());
    }
}

// Sorted by part begin, then whole begin, then value text (stable for tests).
export function sortEvents(events) {
    return [...events].sort((a, b) => {
        const c = a.part.begin.compare(b.part.begin);
        if (c !== 0)
            return c;
        const wa = a.whole ?? a.part, wb = b.whole ?? b.part;
        const d = wa.begin.compare(wb.begin);
        if (d !== 0)
            return d;
        const sa = a.show(), sb = b.show();
        return sa < sb ? -1 : sa > sb ? 1 : 0;
    });
}

// A numeric argument that may be a pattern or mini-notation: plain values go straight to `make`; patterns become
// a pattern of results joined so the inner (made) pattern keeps its own structure (§3 "patterned arguments").
export function patterned(arg, make) {
    if (arg instanceof Pattern || typeof arg === "string" || (arg !== null && typeof arg === "object" && arg.isMiniString === true))
        return reify(arg).fmap((v) => make(v)).innerJoin();
    return make(arg);
}

// --- constructors ---

// The value once per cycle.
export function pure(value) {
    return new Pattern((state) =>
        state.span.spanCycles().map((part) => new Event(part.wholeCycle(), part, value)),
    );
}

export const silence = new Pattern(() => []);

// The patterns stacked: all at once.
export function stack(...items) {
    const pats = items.flat().map(reify);
    return new Pattern((state) => pats.flatMap((p) => p.query(state)));
}

// One pattern per cycle, in turn. Each pattern sees its own cycles one after another: with cat(a, b) the second
// cycle of a plays in cycle 2.
export function slowcat(...items) {
    const pats = items.flat().map(reify);
    if (pats.length === 0)
        return silence;
    const n = pats.length;
    return new Pattern((state) => {
        const cycle = state.span.begin.floor();
        const index = ((cycle % n) + n) % n;
        const ownCycle = Math.floor(cycle / n);
        const shift = new Fraction(cycle - ownCycle); // pattern time = outer time - shift
        return pats[index]
            .query(state.setSpan(state.span.withTime((t) => t.sub(shift))))
            .map((e) => e.withSpan((s) => s.withTime((t) => t.add(shift))));
    }).splitQueries();
}

// The patterns squeezed into one cycle, each taking an equal share.
export function fastcat(...items) {
    const pats = items.flat();
    if (pats.length === 0)
        return silence;
    return slowcat(...pats)._fast(pats.length);
}

export const seq = fastcat;
export const cat = slowcat;

// Lifts a plain function of values to patterns, with structure from the left (the usual case for arithmetic).
export function lift2(f) {
    return (a, b) => reify(a).fmap((x) => (y) => f(x, y)).appLeft(reify(b));
}

export { Event, Span, F, Fraction };
