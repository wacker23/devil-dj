// Deterministic randomness (CODEBEAT_SPEC §5.3): a hash of (seed, salt, time) -> [0, 1). The same inputs always
// give the same number, on every machine and every run; no state is kept between queries.

import { F } from "./fraction.js";

// 32-bit integer mixing (a "lowbias32"-style finaliser: xor-shift and multiply rounds).
function mix32(h) {
    h ^= h >>> 16;
    h = Math.imul(h, 0x7feb352d);
    h ^= h >>> 15;
    h = Math.imul(h, 0x846ca68b);
    h ^= h >>> 16;
    return h >>> 0;
}

// A safe integer as two 32-bit halves folded into the hash.
function addNumber(h, x) {
    const lo = x % 0x100000000;
    const hi = Math.floor(x / 0x100000000);
    h = mix32(h ^ (lo >>> 0));
    return mix32(h ^ (hi >>> 0) ^ 0x9e3779b9);
}

// [0, 1) for (seed, salt, t); t is any time-like value (exact: 1/3 and 2/6 give the same number).
export function randomAt(seed, salt, t) {
    const time = F(t);
    let h = mix32((seed | 0) ^ 0x2545f491);
    h = addNumber(h, salt >>> 0);
    h = addNumber(h, time.n < 0 ? 2 ** 52 - time.n : time.n);
    h = addNumber(h, time.d);
    return h / 0x100000000;
}

// A text's 32-bit hash: salts that differ for different code but stay the same when the code moves.
export function hashString(text) {
    let h = 0x811c9dc5;
    const s = String(text);
    for (let i = 0; i < s.length; ++i)
        h = Math.imul(h ^ s.charCodeAt(i), 0x01000193);
    return h >>> 0;
}
