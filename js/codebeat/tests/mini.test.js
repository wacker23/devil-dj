import { test } from "node:test";
import assert from "node:assert/strict";
import { F } from "../fraction.js";
import { sortEvents, seq, reify } from "../pattern.js";
import { mini, MiniError } from "../mini.js";
import { bjorklund, euclidSteps } from "../euclid.js";
import { randomAt } from "../random.js";

const show = (src, b = 0, e) => mini(src).show(b, e);
const onsetSteps = (src, n) =>
    sortEvents(mini(src).onsets(0, 1)).map((e) => e.part.begin.mul(n).toNumber());

// --- CODEBEAT_SPEC §4.2, one test per row ---

test('§4.2 "a b"', () => {
    assert.deepEqual(show("a b"), ["a [0, 1/2)", "b [1/2, 1)"]);
});

test('§4.2 "a ~ b" (and "-" as a rest)', () => {
    assert.deepEqual(show("a ~ b"), ["a [0, 1/3)", "b [2/3, 1)"]);
    assert.deepEqual(show("a - b"), ["a [0, 1/3)", "b [2/3, 1)"]);
});

test('§4.2 "a [b c]"', () => {
    assert.deepEqual(show("a [b c]"), ["a [0, 1/2)", "b [1/2, 3/4)", "c [3/4, 1)"]);
});

test('§4.2 "<a b>"', () => {
    assert.deepEqual(show("<a b>", 0), ["a [0, 1)"]);
    assert.deepEqual(show("<a b>", 1), ["b [1, 2)"]);
    assert.deepEqual(show("<a b>", 2), ["a [2, 3)"]);
});

test('§4.2 "a <b c>"', () => {
    assert.deepEqual(show("a <b c>", 0), ["a [0, 1/2)", "b [1/2, 1)"]);
    assert.deepEqual(show("a <b c>", 1), ["a [1, 3/2)", "c [3/2, 2)"]);
});

test('§4.2 "a*2 b"', () => {
    assert.deepEqual(show("a*2 b"), ["a [0, 1/4)", "a [1/4, 1/2)", "b [1/2, 1)"]);
});

test('§4.2 "a/2": one event over two cycles, the onset in cycle 0 only', () => {
    assert.deepEqual(show("a/2", 0), ["a [0, 2) part [0, 1)"]);
    assert.deepEqual(show("a/2", 1), ["a [0, 2) part [1, 2)"]);
    assert.equal(mini("a/2").onsets(0, 2).length, 1);
});

test('§4.2 "a, b c"', () => {
    assert.deepEqual(show("a, b c"), ["a [0, 1)", "b [0, 1/2)", "c [1/2, 1)"]);
});

test('§4.2 "a!3 b": four steps a a a b', () => {
    assert.deepEqual(show("a!3 b"), ["a [0, 1/4)", "a [1/4, 1/2)", "a [1/2, 3/4)", "b [3/4, 1)"]);
});

test('§4.2 "a@3 b"', () => {
    assert.deepEqual(show("a@3 b"), ["a [0, 3/4)", "b [3/4, 1)"]);
});

test('§4.2 "a(3,8)": onsets at steps 0, 3, 6', () => {
    assert.deepEqual(show("a(3,8)"), ["a [0, 1/8)", "a [3/8, 1/2)", "a [3/4, 7/8)"]);
});

test('§4.2 "a(3,8,2)": rotated left by 2, onsets at steps 1, 4, 6', () => {
    assert.deepEqual(onsetSteps("a(3,8,2)", 8), [1, 4, 6]);
});

test('§4.2 "a?": kept or dropped with p = 0.5, the same every time for a seed', () => {
    const kept = [];
    for (let c = 0; c < 400; ++c)
        kept.push(mini("a?").onsets(c, c + 1).length);
    const n = kept.reduce((s, x) => s + x, 0);
    assert.ok(n > 160 && n < 240, `kept ${n} of 400`);
    // Deterministic: the same again, and the same as a fresh parse.
    for (let c = 0; c < 50; ++c)
        assert.equal(mini("a?").onsets(c, c + 1).length, kept[c]);
    // Another seed, another choice (for some cycle among the first 50).
    let differs = false;
    for (let c = 0; c < 50 && !differs; ++c)
        differs = mini("a?").onsets(c, c + 1, { seed: 7 }).length !== kept[c];
    assert.ok(differs);
    // "?0.9" drops most; "?0" none.
    const most = Array.from({ length: 200 }, (_, c) => mini("a?0.9").onsets(c, c + 1).length).reduce((s, x) => s + x, 0);
    assert.ok(most < 40, `kept ${most} of 200`);
    assert.equal(mini("a*8?0").onsets(0, 1).length, 8);
    // Each event decided on its own: "a*8?" keeps some of the 8, not all or none every cycle.
    const counts = new Set(Array.from({ length: 20 }, (_, c) => mini("a*8?").onsets(c, c + 1).length));
    assert.ok(counts.size > 2);
});

