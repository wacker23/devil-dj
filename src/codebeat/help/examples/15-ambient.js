// Ambient - long notes, slow changes, lots of room
stack(
  note("<a2 f2 c3 g2>").s("subbass").legato(1).release(2),
  n("<[0,4] [2,6] [4,7] [1,5]>").scale("A3:minor").s("supersaw")
    .attack(1).release(3).lpf(900).room(0.9).gain(0.4).slow(2),
  s("sh*8").gain(perlin.range(0, 0.25)).pan(sine)
)
