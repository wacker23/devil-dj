import { test } from "node:test";
import assert from "node:assert/strict";
import { F, Fraction, ZERO, ONE } from "../fraction.js";

const eqF = (x, s) => assert.equal(String(x), s);

test("fractions are normalised", () => {
    eqF(F(2, 4), "1/2");
    eqF(F(-2, -4), "1/2");
    eqF(F(2, -4), "-1/2");
    eqF(F(0, 5), "0");
    assert.equal(F(0, -5).n, 0);
    assert.ok(Object.is(F(0, -5).n, 0)); // not -0
    eqF(F(6), "6");
    assert.ok(F(3, 6).eq(F(1, 2)));
});

test("bad input is refused", () => {
    assert.throws(() => F(1, 0), RangeError);
    assert.throws(() => new Fraction(1.5, 2), TypeError);
    assert.throws(() => F("one half"), SyntaxError);
    assert.throws(() => F({}), TypeError);
    assert.throws(() => F(1).div(0), RangeError);
});

test("arithmetic is exact", () => {
    eqF(F(1, 3).add(F(1, 6)), "1/2");
    eqF(F(1, 3).sub(F(1, 2)), "-1/6");
    eqF(F(2, 3).mul(F(9, 4)), "3/2");
    eqF(F(2, 3).div(F(4, 9)), "3/2");
    eqF(F(1, 10).add(F(2, 10)), "3/10"); // where floats give 0.30000000000000004
    eqF(F(-3, 4).abs(), "3/4");
    eqF(F(3, 4).neg(), "-3/4");
    // Accepts plain arguments.
    eqF(F(1, 4).add(1), "5/4");
    eqF(F(1, 4).mul("2/3"), "1/6");
    eqF(F(1, 4).add(0.25), "1/2");
});

test("adding many small steps does not drift", () => {
    let t = ZERO;
    for (let i = 0; i < 96000; ++i)
        t = t.add(F(1, 96000));
    assert.ok(t.eq(ONE));
    let u = ZERO;
    for (let i = 0; i < 1000; ++i)
        u = u.add(F(1, 3)).add(F(1, 7));
    eqF(u, "10000/21");
});

test("overflow throws instead of losing precision", () => {
    const big = F(Number.MAX_SAFE_INTEGER - 1, 1);
    assert.throws(() => big.add(big), RangeError);
    assert.throws(() => F(1, 2 ** 40).mul(F(1, 2 ** 40 - 1)), RangeError);
    // Cross-reduction keeps exact products that would otherwise overflow.
    eqF(F(2 ** 40, 3).mul(F(3, 2 ** 40)), "1");
});

test("comparison", () => {
    assert.ok(F(1, 3).lt(F(1, 2)));
    assert.ok(F(1, 2).lte(F(2, 4)));
    assert.ok(F(-1, 2).lt(0));
    assert.ok(F(5, 4).gt(1));
    assert.ok(F(5, 4).gte("5/4"));
    assert.equal(F(1, 3).compare(F(1, 3)), 0);
    eqF(F(1, 3).min(F(1, 4)), "1/4");
    eqF(F(1, 3).max(F(1, 4)), "1/3");
    // Large values whose cross products overflow still compare correctly.
    const a = F(Number.MAX_SAFE_INTEGER - 2, 3), b = F(Number.MAX_SAFE_INTEGER - 4, 3);
    assert.ok(a.gt(b));
    assert.ok(F(2 ** 52 + 1, 2 ** 26).gt(F(2 ** 52, 2 ** 26 + 1)));
});

test("cycles: floor, ceil, sam, nextSam, cyclePos", () => {
    assert.equal(F(7, 4).floor(), 1);
    assert.equal(F(-1, 4).floor(), -1);
    assert.equal(F(-4, 4).floor(), -1);
    assert.equal(F(2).floor(), 2);
    assert.equal(F(7, 4).ceil(), 2);
    assert.equal(F(2).ceil(), 2);
    assert.equal(F(-1, 4).ceil(), 0);
    eqF(F(7, 4).sam(), "1");
    eqF(F(7, 4).nextSam(), "2");
    eqF(F(2).nextSam(), "3");
    eqF(F(7, 4).cyclePos(), "3/4");
    eqF(F(-1, 4).cyclePos(), "3/4");
    assert.ok(F(3).isInteger());
    assert.ok(!F(3, 2).isInteger());
    // Exact even where float division would round to the next integer.
    const n = 2 ** 53 - 1;
    assert.equal(F(n - 1, n).floor(), 0);
});

test("from numbers and strings", () => {
    eqF(F(0.25), "1/4");
    eqF(F(1.5), "3/2");
    eqF(F(-0.125), "-1/8");
    eqF(F(1 / 3), "1/3");
    eqF(Fraction.fromNumber(Math.PI, 1000), "355/113");
    eqF(Fraction.fromNumber(0.1, 96000), "1/10");
    eqF(Fraction.fromNumber(12345 / 96000, 96000), "823/6400");
    eqF(F("3"), "3");
    eqF(F("-1/4"), "-1/4");
    eqF(F(" 2 / 6 "), "1/3");
    eqF(F("0.1"), "1/10");
    eqF(F(".5"), "1/2");
    eqF(F("1.5e-1"), "3/20");
    eqF(F("2e3"), "2000");
    eqF(F([3, 9]), "1/3");
    eqF(F({ isMiniString: true, toString: () => "3/4" }), "3/4");
    // Every denominator up to the limit round-trips.
    for (let d = 1; d <= 200; ++d)
        for (let n = -d; n <= 2 * d; n += 7)
            assert.ok(Fraction.fromNumber(n / d).eq(F(n, d)), `${n}/${d}`);
});

test("conversion out", () => {
    assert.equal(F(3, 4).toNumber(), 0.75);
    assert.equal(+F(3, 4), 0.75);
    assert.equal(JSON.stringify({ t: F(3, 4) }), '{"t":"3/4"}');
    assert.ok(Object.isFrozen(F(1, 2)));
});
