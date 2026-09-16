"""Fit-test coupons, cropped from the real model.

Earlier coupons cropped the full DEPTH of the panel, so every one dragged in the floor, the ledge and
the board cradles - 61 g to test a 5 mm seam. These crop in x AND y, so a coupon is just the piece of
wall that carries the feature. Same geometry, a twelfth of the filament.

  t_seam / g_seam    22 x 14 mm   the parting line: tongue, groove, and one back screw   ~5 g the pair
  t_touch            30 x 17 mm   TTP223 pocket, solder relief, locating dish            ~3 g
  t_knob             22 x 24 mm   EC11 pocket, bushing hole, nut recess, backstop        ~3 g
  t_port             30 x 34 mm   both USB-C ports and the two board cradles             ~8 g

Only reprint the one you changed. t_port in particular has been right since the first print - it only
needs redoing if the cradles or the port geometry move.

Run: python fit_test.py
"""
import ws2812b_8x24_enclosure as m
from ws2812b_8x24_enclosure import box

tray, grid = m.build_tray(), m.build_grid()
sx, _ = m.screw_sites()[0]                  # bottom-wall screw, away from anything else

# The grid's rim is solid only as far as the opening at OPEN_Y0; past that it is 1 mm cell walls, which
# on a 22 mm coupon are tall thin fins with almost no bed contact. So g_seam stops AT the opening: a
# plain bar carrying the groove and the pilot hole, which is all a seam test needs. The tray side keeps
# 2 mm of ledge past the wall for stability.
coupons = [
    ("t_seam",  tray, box(sx - 11, -1, -1, sx + 11, m.WALL + 2.0, 60)),
    ("g_seam",  grid, box(sx - 11, -1, -1, sx + 11, m.OPEN_Y0, 60)),
    ("t_touch", tray, box(m.TOUCHX[1] - 15, m.W - 17, -1, m.TOUCHX[1] + 15, m.W + 1, 60)),
    ("t_knob",  tray, box(m.KNOB_X - 16, m.W - 28, -1, m.KNOB_X + 16, m.W + 1, 60)),
    ("t_port",  tray, box(-1, 4.0, -1, 30.0, 38.0, 60)),
]

total = 0.0
for name, part, crop in coupons:
    cut = part ^ crop
    tm = m.to_trimesh(cut)
    tm.export(f"{name}.stl")
    lo, hi = tm.bounds
    g = cut.volume() / 1000 * 1.24
    total += g
    assert len(cut.decompose()) == 1, f"{name} came out in pieces - the crop severed something"
    print(f"{name}.stl  watertight={tm.is_watertight}  "
          f"{hi[0]-lo[0]:.0f} x {hi[1]-lo[1]:.0f} x {hi[2]-lo[2]:.1f} mm  ~{g:.1f} g")
print(f"total ~{total:.0f} g  (seam pair alone: "
      f"~{sum(c.volume() for n, p, cr in coupons[:2] for c in [p ^ cr]) / 1000 * 1.24:.1f} g)")
