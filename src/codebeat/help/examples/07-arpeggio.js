// An arpeggio - up the chord, a fresh start each bar, echoes on top
n("0 2 4 7 9 7 4 2").scale("<C3:major F3:major G3:major A3:minor>")
  .s("tri").legato(0.6).delay(0.3)
  .off(0.125, (x) => x.add(12).gain(0.4))
