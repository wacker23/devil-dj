// Acid - a squelchy saw line, the filter opening and closing
note("c2 c2 c3 c2 eb2 c2 g2 c2").s("saw").legato(0.5)
  .lpf(perlin.range(300, 2500)).resonance(0.8).decay(0.15).sustain(0.2)
