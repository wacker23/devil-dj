// The prelude's session rules, run the way the C++ evaluator runs user code (begin, the script, end / abort).
import { test } from "node:test";
import assert from "node:assert/strict";
import "../prelude.js";

const g = globalThis;

// One evaluation: returns the parsed __endEval reply, or throws what the code (or the test query) threw.
function run(code, now = 1) {
    g.__beginEval();
    try {
        g.__completion = (0, eval)(`{${code}\n}`);
        return parse(g.__endEval(now)); // the session tests swap at once; the P5-10 tests below wait for the line
    } catch (e) {
        g.__abortEval();
        throw e;
    }
}

function parse(reply) {
    return Object.fromEntries(reply.split("\n").map((line) => {
        const at = line.indexOf("=");
        return [line.slice(0, at), line.slice(at + 1)];
    }));
}

const playingNow = (b = 0, e = 1) => g.__show(b, e).split("\n").filter(Boolean);

test("the API is on globalThis", () => {
    for (const name of ["s", "n", "note", "gain", "seq", "cat", "stack", "mini", "sine", "rand", "run", "euclid",
        "play", "hush", "setbpm", "setcpb", "mute", "unmute", "deckA", "deckD", "master"])
        assert.ok(name in g, name);
});

test("the completion value is the pattern", () => {
    const r = run('s("bd sd")');
    assert.equal(r.playing, "1");
    assert.equal(r.changed, "1");
    assert.deepEqual(playingNow(), ["s:bd [0, 1/2)", "s:sd [1/2, 1)"]);
    // Statements before it are fine; a block's last value counts.
    run('const kick = s("bd*2");\nkick.gain(0.5)');
    assert.deepEqual(playingNow(), ["s:bd gain:0.5 [0, 1/2)", "s:bd gain:0.5 [1/2, 1)"]);
    // const / let can be used again next time (each run is its own block).
    run('const kick = s("hh");\nkick');
    assert.deepEqual(playingNow(), ["s:hh [0, 1)"]);
    // A bare mini-notation string plays too.
    run('"bd ~ sd"');
    assert.deepEqual(playingNow(), ["bd [0, 1/3)", "sd [2/3, 1)"]);
});

test("play() slots stack; named slots, mute / unmute", () => {
    const r = run('play("drums", s("bd*2"));\nplay("hats", s("hh*4"));\n"ignored completion"');
    assert.equal(r.slots, "drums,hats");
    assert.equal(playingNow().length, 6);
    // Unnamed play() calls each get a slot.
    assert.equal(run('play(s("bd")); play(s("sd"))').slots, "$0,$1");
    run('play("drums", s("bd*2")); play("hats", s("hh*4"))');
    let r2 = run('mute("hats")');
    assert.equal(r2.muted, "hats");
    assert.equal(r2.slots, "drums,hats");
    assert.deepEqual(playingNow(), ["s:bd [0, 1/2)", "s:bd [1/2, 1)"]);
    // A mute stays across evaluations until unmuted.
    run('play("drums", s("bd")); play("hats", s("hh*2"))');
    assert.deepEqual(playingNow(), ["s:bd [0, 1)"]);
    r2 = run('unmute("hats")');
    assert.equal(r2.muted, "");
    assert.equal(playingNow().length, 3);
    assert.throws(() => run("play(42)"), /play\(\) needs a pattern/);
});

test("hush() stops everything; play after it in the same code still plays", () => {
    run('s("bd*4")');
    let r = run("hush()");
    assert.equal(r.playing, "0");
    assert.deepEqual(playingNow(), []);
    r = run('hush(); play(s("cp"))');
    assert.equal(r.playing, "1");
    assert.deepEqual(playingNow(), ["s:cp [0, 1)"]);
});

test("code with no pattern keeps the last one playing", () => {
    run('s("bd sd")');
    const r = run("const x = 1 + 1;");
    assert.equal(r.changed, "0");
    assert.match(r.message, /the last one keeps playing/);
    assert.deepEqual(playingNow(), ["s:bd [0, 1/2)", "s:sd [1/2, 1)"]);
});

test("errors never replace what is playing", () => {
    run('s("bd sd")');
    assert.throws(() => run('s("bd [sd")'), /missing "]"/);
    assert.throws(() => run("nope.x"), ReferenceError);
    // An error that only shows when the pattern runs is caught by the test query.
    assert.throws(() => run('s("bd").gain("loud")'), /gain needs a number/);
    assert.throws(() => run('s("bd").every(2, (x) => x.oops())'), TypeError);
    assert.deepEqual(playingNow(), ["s:bd [0, 1/2)", "s:sd [1/2, 1)"]);
    // play() calls of a failed run are forgotten too.
    assert.throws(() => run('play("a", s("cp")); throw new Error("later")'), /later/);
    assert.deepEqual(playingNow(), ["s:bd [0, 1/2)", "s:sd [1/2, 1)"]);
    // Program functions refuse to run outside an evaluation.
    assert.throws(() => g.play(g.s("bd")), /only works in the code being run/);
});

