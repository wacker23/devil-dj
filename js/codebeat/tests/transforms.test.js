import { test } from "node:test";
import assert from "node:assert/strict";
import { F } from "../fraction.js";
import { seq, cat, pure, sortEvents } from "../pattern.js";
import { mini } from "../mini.js";
import { s, n, note } from "../controls.js";
import { run, euclid } from "../transforms.js";
import { parseScale, degreeToNote } from "../scales.js";

const show = (p, b = 0, e) => p.show(b, e);
const values = (p, b = 0, e = 1) => sortEvents(p.onsets(b, e)).map((x) => x.value);
const count = (p, b, e) => p.onsets(b, e).length;

// --- the two §3 rows that needed these ---

test("§3 every(2, rev): cycle 0 reversed, cycle 1 normal", () => {
    const p = seq("a", "b", "c", "d").every(2, (x) => x.rev());
    assert.deepEqual(show(p, 0), ["d [0, 1/4)", "c [1/4, 1/2)", "b [1/2, 3/4)", "a [3/4, 1)"]);
    assert.deepEqual(show(p, 1), ["a [1, 5/4)", "b [5/4, 3/2)", "c [3/2, 7/4)", "d [7/4, 2)"]);
    assert.deepEqual(show(p, 2), show(p.early(0), 2)); // and reversed again
    assert.equal(show(p, 2)[0], "d [2, 9/4)");
});

test('§3 s("kick").off(1/8, x => x.gain(0.5)): kick [0,1), kick gain 0.5 [1/8,9/8)', () => {
    const p = s("kick").off(1 / 8, (x) => x.gain(0.5));
    assert.deepEqual(sortEvents(p.onsets(0, 1)).map((e) => e.show()), [
        "s:kick [0, 1)",
        "s:kick gain:0.5 [1/8, 9/8) part [1/8, 1)",
    ]);
    // The whole is [1/8, 9/8) as the spec says.
    const late = p.onsets(0, 1).find((e) => e.value.gain === 0.5);
    assert.equal(String(late.whole), "[1/8, 9/8)");
});

// --- every / whenCycle / iter / palindrome ---

test("every: patterned n, n <= 0 does nothing", () => {
    const p = pure("a").every("<2 3>", (x) => x.fast(2));
    assert.equal(count(p, 0, 1), 2); // cycle 0: n = 2, 0 mod 2 = 0
    assert.equal(count(p, 1, 2), 1); // cycle 1: n = 3, 1 mod 3 != 0
    assert.equal(count(pure("a").every(0, (x) => x.fast(2)), 0, 1), 1);
    assert.equal(count(pure("a").every(3, (x) => x.fast(2)), -3, -2), 2); // negative cycles too
});

test("whenCycle: any test of the cycle number", () => {
    const p = pure("a").whenCycle((c) => c > 1, (x) => x.fast(3));
    assert.deepEqual([0, 1, 2, 3].map((c) => count(p, c, c + 1)), [1, 1, 3, 3]);
});

test("iter: each cycle starts a step further in", () => {
    const p = mini("a b c d").iter(4);
    assert.deepEqual(show(p, 0).map((x) => x[0]), ["a", "b", "c", "d"]);
    assert.deepEqual(show(p, 1).map((x) => x[0]), ["b", "c", "d", "a"]);
    assert.deepEqual(show(p, 2).map((x) => x[0]), ["c", "d", "a", "b"]);
    assert.deepEqual(show(p, 4).map((x) => x[0]), ["a", "b", "c", "d"]);
    assert.deepEqual(show(p, 1), ["b [1, 5/4)", "c [5/4, 3/2)", "d [3/2, 7/4)", "a [7/4, 2)"]);
});

test("palindrome: forwards, then backwards", () => {
    const p = mini("a b c").palindrome();
    assert.deepEqual(show(p, 0).map((x) => x[0]), ["a", "b", "c"]);
    assert.deepEqual(show(p, 1).map((x) => x[0]), ["c", "b", "a"]);
    assert.deepEqual(show(p, 2).map((x) => x[0]), ["a", "b", "c"]);
});

// --- layering ---

test("superimpose and layer", () => {
    assert.deepEqual(show(mini("a b").superimpose((x) => x.fast(2))).length, 6);
    const l = s("bd").layer((x) => x.gain(0.5), (x) => x.speed(2));
    assert.deepEqual(values(l), [{ s: "bd", gain: 0.5 }, { s: "bd", speed: 2 }]);
});

test("off: patterned time", () => {
    const p = pure("a").off("<0.25 0.5>", (x) => x);
    assert.ok(show(p, 1).some((e) => e.startsWith("a [3/2, 5/2)")));
});

