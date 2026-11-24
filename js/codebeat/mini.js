// Mini-notation (CODEBEAT_SPEC §4): a hand-written tokenizer and recursive-descent parser turning strings like
// "kick [~ snare]*2 hat(3,8)" into patterns. Every atom's event carries its source range (§4.3) so the editor can
// light up what is sounding. Installing this module makes every string given to a pattern function mini-notation.
//
//   stack    = sequence { "," sequence }
//   sequence = step { ws step }                      (a bare "!" step repeats the step before it)
//   step     = term { modifier }                     (modifiers attach with no space before them)
//   term     = atom | "~" | "-" | "[" stack "]" | "<" stack ">"
//   modifier = "*" factor | "/" factor | "!" [int] | "@" number | "?" [number] | "(" f "," f ["," f] ")" | ":" int
//   factor   = number | "[" stack "]" | "<" stack ">"

import { F } from "./fraction.js";
import { Event, combineContexts } from "./event.js";
import { Pattern, pure, silence, stack, slowcat, timecat, patterned, setStringParser, fastcat } from "./pattern.js";
import { euclidSteps } from "./euclid.js";
import { randomAt } from "./random.js";

// A parse error with its place: `index` is the offset in the editor buffer (the literal's offset added), `column`
// counts from 1 inside the string literal.
export class MiniError extends SyntaxError {
    constructor(message, column, index, length = 1) {
        super(`column ${column}: ${message}`);
        this.name = "MiniError";
        this.shortMessage = message;
        this.column = column;
        this.index = index;
        this.length = length;
    }
}

// --- tokens ---

const kSingle = new Set(["[", "]", "<", ">", ",", "*", "/", "!", "@", "?", "(", ")", ":", "~"]);
const isDigit = (c) => c >= "0" && c <= "9";
const isLetter = (c) => /\p{L}/u.test(c);
const isWordChar = (c) => isLetter(c) || isDigit(c) || c === "#" || c === "_" || c === ".";
const isSpace = (c) => c === " " || c === "\t" || c === "\n" || c === "\r";

function tokenize(text, fail) {
    const tokens = [];
    let i = 0;
    let space = true; // whitespace (or the start) before the token
    while (i < text.length) {
        const c = text[i];
        if (isSpace(c)) {
            space = true;
            ++i;
            continue;
        }
        const start = i;
        const startsNumber = isDigit(c) || (c === "." && isDigit(text[i + 1] ?? "")) ||
            (c === "-" && (isDigit(text[i + 1] ?? "") || (text[i + 1] === "." && isDigit(text[i + 2] ?? ""))));
        if (startsNumber) {
            if (c === "-")
                ++i;
            while (i < text.length && isDigit(text[i]))
                ++i;
            if (text[i] === "." && isDigit(text[i + 1] ?? "")) {
                ++i;
                while (i < text.length && isDigit(text[i]))
                    ++i;
            }
            if (i < text.length && isLetter(text[i]))
                fail(`"${text.slice(start, i + 1)}" - a word can't start with a digit`, start, i + 1 - start);
            tokens.push({ type: "number", text: text.slice(start, i), start, end: i, space });
        } else if (isLetter(c)) {
            while (i < text.length && isWordChar(text[i]))
                ++i;
            tokens.push({ type: "word", text: text.slice(start, i), start, end: i, space });
        } else if (c === "-") {
            ++i;
            tokens.push({ type: "~", text: "-", start, end: i, space });
        } else if (kSingle.has(c)) {
            ++i;
            tokens.push({ type: c, text: c, start, end: i, space });
        } else {
            fail(`unexpected "${c}"`, start);
        }
        space = false;
    }
    tokens.push({ type: "end", text: "", start: text.length, end: text.length, space: true });
    return tokens;
}

// --- parser ---

const kClosers = { "[": "]", "<": ">", "(": ")" };

class Parser {
    constructor(text, offset, seedSalt) {
        this.text = text;
        this.offset = offset;
        this.salt = seedSalt; // each "?" gets its own salt so two of them don't drop together
        this.questions = 0;
        this.tokens = tokenize(text, (m, at, len) => this.fail(m, at, len));
        this.pos = 0;
    }

    fail(message, at, length = 1) {
        throw new MiniError(message, at + 1, this.offset + at, length);
    }

    peek() {
        return this.tokens[this.pos];
    }

    next() {
        return this.tokens[this.pos++];
    }

    expect(type, opener) {
        const t = this.peek();
        if (t.type === type)
            return this.next();
        if (t.type === "end")
            this.fail(`missing "${type}" for the "${opener.text}" at column ${opener.start + 1}`, opener.start);
        this.fail(`expected "${type}" but found "${t.text}"`, t.start, t.text.length);
    }

