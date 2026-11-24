// Euclidean rhythms: k onsets spread as evenly as possible over n steps (Bjorklund's algorithm, written from its
// description: repeatedly pair the onset groups with the remainder groups until at most one remainder is left).

export function bjorklund(k, n) {
    if (!Number.isInteger(k) || !Number.isInteger(n))
        throw new RangeError(`Euclid needs whole numbers, got (${k},${n})`);
    if (n <= 0)
        return [];
    if (k < 0)
        return bjorklund(-k, n).map((x) => !x); // negative pulses: the rests become the onsets
    if (k === 0)
        return new Array(n).fill(false);
    if (k >= n)
        return new Array(n).fill(true);
    let a = Array.from({ length: k }, () => [true]);
    let b = Array.from({ length: n - k }, () => [false]);
    while (b.length > 1) {
        const m = Math.min(a.length, b.length);
        const paired = [];
        for (let i = 0; i < m; ++i)
            paired.push(a[i].concat(b[i]));
        const rest = a.length > m ? a.slice(m) : b.slice(m);
        a = paired;
        b = rest;
    }
    return a.flat().concat(b.flat());
}

// Rotated left by r steps (r may be negative or larger than n).
export function rotateLeft(steps, r) {
    const n = steps.length;
    if (n === 0)
        return steps;
    const s = ((r % n) + n) % n;
    return steps.slice(s).concat(steps.slice(0, s));
}

export function euclidSteps(k, n, r = 0) {
    return rotateLeft(bjorklund(k, n), r);
}
