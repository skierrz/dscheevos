#!/usr/bin/env python3
"""Find a NitroSDK game's OS tick counter (OSi_TickCounter, low word) in RAM dumps.

The SDK counts TIMER0 overflows (33.514 MHz / 64 / 65536 = 7.99 Hz) in a u64 in
the game's static RAM. Given main-RAM dumps taken FRAMES_AB and FRAMES_BC frames
apart, print the game-RAM addresses (0x02xxxxxx) whose word grew by the expected
amount over both spans.
Usage: find_tick.py A.bin B.bin C.bin FRAMES_AB FRAMES_BC
"""
import struct, sys

RATE = 33513982 / 64 / 65536 / (33513982 / 6 / 355 / 263)  # overflows per frame (59.83 Hz)

a, b, c = (open(p, "rb").read(0x400000) for p in sys.argv[1:4])
fab, fbc = int(sys.argv[4]), int(sys.argv[5])
n = 0x400000 // 4
wa, wb, wc = (struct.unpack("<%dI" % n, x) for x in (a, b, c))
want_ab, want_bc = fab * RATE, fbc * RATE
for i in range(n):
    dab, dbc = (wb[i] - wa[i]) & 0xFFFFFFFF, (wc[i] - wb[i]) & 0xFFFFFFFF
    if abs(dab - want_ab) <= 1.5 and abs(dbc - want_bc) <= 1.5 and wa[i] > 0:
        print("0x%08x" % (0x02000000 + i * 4))