    parseAll() {
        if (this.peek().type === "end")
            return silence;
        const p = this.parseStack(null);
        const t = this.peek();
        if (t.type !== "end")
            this.unexpected(t);
        return p;
    }

    unexpected(t) {
        const hints = { "]": ' - did you forget "["?', ">": ' - did you forget "<"?', ")": ' - did you forget "("?' };
        this.fail(`unexpected "${t.text}"${hints[t.type] ?? ""}`, t.start, t.text.length);
    }

    // sequence { "," sequence }: alternate = true inside "< >" (one step per cycle).
    parseStack(closer, alternate = false) {
        const seqs = [this.parseSequence(closer, alternate)];
        while (this.peek().type === ",") {
            this.next();
            seqs.push(this.parseSequence(closer, alternate));
        }
        return seqs.length === 1 ? seqs[0] : stack(...seqs);
    }

    parseSequence(closer, alternate) {
        const steps = [];
        for (;;) {
            const t = this.peek();
            // Any closing bracket ends the sequence: its own closer, or a wrong one the opener then reports.
            if (t.type === "end" || t.type === "," || t.type === "]" || t.type === ">" || t.type === ")")
                break;
            if (t.type === "!" && t.space) {
                // A bare "!": the step before it once more.
                this.next();
                if (steps.length === 0)
                    this.fail('"!" on its own repeats the step before it, but there is none', t.start);
                steps.push(steps[steps.length - 1]);
                continue;
            }
            const startsStep = ["word", "number", "~", "[", "<"].includes(t.type);
            if (!t.space && steps.length > 0 && startsStep)
                this.fail(`"${t.text}" needs a space before it`, t.start, t.text.length);
            steps.push(...this.parseStep());
        }
        if (steps.length === 0) {
            const t = this.peek();
            if (t.type === "," || (closer !== null && t.type === closer))
                this.fail(`nothing before "${t.text}"`, t.start);
            return silence;
        }
        if (alternate)
            return slowcat(...steps.map((s) => s.pat));
        return steps.length === 1 && steps[0].weight.eq(1) ? steps[0].pat : timecat(...steps.map((s) => [s.weight, s.pat]));
    }

    // term { modifier } -> one or more steps ("!3" makes three).
    parseStep() {
        let pat = this.parseTerm();
        let weight = F(1);
        let copies = 1;
        for (;;) {
            const t = this.peek();
            if (t.space)
                break;
            if (t.type === "*") {
                this.next();
                const f = this.parseFactor(t);
                pat = pat.fast(f);
            } else if (t.type === "/") {
                this.next();
                const f = this.parseFactor(t);
                pat = pat.slow(f);
            } else if (t.type === "!") {
                this.next();
                const n = this.peek();
                if (n.type === "number" && !n.space) {
                    this.next();
                    const k = Number(n.text);
                    if (!Number.isInteger(k) || k < 1)
                        this.fail(`"!" needs a whole number of 1 or more, not ${n.text}`, n.start, n.text.length);
                    copies *= k;
                } else {
                    copies *= 2;
                }
            } else if (t.type === "@") {
                this.next();
                const n = this.peek();
                if (n.type !== "number" || n.space)
                    this.fail('"@" needs a number after it (the step\'s length)', t.start);
                this.next();
                weight = F(n.text);
                if (weight.n < 0)
                    this.fail(`"@" needs a length of 0 or more, not ${n.text}`, n.start, n.text.length);
            } else if (t.type === "?") {
                this.next();
                let p = 0.5;
                const n = this.peek();
                if (n.type === "number" && !n.space) {
                    this.next();
                    p = Number(n.text);
                    if (!(p >= 0 && p <= 1))
                        this.fail(`"?" needs a chance between 0 and 1, not ${n.text}`, n.start, n.text.length);
                }
                pat = degradeWith(pat, p, this.salt + this.questions++);
            } else if (t.type === "(") {
                this.next();
                const k = this.parseFactor(t);
                this.expectComma(t);
                const n = this.parseFactor(t);
                let r = 0;
                if (this.peek().type === ",") {
                    this.next();
                    r = this.parseFactor(t);
                }
                this.expect(")", t);
                pat = euclidOf(pat, k, n, r);
            } else if (t.type === ":") {
                this.next();
                const n = this.peek();
                if (n.type === "word" && !n.space) {
                    // "c:minor" (a scale), "kit:name": one word joined by ":".
                    this.next();
                    pat = pat.fmap((v) => `${v}:${n.text}`);
                    continue;
                }
                if (n.type !== "number" || n.space || !/^\d+$/.test(n.text))
                    this.fail('":" needs a whole number (the sample number) or a name after it', t.start);
                this.next();
                const index = Number(n.text);
                pat = pat.fmap((v) => (typeof v === "string" ? { s: v, n: index } : this.fail('":" goes after a sound name', t.start)));
            } else {
                break;
            }
        }
        const step = { pat, weight };
        return new Array(copies).fill(step);
    }

