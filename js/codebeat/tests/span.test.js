import { test } from "node:test";
import assert from "node:assert/strict";
import { F } from "../fraction.js";
import { S, Span } from "../span.js";

const spans = (list) => list.map(String);

test("spans hold exact begin and end", () => {
    const s = S(0, "1/2");
    assert.equal(String(s), "[0, 1/2)");
    assert.equal(String(s.duration()), "1/2");
    assert.equal(String(s.midpoint()), "1/4");
    assert.ok(S("1/2", 1).equals(new Span(F(1, 2), F(1))));
    assert.ok(!S(0, 1).equals(S(0, 2)));
    assert.throws(() => S(1, 0), RangeError);
    assert.ok(S(1, 1).isZeroWidth());
    assert.ok(Object.isFrozen(s));
});

test("contains: begin in, end out", () => {
    assert.ok(S(0, 1).contains(0));
    assert.ok(S(0, 1).contains("3/4"));
    assert.ok(!S(0, 1).contains(1));
    assert.ok(!S(0, 1).contains("-1/4"));
});

test("splitting at whole cycles", () => {
    assert.deepEqual(spans(S("1/2", "5/2").spanCycles()), ["[1/2, 1)", "[1, 2)", "[2, 5/2)"]);
    assert.deepEqual(spans(S(0, 1).spanCycles()), ["[0, 1)"]);
    assert.deepEqual(spans(S(0, 3).spanCycles()), ["[0, 1)", "[1, 2)", "[2, 3)"]);
    assert.deepEqual(spans(S("1/4", "3/4").spanCycles()), ["[1/4, 3/4)"]);
    assert.deepEqual(spans(S("-1/2", "1/2").spanCycles()), ["[-1/2, 0)", "[0, 1/2)"]);
    assert.deepEqual(spans(S(2, 2).spanCycles()), ["[2, 2)"]);
    // The pieces cover the span exactly: no gaps, no overlaps.
    const parts = S("1/3", "17/5").spanCycles();
    for (let i = 1; i < parts.length; ++i)
        assert.ok(parts[i].begin.eq(parts[i - 1].end));
    assert.ok(parts[0].begin.eq(F(1, 3)) && parts.at(-1).end.eq(F(17, 5)));
});

test("whole cycle and position in the cycle", () => {
    assert.equal(String(S("5/4", "3/2").wholeCycle()), "[1, 2)");
    assert.equal(String(S("5/4", "3/2").cyclePos()), "[1/4, 1/2)");
    assert.equal(String(S("-1/4", 0).wholeCycle()), "[-1, 0)");
});

test("time functions", () => {
    assert.equal(String(S("1/4", "1/2").withTime((t) => t.mul(2))), "[1/2, 1)");
    assert.equal(String(S("1/4", "1/2").withEnd((t) => t.add(1))), "[1/4, 3/2)");
    // Reflection inside the cycle: [1 + 1/4, 1 + 1/2) -> [1 + 1/2, 1 + 3/4), ends in order.
    assert.equal(String(S("5/4", "3/2").withCycle((t) => F(1).sub(t))), "[3/2, 7/4)");
    assert.equal(String(S("5/4", "3/2").withCycle((t) => t.mul(2))), "[3/2, 2)");
});

test("intersection", () => {
    assert.equal(String(S(0, 1).intersection(S("1/2", 2))), "[1/2, 1)");
    assert.equal(String(S("1/4", "3/4").intersection(S(0, 1))), "[1/4, 3/4)");
    assert.equal(S(0, 1).intersection(S(2, 3)), null);
    // Touching ends do not overlap.
    assert.equal(S(0, 1).intersection(S(1, 2)), null);
    assert.equal(S(1, 2).intersection(S(0, 1)), null);
    // A point at the begin of, or inside, a span does; one at its end does not.
    assert.equal(String(S(1, 1).intersection(S(1, 2))), "[1, 1)");
    assert.equal(String(S("3/2", "3/2").intersection(S(1, 2))), "[3/2, 3/2)");
    assert.equal(String(S(1, 2).intersection(S("3/2", "3/2"))), "[3/2, 3/2)");
    assert.equal(S(2, 2).intersection(S(1, 2)), null);
    assert.equal(String(S(1, 1).intersection(S(1, 1))), "[1, 1)");
    assert.equal(S(1, 1).intersection(S(2, 2)), null);
});