// --- ply ---

test("ply: each event repeated in its own time", () => {
    assert.deepEqual(show(mini("a b").ply(2)), ["a [0, 1/4)", "a [1/4, 1/2)", "b [1/2, 3/4)", "b [3/4, 1)"]);
    assert.deepEqual(show(mini("a b").ply("<2 3>"), 1).length, 6);
    assert.deepEqual(show(mini("a@3 b").ply(3)).slice(0, 3), ["a [0, 1/4)", "a [1/4, 1/2)", "a [1/2, 3/4)"]);
    assert.deepEqual(show(mini("a").ply(0)), []);
    // Long events: the repeats spread over the whole event.
    assert.deepEqual(show(mini("a/2").ply(2), 0, 2), ["a [0, 1)", "a [1, 2)"]);
});

// --- randomness ---

test("degradeBy: about p dropped, deterministic, seeds differ", () => {
    const p = mini("a*16").degradeBy(0.25);
    let kept = 0;
    for (let c = 0; c < 50; ++c)
        kept += count(p, c, c + 1);
    assert.ok(kept > 0.65 * 800 && kept < 0.85 * 800, `kept ${kept} of 800`);
    assert.deepEqual(show(p, 3), show(mini("a*16").degradeBy(0.25), 3));
    assert.notDeepEqual(show(p.seed(1), 3), show(p, 3));
    assert.deepEqual(show(p.seed(1), 3), show(p.seed(1), 3));
    assert.equal(count(mini("a*16").degradeBy(0), 0, 1), 16);
    assert.equal(count(mini("a*16").degradeBy(1), 0, 1), 0);
    assert.equal(count(mini("a*16").degrade(), 0, 1) < 16, true);
    assert.throws(() => mini("a").degradeBy(2), /chance between 0 and 1/);
});

test("sometimesBy / sometimes / often / rarely: about the share changed, the rest as they were", () => {
    const share = (p) => {
        let changed = 0, all = 0;
        for (let c = 0; c < 40; ++c)
            for (const e of p.onsets(c, c + 1)) {
                ++all;
                changed += e.value.gain === 0.5 ? 1 : 0;
            }
        return { changed: changed / all, all };
    };
    const base = s("hat*16");
    for (const [p, expected] of [
        [base.sometimes((x) => x.gain(0.5)), 0.5],
        [base.often((x) => x.gain(0.5)), 0.75],
        [base.rarely((x) => x.gain(0.5)), 0.25],
        [base.sometimesBy(0.1, (x) => x.gain(0.5)), 0.1],
    ]) {
        const r = share(p);
        assert.equal(r.all, 640); // every event is still there, once
        assert.ok(Math.abs(r.changed - expected) < 0.07, `${r.changed} vs ${expected}`);
    }
    // Two different functions choose independently.
    const a = base.sometimes((x) => x.gain(0.5)).onsets(0, 1).map((e) => e.value.gain === 0.5);
    const b = base.sometimes((x) => x.speed(2).gain(0.5)).onsets(0, 1).map((e) => e.value.gain === 0.5);
    assert.notDeepEqual(a, b);
});

// --- structure ---

test("struct: the rhythm of a boolean pattern", () => {
    assert.deepEqual(show(s("bd").struct("x ~ x x")), ["s:bd [0, 1/4)", "s:bd [1/2, 3/4)", "s:bd [3/4, 1)"]);
    assert.deepEqual(show(note("c3 e3").struct("t f t t").fmap((v) => v.note)), ["48 [0, 1/4)", "52 [1/2, 3/4)", "52 [3/4, 1)"]);
    assert.deepEqual(show(mini("a").struct("1 0 1")), ["a [0, 1/3)", "a [2/3, 1)"]);
});

test("mask: only where true, keeping the pattern's structure", () => {
    assert.deepEqual(show(mini("a b c d").mask("1 0 1 1")), ["a [0, 1/4)", "c [1/2, 3/4)", "d [3/4, 1)"]);
    assert.deepEqual(show(mini("a b").mask("<1 0>"), 1), []);
    assert.deepEqual(show(mini("a b").mask("<1 0>"), 2).length, 2);
    // A mask that changes inside an event keeps the true part of it (no new onset).
    assert.deepEqual(show(mini("a").mask("0 1")), ["a [0, 1) part [1/2, 1)"]);
    assert.equal(count(mini("a").mask("0 1"), 0, 1), 0);
});