test("setbpm / setcpb are reported and checked", () => {
    let r = run('setbpm(128); setcpb(3); s("bd")');
    assert.equal(r.bpm, "128");
    assert.equal(r.cpb, "3");
    assert.equal(g.__session.cpb, 3);
    r = run('s("bd")');
    assert.equal(r.bpm, "");
    assert.equal(r.cpb, "");
    assert.equal(g.__session.cpb, 3); // stays until changed
    assert.throws(() => run("setbpm(500)"), /20 to 300 BPM/);
    assert.throws(() => run("setcpb(0)"), /1 to 16 beats/);
    run("setcpb(4)");
});

test("DJ hooks: read-only views refreshed from outside", () => {
    g.__setDj({ decks: [{ bpm: 126, key: 8, keyName: "8A", beat: 64.5, bar: 16, phrase: 2, playing: true }], master: { bpm: 126, beat: 64.5 } });
    assert.equal(run('s("bd").fast(deckA.bpm > 120 ? 2 : 1)').playing, "1");
    assert.equal(playingNow().length, 2);
    assert.equal(g.deckA.keyName, "8A");
    assert.equal(g.deckB.bpm, 0);
    assert.equal(g.deckB.playing, false);
    assert.equal(g.master.bpm, 126);
    // Assigning does nothing (sloppy mode) or throws (strict): the hook is not changed.
    run('deckA = 5; s("bd")');
    assert.equal(g.deckA.bpm, 126);
    run('deckA.bpm = 1; s("bd")'); // a frozen view: the write is ignored
    assert.equal(g.deckA.bpm, 126);
    g.__setDj({});
});

test("user code may use the API's names for its own things", () => {
    assert.equal(run('const s = (x) => x; const note = 3; pure("a")').playing, "1");
    assert.deepEqual(playingNow(), ["a [0, 1)"]);
    assert.equal(typeof g.s, "function"); // the API is untouched
});

test("__query: onsets with their exact times, controls and source places", () => {
    run('s("bd [sd hh:2]").gain(0.5)');
    const lines = g.__query(0, 1, 1, 1).split("\n");
    assert.deepEqual(lines, [
        // Locations are inside the string literal for now (the editor adds the literal's offset: P5-15); a plain
        // number argument has none.
        "0/1 1/2|s=bd;gain=0.5|0-2",
        "1/2 3/4|s=sd;gain=0.5|4-6",
        "3/4 1/1|s=hh;n=2;gain=0.5|7-9",
    ]);
    // Only onsets inside the window; fragments of earlier events are left out.
    run('s("pad/2")');
    assert.equal(g.__query(1, 1, 2, 1), "");
    assert.equal(g.__query(0, 1, 1, 4), "0/1 2/1|s=pad|0-3");
    // Plain values become controls; strings are made safe for the format.
    run('mini("kick 3")');
    assert.deepEqual(g.__query(0, 1, 1, 1).split("\n").map((l) => l.split("|")[1]), ["s=kick", "n=3"]);
    run('s("a").bank(pure("x;y|z"))');
    assert.match(g.__query(0, 1, 1, 1), /bank=x_y_z/);
    // Windows that add up to a cycle give exactly its onsets.
    run('s("hh*8")');
    const pieces = [];
    for (let i = 0; i < 7; ++i)
        pieces.push(...g.__query(i, 7, i + 1, 7).split("\n").filter(Boolean));
    assert.equal(pieces.length, 8);
    run("hush()");
});

// --- P5-10 hot swap ---

// The onsets' begin times (as text) over [from, to) queried in windows of `step` cycles, eval-ing `code` (swap at
// the cycle line) when the cursor reaches `evalAt` (a window always ends there, so every step size evaluates at the
// same moment).
function playThrough(from, to, step, evalAt, code, now = 0) {
    const { Fraction } = g;
    const out = [];
    let replied = null;
    for (let t = from; t.lt(to);) {
        let next = t.add(step).min(to);
        if (t.lt(evalAt))
            next = next.min(evalAt);
        if (replied === null && t.gte(evalAt))
            replied = run(code, now);
        for (const line of g.__query(t.n, t.d, next.n, next.d).split("\n").filter(Boolean)) {
            const [times, fields] = line.split("|");
            out.push(`${fields.split(";")[0].slice(2)}@${times.split(" ")[0]}`);
        }
        t = next;
    }
    return { out, replied };
}

