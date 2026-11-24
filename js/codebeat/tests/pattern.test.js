import { test } from "node:test";
import assert from "node:assert/strict";
import { F } from "../fraction.js";
import { S } from "../span.js";
import { Event } from "../event.js";
import {
    Pattern, pure, silence, stack, slowcat, fastcat, seq, cat, reify, patterned, lift2, sortEvents,
} from "../pattern.js";

// Event strings over [begin, end): "a [0, 1/2)", fragments "a [0, 2) part [1, 2)".
const show = (p, b = 0, e) => p.show(b, e);
const onsets = (p, b, e) => sortEvents(p.onsets(b, e)).map((x) => x.show());

// --- CODEBEAT_SPEC §3, one test per row (rows needing controls and tier-1 transforms are in P5-04 / P5-05) ---

test("§3 pure: a [0,1)", () => {
    assert.deepEqual(show(pure("a")), ["a [0, 1)"]);
});

test("§3 fastcat (= seq): a [0,1/2), b [1/2,1)", () => {
    assert.deepEqual(show(fastcat("a", "b")), ["a [0, 1/2)", "b [1/2, 1)"]);
    assert.deepEqual(show(seq("a", "b")), ["a [0, 1/2)", "b [1/2, 1)"]);
});

test("§3 slowcat (= cat): a, b, a over cycles 0, 1, 2", () => {
    const p = slowcat("a", "b");
    assert.deepEqual(show(p, 0), ["a [0, 1)"]);
    assert.deepEqual(show(p, 1), ["b [1, 2)"]);
    assert.deepEqual(show(p, 2), ["a [2, 3)"]);
    assert.deepEqual(show(cat("a", "b"), 0, 3), ["a [0, 1)", "b [1, 2)", "a [2, 3)"]);
});

test("§3 stack: a and b at once", () => {
    assert.deepEqual(show(stack("a", "b")), ["a [0, 1)", "b [0, 1)"]);
});

test("§3 fast(2): a twice", () => {
    assert.deepEqual(show(pure("a").fast(2)), ["a [0, 1/2)", "a [1/2, 1)"]);
});

test("§3 slow(2): one a over two cycles, the onset in cycle 0 only", () => {
    const p = pure("a").slow(2);
    assert.deepEqual(show(p, 0), ["a [0, 2) part [0, 1)"]);
    assert.deepEqual(show(p, 1), ["a [0, 2) part [1, 2)"]);
    assert.equal(p.onsets(0, 1).length, 1);
    assert.equal(p.onsets(1, 2).length, 0);
});

test("§3 rev: b then a", () => {
    assert.deepEqual(show(seq("a", "b").rev()), ["b [0, 1/2)", "a [1/2, 1)"]);
});

test("§3 early(1/4): b [1/4,3/4), a [3/4,5/4), and the tail of the previous a", () => {
    const events = show(seq("a", "b").early(F(1, 4)));
    assert.deepEqual(events, ["a [-1/4, 1/4) part [0, 1/4)", "b [1/4, 3/4)", "a [3/4, 5/4) part [3/4, 1)"]);
    assert.deepEqual(onsets(seq("a", "b").early(F(1, 4)), 0, 1), ["b [1/4, 3/4)", "a [3/4, 5/4) part [3/4, 1)"]);
});

test.todo("§3 every(2, rev) - P5-05");
test.todo("§3 s(\"kick\").off(1/8, x => x.gain(0.5)) - P5-04 / P5-05");

// --- the rest of the core ---

test("silence has no events", () => {
    assert.deepEqual(show(silence), []);
    assert.deepEqual(show(seq("a", silence)), ["a [0, 1/2)"]);
});

test("queries across cycles split where needed", () => {
    assert.deepEqual(show(pure("a"), "1/2", "5/2"), ["a [0, 1) part [1/2, 1)", "a [1, 2)", "a [2, 3) part [2, 5/2)"]);
    assert.deepEqual(show(seq("a", "b"), "1/4", "3/4"), ["a [0, 1/2) part [1/4, 1/2)", "b [1/2, 1) part [1/2, 3/4)"]);
    // A zero-width query: what is active at that point.
    assert.deepEqual(pure("a").queryArc("1/2", "1/2").map(String), ["a [0, 1) part [1/2, 1/2)"]);
});