test("euclid as a method and as a pattern", () => {
    assert.deepEqual(show(s("bd").euclid(3, 8)), ["s:bd [0, 1/8)", "s:bd [3/8, 1/2)", "s:bd [3/4, 7/8)"]);
    assert.equal(count(s("bd").euclid("<3 5>", 8), 1, 2), 5);
    assert.deepEqual(sortEvents(s("bd").euclid(3, 8, 2).onsets(0, 1)).map((e) => e.part.begin.mul(8).toNumber()), [1, 4, 6]);
    assert.equal(count(euclid(5, 8), 0, 1), 5);
    assert.throws(() => s("bd").euclid(2.5, 8), /whole number/);
});

// --- arithmetic ---

test("add / sub / mul / div on numbers and on note or n", () => {
    assert.deepEqual(values(mini("1 2").add(10)), [11, 12]);
    assert.deepEqual(values(mini("1 2").add("10 20")), [11, 22]);
    assert.deepEqual(values(mini("4").sub(1)), [3]);
    assert.deepEqual(values(mini("4").mul(3)), [12]);
    assert.deepEqual(values(mini("4").div(8)), [0.5]);
    assert.deepEqual(values(note("c3 e3").add(12)), [{ note: 60 }, { note: 64 }]);
    assert.deepEqual(values(n("0 2").add("<0 7>"), 1, 2), [{ n: 7 }, { n: 9 }]);
    assert.deepEqual(values(note("c3").s("saw").add({ note: 12 })), [{ note: 60, s: "saw" }]);
    assert.deepEqual(values(note("c3").gain(0.5).mul({ gain: 2 })), [{ note: 48, gain: 1 }]);
    // Structure from the left.
    assert.equal(count(note("c3").add("0 7 12"), 0, 1), 1);
    assert.throws(() => s("bd").add(1).queryArc(0, 1), /nothing to add 1 to/);
    assert.throws(() => mini("1").div(0).queryArc(0, 1), /div by zero/);
});

test("scale: degrees to notes", () => {
    assert.deepEqual(values(n("0 2 4").scale("C:minor")), [{ note: 48 }, { note: 51 }, { note: 55 }]);
    assert.deepEqual(values(n("0 7 -1").scale("C:major")), [{ note: 48 }, { note: 60 }, { note: 47 }]);
    assert.deepEqual(values(mini("0 1").scale("a4:minPent")), [{ note: 69 }, { note: 72 }]);
    assert.deepEqual(values(n("0").scale("<C:major F:major>"), 1, 2), [{ note: 53 }]);
    assert.deepEqual(values(n("2").s("piano").scale("D:dorian")), [{ s: "piano", note: 53 }]);
    assert.deepEqual(values(n("0").scale("harmonicminor")), [{ note: 48 }]);
    assert.throws(() => n("0").scale("C:nope").queryArc(0, 1), /no scale called "nope"/);
    assert.throws(() => n("0").scale("H:minor").queryArc(0, 1), /isn't a root note/);
    // Every scale in the spec is there, starting on its root.
    for (const name of ["major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian", "harmonicMinor",
        "melodicMinor", "majPent", "minPent", "blues", "chromatic"]) {
        const sc = parseScale(`C:${name}`);
        assert.equal(degreeToNote(sc, 0), 48, name);
        assert.equal(degreeToNote(sc, sc.steps.length), 60, name);
    }
});

test("hurry: faster and pitched up", () => {
    assert.deepEqual(values(s("bd sd").hurry(2)), [{ s: "bd", speed: 2 }, { s: "sd", speed: 2 }, { s: "bd", speed: 2 }, { s: "sd", speed: 2 }]);
    assert.deepEqual(values(s("bd").speed(0.5).hurry(2)).map((v) => v.speed), [1, 1]);
});

test("run: 0 .. n-1", () => {
    assert.deepEqual(values(run(4)), [0, 1, 2, 3]);
    assert.deepEqual(values(n(run(3))), [{ n: 0 }, { n: 1 }, { n: 2 }]);
    assert.deepEqual(values(run("<2 4>"), 1, 2), [0, 1, 2, 3]);
});

test("transforms keep onsets exact across query windows", () => {
    const p = s("bd*4").every(3, (x) => x.fast(2)).off(F(1, 3), (x) => x.gain(0.4)).iter(4).sometimes((x) => x.speed(2));
    const full = p.onsets(0, 8).map((e) => e.show().split(" part")[0]).sort();
    const pieces = [];
    for (let i = 0; i < 8 * 5; ++i)
        pieces.push(...p.onsets(F(i, 5), F(i + 1, 5)).map((e) => e.show().split(" part")[0]));
    assert.deepEqual(pieces.sort(), full);
});
