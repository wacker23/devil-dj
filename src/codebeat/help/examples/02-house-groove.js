// House groove - open hats on the off-beat, a shuffling shaker
stack(
  s("bd*4"),
  s("~ cp ~ cp").room(0.2),
  s("[~ oh]*4").cut(1).gain(0.6),
  s("sh*16").gain(sine.range(0.2, 0.5))
)