    expectComma(opener) {
        const t = this.peek();
        if (t.type === ",") {
            this.next();
            return;
        }
        this.fail(`the euclid rhythm needs (pulses, steps) - expected "," but found "${t.text || "the end"}"`, t.type === "end" ? opener.start : t.start);
    }

    parseTerm() {
        const t = this.next();
        switch (t.type) {
            case "word":
                return atom(t.text, this.offset + t.start, this.offset + t.end);
            case "number":
                return atom(Number(t.text), this.offset + t.start, this.offset + t.end);
            case "~":
                return silence;
            case "[": {
                const p = this.parseStack("]");
                this.expect("]", t);
                return p;
            }
            case "<": {
                const p = this.parseStack(">", true);
                this.expect(">", t);
                return p;
            }
            case "end":
                return this.fail("unexpected end", t.start);
            default:
                return this.unexpected(t);
        }
    }

    // A number (exact) or a bracketed pattern of numbers.
    parseFactor(after) {
        const t = this.peek();
        if (t.type === "number" && !t.space) {
            this.next();
            return F(t.text);
        }
        if ((t.type === "[" || t.type === "<") && !t.space) {
            this.next();
            const p = this.parseStack(kClosers[t.type], t.type === "<");
            this.expect(kClosers[t.type], t);
            return p;
        }
        if (t.type === "number" || t.type === "[" || t.type === "<")
            this.fail(`no space allowed after "${after.text}"`, t.start);
        return this.fail(`"${after.text}" needs a number or a [pattern] after it`, after.start);
    }
}

// An atom: its value once per cycle, with its source range.
function atom(value, start, end) {
    const context = Object.freeze({ locations: Object.freeze([Object.freeze({ start, end })]) });
    return new Pattern((state) =>
        state.span.spanCycles().map((part) => new Event(part.wholeCycle(), part, value, context)),
    );
}

// Events dropped with probability p, decided by the seeded hash at each event's start (the same every time).
export function degradeWith(pat, p, salt) {
    return new Pattern((state) => {
        const seed = state.controls.seed ?? 0;
        return pat.query(state).filter((e) => randomAt(seed, salt, e.wholeOrPart().begin) >= p);
    });
}

function euclidOf(pat, k, n, r) {
    const asInt = (x, what) => {
        const v = Number(x);
        if (!Number.isInteger(v))
            throw new RangeError(`euclid ${what} must be a whole number, not ${x}`);
        return v;
    };
    return patterned(k, (kk) =>
        patterned(n, (nn) =>
            patterned(r, (rr) => {
                const steps = euclidSteps(asInt(kk, "pulses"), asInt(nn, "steps"), asInt(rr, "rotation"));
                return steps.length === 0 ? silence : fastcat(...steps.map((on) => (on ? pat : silence)));
            }),
        ),
    );
}

// A string literal from the editor (P5-15, CODEBEAT_SPEC §8.3): the pre-tokenizer rewrites "..." in argument
// position to __mini("...", offset). It is a real String (every string method works) that also knows where its
// text starts in the buffer, so its atoms' places are the buffer's.
export class MiniString extends String {
    constructor(text, offset) {
        super(text);
        this.offset = Number(offset) || 0;
    }

    get isMiniString() {
        return true;
    }
}

// --- entry point ---

// The pattern for a mini-notation string. `offset` is where the string's text starts in the editor buffer (0 when
// it isn't from the editor); an editor string literal object ({ isMiniString, toString(), offset }) works too.
export function mini(source, offset = 0) {
    let text = source;
    if (source !== null && typeof source === "object" && source.isMiniString === true) {
        text = source.toString();
        offset = source.offset ?? offset;
    }
    if (typeof text !== "string")
        throw new TypeError(`mini() needs a string, got ${typeof text}`);
    // The "?" salts come from the text itself (not its place), so moving code around the buffer keeps its randomness
    // while two different strings don't drop their events together.
    let salt = 0;
    for (let i = 0; i < text.length; ++i)
        salt = (Math.imul(salt, 31) + text.charCodeAt(i)) >>> 0;
    return new Parser(text, offset, salt).parseAll();
}

// Strings given to any pattern function are mini-notation from now on.
setStringParser(mini);

export { combineContexts, pure };
