"""Fit-test coupons: the left 46 mm of tray and frame, cropped from the real model. --slim for the small set."""
import sys
import ws2812b_8x24_enclosure as m
from ws2812b_8x24_enclosure import box
tray, grid = m.build_tray(), m.build_grid()
if "--slim" in sys.argv:
    slab = box(15 - 8, -1, -1, 15 + 8, m.W + 1, 60)
    parts = (("test_ports", tray ^ box(-1, 10, -1, 26, 65, m.LEDGETOP + 0.5)), ("test_slab_tray", tray ^ slab), ("test_slab_grid", grid ^ slab))
else:
    keep = box(-1, -1, -1, 46.0, m.W + 1, 60)
    parts = (("test_tray", tray ^ keep), ("test_grid", grid ^ keep))
for name, part in parts:
    tm = m.to_trimesh(part); tm.export(f"{name}.stl"); lo, hi = tm.bounds
    print(f"{name}.stl pieces={len(part.decompose())} {hi[0]-lo[0]:.0f} x {hi[1]-lo[1]:.0f} x {hi[2]-lo[2]:.1f} mm ~{part.volume()/1000*1.24:.0f} g")