const Fr = (n, d = 1) => g.Fraction(n, d);

test("hot swap: the new pattern takes over exactly at the next cycle line, nothing doubled or missed", () => {
    run('s("a*3")'); // at once
    const { out, replied } = playThrough(Fr(0), Fr(4), Fr(1, 7), Fr(9, 7), 's("b*5")');
    assert.equal(replied.swap, "cycle");
    assert.deepEqual(out, [
        "a@0/1", "a@1/3", "a@2/3", "a@1/1", "a@4/3", "a@5/3",
        "b@2/1", "b@11/5", "b@12/5", "b@13/5", "b@14/5", "b@3/1", "b@16/5", "b@17/5", "b@18/5", "b@19/5",
    ]);
});

test("hot swap: any window sizes give the same onsets, also when a window spans the line", () => {
    const reference = (() => {
        run('s("a*3")');
        return playThrough(Fr(0), Fr(4), Fr(1, 7), Fr(9, 7), 's("b*5")').out;
    })();
    for (const step of [Fr(1, 3), Fr(3, 10), Fr(1, 96), Fr(5, 4), Fr(2)]) {
        run('s("a*3")');
        const { out } = playThrough(Fr(0), Fr(4), step, Fr(9, 7), 's("b*5")');
        assert.deepEqual(out, reference, `step ${step}`);
    }
});

test("hot swap: events of the new pattern that began before the line stay silent (no half notes)", () => {
    run('s("a*2")');
    // Evaluated at 1/4: the swap is at line 1. b lasts a cycle from x.5: the one over [0.5, 1.5) began before the
    // line and is not played; 1.5 is.
    const { out } = playThrough(Fr(0), Fr(4), Fr(1, 8), Fr(1, 4), 's("b").late(0.5)');
    assert.deepEqual(out, ["a@0/1", "a@1/2", "b@3/2", "b@5/2", "b@7/2"]);
});

test("hot swap: only the newest of several waiting patterns plays; errors swap nothing", () => {
    run('s("a")');
    g.__query(0, 1, 1, 2);
    run('s("b")', 0);
    run('s("c")', 0); // replaces b before the line
    assert.throws(() => run('s("d").gain("loud")', 0));
    assert.equal(g.__query(1, 2, 1, 1), ""); // [1/2, 1): a has no onset there
    assert.equal(g.__query(1, 1, 2, 1).split("|")[1], "s=c");
});

test("hot swap: 'now' (Ctrl+Shift+Enter) and hush() don't wait for the line", () => {
    run('s("a*4")');
    let { out, replied } = playThrough(Fr(0), Fr(1), Fr(1, 8), Fr(3, 8), 's("b*4")', 1);
    assert.equal(replied.swap, "now");
    assert.deepEqual(out, ["a@0/1", "a@1/4", "b@1/2", "b@3/4"]);
    run('s("a*4")');
    ({ out, replied } = playThrough(Fr(0), Fr(1), Fr(1, 8), Fr(3, 8), "hush()"));
    assert.equal(replied.swap, "now");
    assert.deepEqual(out, ["a@0/1", "a@1/4"]);
    // Code without a pattern changes nothing and swaps nothing.
    run('s("a")');
    assert.equal(run("const x = 1;", 0).swap, "");
    run("hush()");
});

test("__mini: editor strings act as strings and give atoms their place in the buffer (P5-15)", () => {
    // s("kick snare hat*2") as the pre-tokenizer writes it: the text starts at character 3.
    run('s(__mini("kick snare hat*2", 3))');
    const places = g.__query(0, 1, 1, 1).split("\n").map((l) => l.split("|")[2]);
    assert.deepEqual(places, ["3-7", "8-13", "14-17", "14-17"]);
    // Still a string for everything else.
    const m = g.__mini("a b", 10);
    assert.equal(m.split(" ").length, 2);
    assert.equal(m.toUpperCase(), "A B");
    assert.equal(`${m}!`, "a b!");
    assert.equal(m + "c", "a bc");
    assert.equal(m.length, 3);
    assert.ok(m.isMiniString);
    // Names, scales, numbers and control values given as editor strings.
    assert.equal(run('play(__mini("drums", 6), s(__mini("bd", 17)))').slots, "drums");
    run('n(__mini("0 2", 2)).scale(__mini("C:minor", 20)).gain(__mini("0.5", 40))');
    // The degree, the scale name's atom ("C") and the gain value, each with its own place.
    assert.match(g.__query(0, 1, 1, 1), /note=48;gain=0\.5\|2-3,20-21,40-43/);
    run("hush()");
});
