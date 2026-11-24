// Scales for scale("C:minor") (CODEBEAT_SPEC §5.4): semitone steps above the root, one octave.

import { noteNameToMidi } from "./controls.js";

export const kScales = {
    major: [0, 2, 4, 5, 7, 9, 11],
    minor: [0, 2, 3, 5, 7, 8, 10],
    dorian: [0, 2, 3, 5, 7, 9, 10],
    phrygian: [0, 1, 3, 5, 7, 8, 10],
    lydian: [0, 2, 4, 6, 7, 9, 11],
    mixolydian: [0, 2, 4, 5, 7, 9, 10],
    locrian: [0, 1, 3, 5, 6, 8, 10],
    harmonicMinor: [0, 2, 3, 5, 7, 8, 11],
    melodicMinor: [0, 2, 3, 5, 7, 9, 11],
    majPent: [0, 2, 4, 7, 9],
    minPent: [0, 3, 5, 7, 10],
    blues: [0, 3, 5, 6, 7, 10],
    chromatic: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
};

// "C:minor", "f#4:dorian", "minor" (root C3). Names ignore case: "harmonicminor" works.
export function parseScale(text) {
    const s = String(text).trim();
    const [rootText, nameText] = s.includes(":") ? s.split(":") : ["c", s];
    const root = noteNameToMidi(rootText);
    if (root === null)
        throw new RangeError(`scale: "${rootText}" isn't a root note (use c, f#, eb, a4 ...)`);
    const key = Object.keys(kScales).find((k) => k.toLowerCase() === String(nameText).toLowerCase());
    if (key === undefined)
        throw new RangeError(`scale: no scale called "${nameText}" (there are: ${Object.keys(kScales).join(", ")})`);
    return { root, steps: kScales[key], name: key };
}

// Scale degree (0 = the root; negative and above-the-octave degrees wrap into octaves) -> MIDI note.
export function degreeToNote(scale, degree) {
    const len = scale.steps.length;
    const whole = Math.floor(degree);
    const octave = Math.floor(whole / len);
    const index = whole - octave * len;
    // A fractional degree (e.g. 2.5) adds that fraction of a semitone: handy for slides.
    return scale.root + 12 * octave + scale.steps[index] + (degree - whole);
}
