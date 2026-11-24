// A filter sweep - the cutoff follows a slow wave over four cycles
note("a1*16").s("saw").lpf(sine.range(200, 3500).slow(4)).resonance(0.5).gain(0.7)
