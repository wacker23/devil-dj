// Named parts you can mute live - add mute("bass") and press Ctrl+Enter
play("drums", s("bd*4, ~ cp, hh*8"))
play("bass", n("0 0 3 5").scale("E1:minor").s("subbass"))
play("keys", note("<[e3,g3,b3] [c3,e3,g3]>").s("saw").lpf(1100).room(0.3).gain(0.5))
