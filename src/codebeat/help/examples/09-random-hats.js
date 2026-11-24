// Random but repeatable - drop some hats, the same ones every time (change the seed for others)
stack(
  s("bd ~ ~ bd ~ ~ bd ~"),
  s("~ cp"),
  s("hh*16").degradeBy(0.35).gain(rand.range(0.4, 0.9)).seed(3)
)