test('§4.2 "kick:3" is { s: "kick", n: 3 }', () => {
    const [e] = mini("kick:3").queryArc(0, 1);
    assert.deepEqual(e.value, { s: "kick", n: 3 });
    assert.deepEqual(show("bd:0 sd:12"), ["s:bd n:0 [0, 1/2)", "s:sd n:12 [1/2, 1)"]);
});

test('§4.2 "0 .5 -1": numbers are values', () => {
    const values = mini("0 .5 -1").queryArc(0, 1).map((e) => e.value);
    assert.deepEqual(values, [0, 0.5, -1]);
    assert.equal(typeof values[0], "number");
});

// --- Bjorklund ---

test("euclid: (3,8), (5,8), (7,16), (4,12) and the edges", () => {
    const x = (k, n, r) => euclidSteps(k, n, r).map((b) => (b ? "x" : ".")).join("");
    assert.equal(x(3, 8), "x..x..x.");
    assert.equal(x(5, 8), "x.xx.xx.");
    assert.equal(x(7, 16), "x..x.x.x..x.x.x.");
    assert.equal(x(4, 12), "x..x..x..x..");
    assert.equal(x(3, 8, 2), ".x..x.x.");
    assert.equal(x(3, 8, -1), ".x..x..x");
    assert.equal(x(0, 4), "....");
    assert.equal(x(4, 4), "xxxx");
    assert.equal(x(6, 4), "xxxx");
    assert.equal(x(-3, 8), ".xx.xx.x");
    assert.deepEqual(bjorklund(3, 0), []);
    // Always k onsets over n steps.
    for (let n = 1; n <= 24; ++n)
        for (let k = 0; k <= n; ++k)
            assert.equal(bjorklund(k, n).filter(Boolean).length, k);
});

// --- the rest of the grammar ---

test("nesting, alternation inside sequences, stacks inside brackets", () => {
    assert.deepEqual(show("[a b, c] d"), ["a [0, 1/4)", "c [0, 1/2)", "b [1/4, 1/2)", "d [1/2, 1)"]);
    assert.deepEqual(show("<a [b c]>", 1), ["b [1, 3/2)", "c [3/2, 2)"]);
    assert.deepEqual(show("<a b, c>", 1), ["b [1, 2)", "c [1, 2)"]);
    assert.deepEqual(show("<a <b c>>", 3), ["c [3, 4)"]);
    assert.deepEqual(show("[[a b] c]"), ["a [0, 1/4)", "b [1/4, 1/2)", "c [1/2, 1)"]);
});

test("patterned factors: a*<2 3>, a*[1 2], a(<3 5>,8)", () => {
    assert.equal(mini("a*<2 3>").onsets(0, 1).length, 2);
    assert.equal(mini("a*<2 3>").onsets(1, 2).length, 3);
    assert.deepEqual(show("a*[1 2]"), ["a [0, 1) part [0, 1/2)", "a [1/2, 1)"]);
    assert.equal(mini("a(<3 5>,8)").onsets(0, 1).length, 3);
    assert.equal(mini("a(<3 5>,8)").onsets(1, 2).length, 5);
    assert.deepEqual(show("a*1.5", 0), ["a [0, 2/3)", "a [2/3, 4/3) part [2/3, 1)"]);
});

test("modifiers combine left to right", () => {
    assert.deepEqual(show("a*2!2"), ["a [0, 1/4)", "a [1/4, 1/2)", "a [1/2, 3/4)", "a [3/4, 1)"]);
    assert.deepEqual(show("a!"), ["a [0, 1/2)", "a [1/2, 1)"]);
    assert.deepEqual(show("a ! b"), ["a [0, 1/3)", "a [1/3, 2/3)", "b [2/3, 1)"]);
    assert.deepEqual(show("a ! !"), ["a [0, 1/3)", "a [1/3, 2/3)", "a [2/3, 1)"]);
    assert.deepEqual(show("a@1.5 b@0.5"), ["a [0, 3/4)", "b [3/4, 1)"]);
    assert.deepEqual(show("[a b]@3 c"), ["a [0, 3/8)", "b [3/8, 3/4)", "c [3/4, 1)"]);
    assert.deepEqual(show("bd:1*2"), ["s:bd n:1 [0, 1/2)", "s:bd n:1 [1/2, 1)"]);
    assert.deepEqual(show("a(3,8)*2").length, 6);
    assert.deepEqual(show("~ a@3"), ["a [1/4, 1)"]);
});

test("words: sharps, flats, underscores, dots, other alphabets; empty strings are silence", () => {
    const values = mini("c#3 eb4 super_saw x.2 kölsch").queryArc(0, 1).map((e) => e.value);
    assert.deepEqual(values, ["c#3", "eb4", "super_saw", "x.2", "kölsch"]);
    assert.deepEqual(show(""), []);
    assert.deepEqual(show("   "), []);
    assert.deepEqual(show(" a  \n\t b "), ["a [0, 1/2)", "b [1/2, 1)"]);
    assert.deepEqual(show("~"), []);
});

