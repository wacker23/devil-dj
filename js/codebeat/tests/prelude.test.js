// The prelude's session rules, run the way the C++ evaluator runs user code (begin, the script, end / abort).
import { test } from "node:test";
import assert from "node:assert/strict";
import "../prelude.js";

const g = globalThis;

// One evaluation: returns the parsed __endEval reply, or throws what the code (or the test query) threw.
function run(code) {
    g.__beginEval();
    try {
        g.__completion = (0, eval)(`{${code}\n}`);
        return parse(g.__endEval());
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
