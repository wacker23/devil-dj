import { test } from "node:test";
import assert from "node:assert/strict";
import { F } from "../fraction.js";
import { sortEvents } from "../pattern.js";
import { s, n } from "../controls.js";
import { sine, cosine, saw, isaw, tri, square, rand, irand, perlin, signal } from "../signals.js";

const at = (sig, t, controls = {}) => sig.queryArc(t, t, controls)[0].value;
const close = (a, b, msg) => assert.ok(Math.abs(a - b) < 1e-9, `${msg ?? ""} ${a} vs ${b}`);
const segValues = (p, b = 0, e = 1) => sortEvents(p.onsets(b, e)).map((x) => x.value);

test("signals have no whole: one event over the queried span, no onsets", () => {
    const events = sine.queryArc(F(1, 4), F(1, 2));
    assert.equal(events.length, 1);
    assert.equal(events[0].whole, null);
    assert.equal(String(events[0].part), "[1/4, 1/2)");
    assert.equal(sine.onsets(0, 1).length, 0);
    // The value is taken at the begin of the query.
    close(events[0].value, 1);
});

test("the shapes over one cycle", () => {
    close(at(sine, 0), 0.5); close(at(sine, F(1, 4)), 1); close(at(sine, F(1, 2)), 0.5); close(at(sine, F(3, 4)), 0);
    close(at(cosine, 0), 1); close(at(cosine, F(1, 2)), 0); close(at(cosine, F(1, 4)), 0.5);
    close(at(saw, 0), 0); close(at(saw, F(1, 4)), 0.25); close(at(saw, F(3, 4)), 0.75);
    close(at(isaw, 0), 1); close(at(isaw, F(1, 4)), 0.75);
    close(at(tri, 0), 0); close(at(tri, F(1, 4)), 0.5); close(at(tri, F(1, 2)), 1); close(at(tri, F(3, 4)), 0.5);
    close(at(square, 0), 0); close(at(square, F(1, 4)), 0); close(at(square, F(1, 2)), 1); close(at(square, F(3, 4)), 1);
    // Every cycle the same, negative cycles too.
    for (const sig of [sine, cosine, saw, isaw, tri, square])
        for (const t of [F(1, 8), F(5, 7)]) {
            close(at(sig, t.add(3)), at(sig, t));
            close(at(sig, t.sub(2)), at(sig, t));
        }
});

test("all signals stay in [0, 1]", () => {
    for (const [name, sig] of Object.entries({ sine, cosine, saw, isaw, tri, square, rand, perlin }))
        for (let i = 0; i < 400; ++i) {
            const v = at(sig, F(i, 37));
            assert.ok(v >= 0 && v <= 1, `${name} at ${i}/37: ${v}`);
        }
});

test("range: scaled, patterned bounds", () => {
    close(at(sine.range(0.4, 1), F(1, 4)), 1);
    close(at(sine.range(0.4, 1), F(3, 4)), 0.4);
    close(at(saw.range(100, 200), F(1, 2)), 150);
    close(at(saw.range(1, -1), F(1, 4)), 0.5); // upside down works
    // Patterned bounds: the high end alternates per cycle.
    close(at(saw.range(0, "<10 20>"), F(3, 2)), 10);
    close(at(saw.range(0, "<10 20>"), F(1, 2)), 5);
});

test("segment: discrete samples with wholes", () => {
    assert.deepEqual(segValues(saw.segment(4)), [0, 0.25, 0.5, 0.75]);
    assert.deepEqual(sortEvents(saw.segment(4).queryArc(0, 1)).map((e) => String(e.whole)), ["[0, 1/4)", "[1/4, 1/2)", "[1/2, 3/4)", "[3/4, 1)"]);
    assert.deepEqual(segValues(square.segment(2)), [0, 1]);
    assert.deepEqual(segValues(saw.segment("<2 4>"), 1, 2).length, 4);
    assert.throws(() => saw.segment(0), /above 0/);
});

test('as a control: s("hat*8").gain(sine.range(0.4, 1)) - each hat at its own start', () => {
    const hats = sortEvents(s("hat*8").gain(sine.range(0.4, 1)).onsets(0, 1));
    assert.equal(hats.length, 8);
    hats.forEach((e, i) => close(e.value.gain, 0.4 + 0.6 * (Math.sin((2 * Math.PI * i) / 8) + 1) / 2, `hat ${i}`));
    assert.ok(hats.every((e) => e.value.s === "hat"));
    // As notes: n(saw.range(0, 4).segment(4)) climbs 0 1 2 3.
    assert.deepEqual(segValues(n(saw.range(0, 4).segment(4))), [{ n: 0 }, { n: 1 }, { n: 2 }, { n: 3 }]);
});

test("rand: seeded white noise - the same seed, the same values", () => {
    const values = (seed) => Array.from({ length: 64 }, (_, i) => at(rand, F(i, 16), { seed }));
    assert.deepEqual(values(0), values(0));
    assert.notDeepEqual(values(0), values(1));
    assert.deepEqual(rand.seed(5).segment(16).onsets(0, 4).map((e) => e.value), rand.seed(5).segment(16).onsets(0, 4).map((e) => e.value));
    // Spread: the mean of many samples is near 1/2.
    const many = Array.from({ length: 4000 }, (_, i) => at(rand, F(i, 64)));
    const mean = many.reduce((a, b) => a + b, 0) / many.length;
    assert.ok(Math.abs(mean - 0.5) < 0.03, `mean ${mean}`);
    // Used for choices: sometimes-like control randomness, deterministic per cycle.
    const g = (seed) => s("hat*8").gain(rand).seed(seed).onsets(2, 3).map((e) => e.value.gain);
    assert.deepEqual(g(3), g(3));
    assert.notDeepEqual(g(3), g(4));
});

test("irand: whole numbers 0 .. n-1, all of them", () => {
    const seen = new Set();
    for (let i = 0; i < 500; ++i) {
        const v = at(irand(8), F(i, 10));
        assert.ok(Number.isInteger(v) && v >= 0 && v < 8, `${v}`);
        seen.add(v);
    }
    assert.equal(seen.size, 8);
    assert.ok(Number.isInteger(at(irand("<4 6>"), F(3, 2))));
    assert.throws(() => irand(0), /whole number of 1 or more/);
});

test("perlin: smooth - neighbours close, whole cycles random", () => {
    for (let i = 0; i < 300; ++i) {
        const a = at(perlin, F(i, 100)), b = at(perlin, F(i + 1, 100));
        assert.ok(Math.abs(a - b) < 0.03, `jump at ${i}/100: ${a} -> ${b}`);
    }
    // Continuous across the cycle line.
    close(at(perlin, F(999999, 1000000)), at(perlin, 1), "at 1");
    assert.notEqual(at(perlin, 0), at(perlin, 1));
    assert.notEqual(at(perlin, 0, { seed: 2 }), at(perlin, 0));
    assert.equal(at(perlin, F(1, 3), { seed: 9 }), at(perlin, F(1, 3), { seed: 9 }));
});

test("signals work with the core: fast, slow, early, add", () => {
    close(at(saw.fast(2), F(1, 4)), 0.5);
    close(at(saw.slow(2), F(1, 2)), 0.25);
    close(at(saw.early(F(1, 4)), 0), 0.25);
    close(at(saw.add(1), F(1, 2)), 1.5);
    // A custom signal.
    close(at(signal((t) => t.toNumber() * 2), F(3, 4)), 1.5);
});