test("nesting: seq inside seq, cat inside seq", () => {
    assert.deepEqual(show(seq("a", seq("b", "c"))), ["a [0, 1/2)", "b [1/2, 3/4)", "c [3/4, 1)"]);
    const p = seq("a", cat("b", "c"));
    assert.deepEqual(show(p, 0), ["a [0, 1/2)", "b [1/2, 1)"]);
    assert.deepEqual(show(p, 1), ["a [1, 3/2)", "c [3/2, 2)"]);
});

test("slowcat gives each pattern its own cycles in turn", () => {
    // cat(seq(a, b).slow(2), c): the slow one plays its first half in cycle 0 and its second half in cycle 2.
    const p = cat(seq("a", "b").slow(2), "c");
    assert.deepEqual(show(p, 0), ["a [0, 1)"]);
    assert.deepEqual(show(p, 1), ["c [1, 2)"]);
    assert.deepEqual(show(p, 2), ["b [2, 3)"]);
    assert.deepEqual(show(cat(), 0), []);
    // Negative cycles too.
    assert.deepEqual(show(cat("a", "b"), -1), ["b [-1, 0)"]);
});

test("fast and slow by fractions, zero and negative", () => {
    assert.deepEqual(show(seq("a", "b").fast("3/2"), 0, 1), ["a [0, 1/3)", "b [1/3, 2/3)", "a [2/3, 1)"]);
    assert.deepEqual(show(seq("a", "b", "c").slow(F(3, 2))), ["a [0, 1/2)", "b [1/2, 1)"]);
    assert.deepEqual(show(pure("a").fast(0)), []);
    assert.deepEqual(show(pure("a").slow(0)), []);
    assert.deepEqual(show(seq("a", "b").fast(-1)), ["b [0, 1/2)", "a [1/2, 1)"]);
    assert.deepEqual(show(pure("a").fast(0.5)), ["a [0, 2) part [0, 1)"]);
});

test("early and late are opposites and keep events exact", () => {
    const p = seq("a", "b", "c");
    assert.deepEqual(show(p.early("1/3").late("1/3")), show(p));
    assert.deepEqual(show(p.late(1)), show(p));
    assert.deepEqual(onsets(p.late(F(1, 6)), 0, 1), ["a [1/6, 1/2)", "b [1/2, 5/6)", "c [5/6, 7/6) part [5/6, 1)"]);
});

test("rev works per cycle, also on later cycles and long events", () => {
    const p = cat(seq("a", "b"), seq("c", "d", "e")).rev();
    assert.deepEqual(show(p, 1), ["e [1, 4/3)", "d [4/3, 5/3)", "c [5/3, 2)"]);
    // An event longer than the cycle is reflected as a whole.
    assert.deepEqual(show(pure("a").slow(2).rev(), 0), ["a [-1, 1) part [0, 1)"]);
    assert.deepEqual(show(seq("a", "b").rev().rev()), show(seq("a", "b")));
});

test("fmap and withValue change values only", () => {
    assert.deepEqual(show(seq(1, 2).fmap((x) => x * 10)), ["10 [0, 1/2)", "20 [1/2, 1)"]);
    assert.deepEqual(show(seq("a", "b").withValue((v) => v.toUpperCase())), ["A [0, 1/2)", "B [1/2, 1)"]);
});

test("withTime: queries one way, events back", () => {
    const twice = seq("a", "b").withTime((t) => t.mul(2), (t) => t.div(2));
    assert.deepEqual(show(twice), show(seq("a", "b").fast(2)));
});

test("appLeft: structure from the left", () => {
    const add = lift2((x, y) => x + y);
    // seq(1, 2) + seq(10, 20, 30): two events, each taking the right values active during it.
    assert.deepEqual(show(add(seq(1, 2), seq(10, 20, 30))), [
        "11 [0, 1/2) part [0, 1/3)",
        "21 [0, 1/2) part [1/3, 1/2)",
        "22 [1/2, 1) part [1/2, 2/3)",
        "32 [1/2, 1) part [2/3, 1)",
    ]);
    // Onsets: one per left event.
    assert.deepEqual(onsets(add(seq(1, 2), seq(10, 20, 30)), 0, 1), ["11 [0, 1/2) part [0, 1/3)", "22 [1/2, 1) part [1/2, 2/3)"]);
});

