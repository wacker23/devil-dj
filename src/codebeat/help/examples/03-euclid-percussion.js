// Euclid percussion - three rhythms spread evenly, each its own length
stack(
  s("bd(3,8)"),
  s("rim(5,8,2)").sometimes((x) => x.speed(2)),
  s("cb(2,8,3)").gain(0.5),
  s("hh*16").gain(saw.range(0.2, 0.7))
)
