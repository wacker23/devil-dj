// A broken beat - steps with different lengths and a kick that moves around
stack(
  s("bd ~ [~ bd] ~ , ~ sd ~ [sd ~ ~ sd]").iter(4),
  s("hh*8").struct("x x ~ x x ~ x x"),
  s("oh").late(0.875).gain(0.5)
)
