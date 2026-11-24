// Every fourth bar something changes - a fill at the end of each phrase
stack(
  s("bd*4").every(4, (x) => x.ply(2)),
  s("~ sd ~ sd").every(4, (x) => x.fast(2)),
  s("hh*8")
)
