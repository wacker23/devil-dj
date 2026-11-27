// Time spans [begin, end) in cycles (CODEBEAT_SPEC §2). Immutable; begin and end are Fractions.

import { F, Fraction } from "./fraction.js";

// A Span from two Fractions already known to be in order (begin <= end) - the checks are not repeated for the
// spans queries make by the thousand (P5 performance pass).
function ordered(begin, end) {
    const s = Object.create(Span.prototype);
    s.begin = begin;
    s.end = end;
    return Object.freeze(s);
}

export class Span {
    constructor(begin, end) {
        this.begin = begin instanceof Fraction ? begin : F(begin);
        this.end = end instanceof Fraction ? end : F(end);
        if (this.end.lt(this.begin))
            throw new RangeError(`Span ends before it begins: [${this.begin}, ${this.end})`);
        Object.freeze(this);
    }

    duration() {
        return this.end.sub(this.begin);
    }

    isZeroWidth() {
        return this.begin.eq(this.end);
    }

    midpoint() {
        return this.begin.add(this.end.sub(this.begin).div(2));
    }

    equals(other) {
        return other instanceof Span && this.begin.eq(other.begin) && this.end.eq(other.end);
    }

    // Does the span contain time t (begin <= t < end)?
    contains(t) {
        const x = F(t);
        return this.begin.lte(x) && x.lt(this.end);
    }

    // Split at whole cycles: [1/2, 5/2) -> [1/2, 1), [1, 2), [2, 5/2). A zero-width span stays as it is.
    spanCycles() {
        if (this.isZeroWidth())
            return [this];
        let begin = this.begin;
        let next = begin.nextSam();
        if (this.end.lte(next)) // within one cycle (most queries): the span itself
            return [this];
        const out = [];
        while (begin.lt(this.end)) {
            const end = next.lt(this.end) ? next : this.end;
            out.push(ordered(begin, end));
            begin = end;
            next = begin.nextSam();
        }
        return out;
    }

    // The whole cycle the span begins in: [floor(begin), floor(begin) + 1).
    wholeCycle() {
        return ordered(this.begin.sam(), this.begin.nextSam());
    }

    // The span moved into cycle 0 (both ends shifted by the begin's cycle); used to look up per-cycle structure.
    cyclePos() {
        const sam = this.begin.sam();
        return new Span(this.begin.sub(sam), this.end.sub(sam));
    }

    // Both ends through f (a function Fraction -> Fraction, e.g. scaling for fast/slow).
    withTime(f) {
        return new Span(f(this.begin), f(this.end));
    }

    withEnd(f) {
        return new Span(this.begin, f(this.end));
    }

    // Applies f to the times relative to the cycle the span begins in, keeping that cycle: for cycle-local operators.
    // The ends come back in order, so f may be a reflection (t -> 1 - t, as rev uses).
    withCycle(f) {
        const sam = this.begin.sam();
        const a = sam.add(f(this.begin.sub(sam)));
        const b = sam.add(f(this.end.sub(sam)));
        return a.lte(b) ? new Span(a, b) : new Span(b, a);
    }

    // The overlap [max begin, min end) without a check (may be "negative": the caller decides).
    sect(other) {
        return { begin: this.begin.max(other.begin), end: this.end.min(other.end) };
    }

    // The overlap, or null when they do not overlap. Spans that only touch do not overlap, except that a
    // zero-width span at the begin of (or inside) the other one does: that is how a point in time is queried.
    intersection(other) {
        const begin = this.begin.max(other.begin), end = this.end.min(other.end);
        if (begin.gt(end))
            return null;
        if (begin.eq(end)) {
            // Touching ends ([0,1) and [1,2)): no overlap. A zero-width span counts when it lies within [b, e).
            const point = this.isZeroWidth() ? this : other.isZeroWidth() ? other : null;
            if (point === null)
                return null;
            const wide = point === this ? other : this;
            if (wide.isZeroWidth() ? !wide.begin.eq(point.begin) : !wide.contains(point.begin))
                return null;
            return ordered(begin, end);
        }
        return ordered(begin, end);
    }

    toString() {
        return `[${this.begin}, ${this.end})`;
    }
}

// Shorthand: S(0, "1/2").
export function S(begin, end) {
    return new Span(begin, end);
}

export { Fraction };
