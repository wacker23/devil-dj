// Events (CODEBEAT_SPEC §2): a value over a time span.
//   whole   - the event's full span (null for continuous signals); may reach outside the query
//   part    - the piece of whole inside the queried span
//   value   - a primitive, a control object ({ s: "kick", gain: 0.8 }) or, inside operators, a function / pattern
//   context - { locations: [{ start, end }] }: source ranges of the atoms that made it (for highlighting)

import { Span } from "./span.js";

const kEmptyContext = Object.freeze({ locations: Object.freeze([]) });

export function combineContexts(a, b) {
    if (!b || b.locations.length === 0)
        return a ?? kEmptyContext;
    if (!a || a.locations.length === 0)
        return b;
    return Object.freeze({ locations: Object.freeze([...a.locations, ...b.locations]) });
}

export class Event {
    constructor(whole, part, value, context = kEmptyContext) {
        if (!(part instanceof Span))
            throw new TypeError("An event needs a part span");
        if (whole !== null && !(whole instanceof Span))
            throw new TypeError("An event's whole is a span or null");
        this.whole = whole;
        this.part = part;
        this.value = value;
        this.context = context ?? kEmptyContext;
        Object.freeze(this);
    }

    // Onset rule (§2): a sound starts only where the whole begins inside the query.
    hasOnset() {
        return this.whole !== null && this.whole.begin.eq(this.part.begin);
    }

    hasWhole() {
        return this.whole !== null;
    }

    // The whole, or for a signal the part.
    wholeOrPart() {
        return this.whole ?? this.part;
    }

    withSpan(f) {
        return new Event(this.whole === null ? null : f(this.whole), f(this.part), this.value, this.context);
    }

    withValue(f) {
        return new Event(this.whole, this.part, f(this.value), this.context);
    }

    withContext(context) {
        return new Event(this.whole, this.part, this.value, context);
    }

    // "a [0, 1/2)"; a fragment shows its part too: "a [0, 2) part [1, 2)"; a signal: "a ~[0, 1/4)".
    show() {
        const v = showValue(this.value);
        if (this.whole === null)
            return `${v} ~${this.part}`;
        if (this.whole.equals(this.part))
            return `${v} ${this.whole}`;
        return `${v} ${this.whole} part ${this.part}`;
    }

    toString() {
        return this.show();
    }
}

export function showValue(v) {
    if (v === null || v === undefined)
        return String(v);
    if (typeof v === "object" && !Array.isArray(v) && Object.getPrototypeOf(v) === Object.prototype)
        return Object.entries(v)
            .map(([k, x]) => `${k}:${showValue(x)}`)
            .join(" ");
    if (Array.isArray(v))
        return `(${v.map(showValue).join(" ")})`;
    return String(v);
}

export { kEmptyContext };
