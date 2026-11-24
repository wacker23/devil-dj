// Exact rationals for pattern time (CODEBEAT_SPEC §2). Numerator and denominator are plain JS numbers that must stay
// safe integers; every result is checked, and an operation that would lose precision throws a RangeError instead
// of silently drifting. No BigInt: QuickJS has it, but plain numbers are faster and pattern times stay small.
//
// Fractions are immutable and always normalised: the denominator is positive and gcd(n, d) = 1, so equal values
// have equal fields.

function gcd(a, b) {
    a = Math.abs(a);
    b = Math.abs(b);
    while (b !== 0) {
        const t = a % b;
        a = b;
        b = t;
    }
    return a;
}

function checked(x, what) {
    if (!Number.isSafeInteger(x))
        throw new RangeError(`Fraction overflow in ${what} (the result is too large to be exact)`);
    return x;
}

export class Fraction {
    constructor(n, d = 1) {
        if (!Number.isSafeInteger(n) || !Number.isSafeInteger(d))
            throw new TypeError(`Fraction parts must be safe integers, got ${n}/${d}`);
        if (d === 0)
            throw new RangeError("Fraction with a zero denominator");
        if (d < 0) {
            n = -n;
            d = -d;
        }
        const g = gcd(n, d) || 1;
        this.n = n / g + 0; // + 0 turns -0 into 0
        this.d = d / g;
        Object.freeze(this);
    }

    // --- arithmetic (each argument may be anything F() accepts) ---

    add(other) {
        const o = F(other);
        const g = gcd(this.d, o.d);
        const a = checked(this.n * (o.d / g), "add");
        const b = checked(o.n * (this.d / g), "add");
        return new Fraction(checked(a + b, "add"), checked((this.d / g) * o.d, "add"));
    }

    sub(other) {
        return this.add(F(other).neg());
    }

    mul(other) {
        const o = F(other);
        // Cross-reduce first so the products stay small.
        const g1 = gcd(this.n, o.d) || 1;
        const g2 = gcd(o.n, this.d) || 1;
        return new Fraction(checked((this.n / g1) * (o.n / g2), "mul"), checked((this.d / g2) * (o.d / g1), "mul"));
    }

    div(other) {
        const o = F(other);
        if (o.n === 0)
            throw new RangeError("Fraction division by zero");
        return this.mul(new Fraction(o.d, o.n));
    }

    neg() {
        return new Fraction(-this.n, this.d);
    }

    abs() {
        return this.n < 0 ? this.neg() : this;
    }

    // --- comparison ---

    compare(other) {
        const o = F(other);
        if (this.d === o.d)
            return Math.sign(this.n - o.n);
        const g = gcd(this.d, o.d);
        const a = this.n * (o.d / g);
        const b = o.n * (this.d / g);
        if (Number.isSafeInteger(a) && Number.isSafeInteger(b))
            return Math.sign(a - b);
        // Too large to cross-multiply exactly: compare the integer parts, then the remainders.
        const fa = this.floor(), fb = o.floor();
        if (fa !== fb)
            return Math.sign(fa - fb);
        return this.sub(fa).compare(o.sub(fb));
    }

    eq(other) {
        const o = F(other);
        return this.n === o.n && this.d === o.d;
    }

    lt(other) {
        return this.compare(other) < 0;
    }

    lte(other) {
        return this.compare(other) <= 0;
    }

    gt(other) {
        return this.compare(other) > 0;
    }

    gte(other) {
        return this.compare(other) >= 0;
    }

    min(other) {
        const o = F(other);
        return this.lte(o) ? this : o;
    }

    max(other) {
        const o = F(other);
        return this.gte(o) ? this : o;
    }

    // --- cycles ---

    floor() {
        // Exact (no float division): n = q * d + r with the remainder taken towards negative infinity.
        const r = this.n % this.d;
        return (this.n - r) / this.d - (r < 0 ? 1 : 0);
    }

    ceil() {
        return this.d === 1 ? this.n : this.floor() + 1;
    }

    // The start of the cycle this time is in, as a Fraction ("sam").
    sam() {
        return new Fraction(this.floor());
    }

    // The start of the next cycle.
    nextSam() {
        return new Fraction(this.floor() + 1);
    }