test("source locations: each atom's range in the string, plus the literal's offset", () => {
    const loc = (src, offset = 0) => sortEvents(mini(src, offset).queryArc(0, 1)).map((e) => e.context.locations.map((l) => [l.start, l.end]));
    assert.deepEqual(loc("kick [sn hat]"), [[[0, 4]], [[6, 8]], [[9, 12]]]);
    assert.deepEqual(loc("kick [sn hat]", 100), [[[100, 104]], [[106, 108]], [[109, 112]]]);
    assert.deepEqual(loc("<a bb>*2"), [[[1, 2]], [[3, 5]]]);
    // From an editor string literal.
    const lit = { isMiniString: true, offset: 40, toString: () => "x y" };
    assert.deepEqual(loc(lit), [[[40, 41]], [[42, 43]]]);
    // "~" makes no event, so no location.
    assert.deepEqual(loc("a ~ b"), [[[0, 1]], [[4, 5]]]);
});

test("strings given to pattern functions are mini-notation once mini.js is loaded", () => {
    assert.deepEqual(seq("a b", "c").show(), ["a [0, 1/4)", "b [1/4, 1/2)", "c [1/2, 1)"]);
    assert.deepEqual(reify("a*2").show(), ["a [0, 1/2)", "a [1/2, 1)"]);
    // Patterned arguments as strings.
    assert.equal(mini("a").fast("<1 2>").onsets(1, 2).length, 2);
    assert.equal(mini("a b c d").fast("2").onsets(0, 1).length, 8);
});

// --- errors: a short message, the column inside the string and the buffer index ---

function error(src, offset = 0) {
    try {
        mini(src, offset);
    } catch (e) {
        assert.ok(e instanceof MiniError, `not a MiniError: ${e}`);
        assert.ok(e instanceof SyntaxError);
        return { message: e.message, column: e.column, index: e.index };
    }
    assert.fail(`no error for ${JSON.stringify(src)}`);
}

test("errors: unbalanced brackets", () => {
    assert.deepEqual(error("a b)"), { message: 'column 4: unexpected ")" - did you forget "("?', column: 4, index: 3 });
    assert.deepEqual(error("a ]"), { message: 'column 3: unexpected "]" - did you forget "["?', column: 3, index: 2 });
    assert.equal(error("a [b c").message, 'column 3: missing "]" for the "[" at column 3');
    assert.equal(error("<a b").message, 'column 1: missing ">" for the "<" at column 1');
    assert.equal(error("[a b>").message, 'column 5: expected "]" but found ">"');
    assert.equal(error("a b)", 20).index, 23);
});

test("errors: modifiers without what they need", () => {
    assert.equal(error("a*").message, 'column 2: "*" needs a number or a [pattern] after it');
    assert.equal(error("a* 2").message, 'column 4: no space allowed after "*"');
    assert.equal(error("a@").message, 'column 2: "@" needs a number after it (the step\'s length)');
    assert.equal(error("a!0").message, 'column 3: "!" needs a whole number of 1 or more, not 0');
    assert.equal(error("a?2").message, 'column 3: "?" needs a chance between 0 and 1, not 2');
    assert.equal(error("a(3)").message, 'column 4: the euclid rhythm needs (pulses, steps) - expected "," but found ")"');
    assert.equal(error("a(3,8").message, 'column 2: missing ")" for the "(" at column 2');
    assert.equal(error("kick:-").message, 'column 5: ":" needs a whole number (the sample number) or a name after it');
    assert.equal(error("! a").message, 'column 1: "!" on its own repeats the step before it, but there is none');
});

test("errors: stray characters and misplaced pieces", () => {
    assert.equal(error("a & b").message, 'column 3: unexpected "&"');
    assert.equal(error("3a").message, 'column 1: "3a" - a word can\'t start with a digit');
    assert.equal(error("a[b]").message, 'column 2: "[" needs a space before it');
    assert.equal(error(", a").message, 'column 1: nothing before ","');
    assert.equal(error("[]").message, 'column 2: nothing before "]"');
    assert.equal(error("a *2").message, 'column 3: unexpected "*"');
});

test("the random hash is deterministic and spread out", () => {
    assert.equal(randomAt(0, 0, F(1, 3)), randomAt(0, 0, F(2, 6)));
    assert.notEqual(randomAt(0, 0, 0), randomAt(1, 0, 0));
    assert.notEqual(randomAt(0, 0, 0), randomAt(0, 1, 0));
    const buckets = new Array(10).fill(0);
    for (let i = 0; i < 10000; ++i) {
        const r = randomAt(0, 5, F(i, 16));
        assert.ok(r >= 0 && r < 1);
        buckets[Math.floor(r * 10)]++;
    }
    for (const b of buckets)
        assert.ok(b > 850 && b < 1150, `bucket ${b}`);
});
