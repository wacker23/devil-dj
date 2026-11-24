// Follow the DJ - half time until deck A plays, then full time; a fill every fourth bar
s("bd*4, ~ cp, hh*8")
  .fast(deckA.playing ? 1 : 0.5)
  .every(4, (x) => x.ply(2))