    // The position inside the cycle, 0 <= x < 1.
    cyclePos() {
        return this.sub(this.floor());
    }

    isInteger() {
        return this.d === 1;
    }

    // --- conversion ---

    valueOf() {
        return this.n / this.d;
    }

    toNumber() {
        return this.n / this.d;
    }

    toString() {
        return this.d === 1 ? `${this.n}` : `${this.n}/${this.d}`;
    }

    toJSON() {
        return this.toString();
    }

    // The closest fraction to a float with a denominator up to maxDenominator (continued fractions). Used for
    // decimals typed by the user ("0.25", 1.5) and for turning clock positions into cycles.
    static fromNumber(x, maxDenominator = 1e6) {
        if (!Number.isFinite(x))
            throw new RangeError(`Not a finite number: ${x}`);
        if (Number.isInteger(x))
            return new Fraction(checked(x, "fromNumber"));
        const sign = x < 0 ? -1 : 1;
        let v = Math.abs(x);
        // Convergents h/k of the continued fraction of v.
        let h0 = 0, h1 = 1, k0 = 1, k1 = 0;
        let rest = v;
        for (let i = 0; i < 64; ++i) {
            const a = Math.floor(rest);
            const h2 = a * h1 + h0;
            const k2 = a * k1 + k0;
            if (k2 > maxDenominator) {
                // The best semiconvergent within the limit, if it beats the last convergent.
                const t = Math.floor((maxDenominator - k0) / k1);
                const hs = t * h1 + h0, ks = t * k1 + k0;
                if (ks > 0 && Math.abs(v - hs / ks) < Math.abs(v - h1 / k1))
                    return new Fraction(sign * checked(hs, "fromNumber"), ks);
                break;
            }
            h0 = h1; h1 = h2;
            k0 = k1; k1 = k2;
            const frac = rest - a;
            if (frac < 1e-12 || Math.abs(v - h1 / k1) === 0)
                break;
            rest = 1 / frac;
        }
        return new Fraction(sign * checked(h1, "fromNumber"), k1);
    }

    // "3", "-1/4", "0.25", "1.5e-1".
    static parse(text) {
        const s = String(text).trim();
        const m = /^(-?\d+)\s*\/\s*(\d+)$/.exec(s);
        if (m)
            return new Fraction(Number(m[1]), Number(m[2]));
        if (/^-?(\d+\.?\d*|\.\d+)(e-?\d+)?$/i.test(s)) {
            // Exact for finite decimals: "0.1" is 1/10, not the float's binary value.
            const [mant, expPart] = s.toLowerCase().split("e");
            const exp = expPart ? Number(expPart) : 0;
            const neg = mant.startsWith("-");
            const [ip, fp = ""] = mant.replace("-", "").split(".");
            const digits = Number((ip || "0") + fp);
            const scale = fp.length - exp;
            if (Number.isSafeInteger(digits) && scale >= 0 && scale <= 15)
                return new Fraction(neg ? -digits : digits, 10 ** scale);
            if (Number.isSafeInteger(digits) && scale < 0 && Number.isSafeInteger(digits * 10 ** -scale))
                return new Fraction((neg ? -digits : digits) * 10 ** -scale);
            return Fraction.fromNumber(Number(s));
        }
        throw new SyntaxError(`Not a number: "${s}"`);
    }
}

export const ZERO = new Fraction(0);
export const ONE = new Fraction(1);

// Anything time-like to a Fraction: a Fraction, an integer, a float (closest with denominator <= 1e6), a [n, d] pair,
// or a string ("1/3", "0.25").
export function F(x, d) {
    if (x instanceof Fraction)
        return x;
    if (d !== undefined)
        return new Fraction(x, d);
    if (typeof x === "number")
        return Number.isInteger(x) ? new Fraction(x) : Fraction.fromNumber(x);
    if (typeof x === "string")
        return Fraction.parse(x);
    if (Array.isArray(x) && x.length === 2)
        return new Fraction(x[0], x[1]);
    if (x !== null && typeof x === "object" && x.isMiniString === true) // a string literal from the editor (§8.3)
        return Fraction.parse(x.toString());
    throw new TypeError(`Not a time value: ${x}`);
}

export function isFraction(x) {
    return x instanceof Fraction;
}

export { gcd };