test("appRight: structure from the right", () => {
    const fs = seq(1, 2).fmap((x) => (y) => x + y);
    assert.deepEqual(show(fs.appRight(seq(10, 20, 30))), [
        "11 [0, 1/3)",
        "21 [1/3, 2/3) part [1/3, 1/2)",
        "22 [1/3, 2/3) part [1/2, 2/3)",
        "32 [2/3, 1)",
    ]);
});

test("appBoth: structure from both", () => {
    const fs = seq(1, 2).fmap((x) => (y) => x + y);
    assert.deepEqual(show(fs.appBoth(seq(10, 20, 30))), ["11 [0, 1/3)", "21 [1/3, 1/2)", "22 [1/2, 2/3)", "32 [2/3, 1)"]);
    // With a signal-like (no whole) side, the result has no whole.
    const signal = new Pattern((state) => [new Event(null, state.span, 5)]);
    assert.deepEqual(show(fs.appBoth(signal)), ["6 ~[0, 1/2)", "7 ~[1/2, 1)"]);
});

test("patterned arguments: fast(cat(1, 2)) and slow(seq(1, 2))", () => {
    const p = pure("a").fast(cat(1, 2));
    assert.deepEqual(show(p, 0), ["a [0, 1)"]);
    assert.deepEqual(show(p, 1), ["a [1, 3/2)", "a [3/2, 2)"]);
    // Structure from the pattern being changed: the argument only chooses the factor in each of its own events.
    const q = seq("a", "b", "c", "d").fast(seq(1, 2));
    assert.deepEqual(show(q), ["a [0, 1/4)", "b [1/4, 1/2)", "a [1/2, 5/8)", "b [5/8, 3/4)", "c [3/4, 7/8)", "d [7/8, 1)"]);
    assert.deepEqual(show(pure("a").early(cat(0, "1/2")), 1), ["a [1/2, 3/2) part [1, 3/2)", "a [3/2, 5/2) part [3/2, 2)"]);
    assert.equal(patterned(2, (x) => x * 3), 6);
});

test("contexts (source locations) are carried and combined", () => {
    const at = (p, start, end) => new Pattern((state) => p.query(state).map((e) => e.withContext({ locations: [{ start, end }] })));
    const a = at(pure(1), 0, 1);
    const b = at(seq(10, 20), 4, 6);
    const sum = lift2((x, y) => x + y)(a, b);
    assert.deepEqual(sum.queryArc(0, 1)[0].context.locations, [{ start: 0, end: 1 }, { start: 4, end: 6 }]);
    assert.deepEqual(a.fast(2).queryArc(0, 1)[1].context.locations, [{ start: 0, end: 1 }]);
});

test("reify: patterns stay, other values become pure", () => {
    const p = seq("a", "b");
    assert.equal(reify(p), p);
    assert.deepEqual(show(reify(3)), ["3 [0, 1)"]);
    assert.deepEqual(show(reify("x")), ["x [0, 1)"]); // until the mini-notation parser is installed (P5-03)
});

test("the same query always gives the same events", () => {
    const p = cat(seq("a", "b").fast(cat(1, 3)), stack("c", seq("d", "e").rev())).early("1/7");
    for (let c = 0; c < 5; ++c)
        assert.deepEqual(show(p, c), show(p, c));
    // And windows of a longer query add up to it: no gaps, no doubles.
    const whole = onsets(p, 0, 4);
    const pieces = [];
    for (let i = 0; i < 4 * 7; ++i)
        pieces.push(...onsets(p, F(i, 7), F(i + 1, 7)));
    assert.deepEqual(pieces.map((s) => s.split(" part")[0]), whole.map((s) => s.split(" part")[0]));
});

test("spans used in events are exact fractions", () => {
    const [e] = pure("a").fast(3).queryArc(0, F(1, 3));
    assert.ok(e.whole.equals(S(0, F(1, 3))));
});
