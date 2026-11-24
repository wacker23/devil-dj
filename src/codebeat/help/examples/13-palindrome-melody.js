// A melody that answers itself - forwards one bar, backwards the next
n("0 2 3 5 7 5 3 [2 0]").scale("D4:dorian").s("sine")
  .palindrome().release(0.3).room(0.4).gain(0.7)
