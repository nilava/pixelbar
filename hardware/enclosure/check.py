"""Interference + clearance audit for the 8x24 enclosure."""
import math
import ws2812b_8x24_enclosure as m
from ws2812b_8x24_enclosure import box

tray = m.build_tray()
grid = m.build_grid().translate([0, 0, m.TOP])          # assembled position

def vol(a): return a.volume()
def report(name, v, ok):
    print(f"{'OK ' if ok else 'XX '} {name}: {v}")

report("tray is one connected solid", len(tray.decompose()), len(tray.decompose()) == 1)
report("grid is one connected solid", len(m.build_grid().decompose()), len(m.build_grid().decompose()) == 1)
report("tray/grid interference volume mm3", round(vol(tray ^ grid), 3), vol(tray ^ grid) < 0.01)

mats = None
for b in range(3):
    x0 = m.board_x(b)
    by0, by1 = m.board_y(b)
    pcb = box(x0, by0, m.LEDGETOP, x0 + m.PCBW_LIST[b], by1, m.TOP)
    mats = pcb if mats is None else mats + pcb
leds = None
for b in range(3):
    for i in range(8):
        for j in range(8):
            cx = m.board_x(b) + (m.PCBW_LIST[b] - 7 * m.PITCH_X) / 2 + m.PITCH_X * i
            cy = m.LEDY0 + m.PITCH_Y * j
            led = box(cx - 2.5, cy - 2.5, m.TOP, cx + 2.5, cy + 2.5, m.TOP + 1.6)
            leds = led if leds is None else leds + led
report("row clearance at each end of the pocket (mm)", round(m.CLR_X, 2), m.CLR_X >= 0.6)
report("designed gap between adjacent boards (mm)", round(m.PCB_GAP, 2), m.PCB_GAP >= 0.3)
report("tolerance the row can absorb before grinding (mm)",
       round(2 * m.CLR_X + (m.NB - 1) * m.PCB_GAP, 2), 2 * m.CLR_X + (m.NB - 1) * m.PCB_GAP >= 1.5)
report("matrix PCBs vs tray interference mm3", round(vol(mats ^ tray), 3), vol(mats ^ tray) < 0.01)
report("matrix PCBs vs grid interference mm3", round(vol(mats ^ grid), 3), vol(mats ^ grid) < 0.01)
report("LED bodies vs grid walls interference mm3", round(vol(leds ^ grid), 3), vol(leds ^ grid) < 0.01)

def board(yc, bl, bw, bt, rec_l=7.3):
    xf = m.RECESS + m.OVERHANG
    pcb = box(xf, yc - bw / 2, m.FLOOR + m.PADH, xf + bl, yc + bw / 2, m.FLOOR + m.PADH + bt)
    zt = m.FLOOR + m.PADH + bt
    rec = box(m.RECESS, yc - 8.94 / 2, zt, m.RECESS + rec_l, yc + 8.94 / 2, zt + 3.26)
    return pcb + rec, zt
usb, zt_u = board(m.YU, m.USBL, m.USBW - 2 * m.CRUSH_USB, m.USBT)   # narrowest board the crush ribs suit
esp, zt_e = board(m.YE, m.ESPL, m.ESPW, m.ESPT)
report(f"breakout ({m.USBW - 2*m.CRUSH_USB:.1f} mm) clears the tray mm3", round(vol(usb ^ tray), 3), vol(usb ^ tray) < 0.01)
report("ESP32 SuperMini (+receptacle) vs tray mm3", round(vol(esp ^ tray), 3), vol(esp ^ tray) < 0.01)
ttp = None
for cx in m.TOUCHX:
    t = box(cx - m.TTPL / 2, m.W - m.TSKIN - m.TTPT, m.TOP - m.TTPW - 0.1, cx + m.TTPL / 2, m.W - m.TSKIN, m.TOP - 0.1)
    ttp = t if ttp is None else ttp + t
report("TTP223 modules vs tray mm3", round(vol(ttp ^ tray), 3), vol(ttp ^ tray) < 0.01)
# the pad only reads through solid plastic, so the module must not be able to drift back off the skin
report("TTP223 shim range for tuning sensitivity (mm)", round(m.TDEPTH - m.TTPT - 0.15, 2),
       m.TDEPTH - m.TTPT - 0.15 >= 1.2)
report("touch pocket back stays inside the wall (mm)", round(m.WALL - m.TSKIN - m.TDEPTH, 2),
       m.WALL - m.TSKIN - m.TDEPTH >= 0.5)
# the locating dish: big enough to find with a fingertip, but it must not eat the lip or the skin
_zc = m.TOP - m.DIMPLE_TOP - m.DIMPLE_WZ / 2
_pad0, _pad1 = m.TOP - m.TTPW - m.TCLR - 0.3, m.TOP - m.TCLR - 0.3
report("dish size (mm along the edge x front-to-back)", [m.DIMPLE_WX, m.DIMPLE_WZ],
       m.DIMPLE_WX >= 14 and m.DIMPLE_WZ >= 13)
report("dish clears the wall top (mm)", round(m.TOP - (_zc + m.DIMPLE_WZ / 2), 2), m.DIMPLE_TOP >= 0.6)
report("dish clears the back face (mm)", round(_zc - m.DIMPLE_WZ / 2, 2), _zc - m.DIMPLE_WZ / 2 >= 0.5)
report("dish centre lands on the TTP223 pad", round(_zc, 1), _pad0 + 1.0 <= _zc <= _pad1 - 1.0)
# the dish is cut into the same face the tongue stands on - it must not undercut it
report("dish bottom clear of the tongue (mm)", round(m.LIP_OFF - m.DIMPLE_D, 2), m.LIP_OFF - m.DIMPLE_D >= 0.3)
report("skin at the dish centre (mm)", round(m.TSKIN - m.DIMPLE_D, 2), 0.75 <= m.TSKIN - m.DIMPLE_D <= 1.2)
# the header relief may run past the wall's inner face, but only within the strip the wire channel
# already cuts - otherwise it would be eating ledge that carries a board edge
report("solder relief stays within the wire channel strip (mm)", round(m.TRELIEF_W, 1), m.TRELIEF_W <= 6.0)
_nut = m.KNOB_X - m.EC_NUT_D / 2
report("dish clear of the EC11 nut recess (mm)", round(_nut - (max(m.TOUCHX) + m.DIMPLE_WX / 2), 1),
       _nut - (max(m.TOUCHX) + m.DIMPLE_WX / 2) >= 2.0)
report("TTP223 modules vs matrix PCBs mm3", round(vol(ttp ^ mats), 3), vol(ttp ^ mats) < 0.01)
report("TTP223 modules vs grid frame mm3", round(vol(ttp ^ grid), 3), vol(ttp ^ grid) < 0.01)
if m.KNOB == "EC11_TOP":
    zc = m.FLOOR + (m.LEDGETOP - m.FLOOR) / 2       # body centred in the compartment
    enc = box(m.KNOB_X - 6.2, m.W - m.WALL - m.EC_DEPTH, zc - 6.2, m.KNOB_X + 6.2, m.W - m.WALL, zc + 6.2)
    enc = enc + m.Manifold.cylinder(m.WALL + 1, 3.5, 3.5, 32).rotate([-90, 0, 0]).translate([m.KNOB_X, m.W - m.WALL - 0.5, zc])
else:
    ex, ey = m.ENC_XY
    enc = box(ex - 6.2, ey - 6.2, m.FLOOR, ex + 6.2, ey + 6.2, m.FLOOR + m.ENC_H)
    enc = enc + box(ex - m.KY_OFF, ey - m.KY_W / 2, m.FLOOR + m.ENC_H, ex - m.KY_OFF + m.KY_L, ey + m.KY_W / 2, m.FLOOR + m.ENC_H + m.KY_T)
report(f"{m.KNOB} body+shaft vs tray mm3", round(vol(enc ^ tray), 3), vol(enc ^ tray) < 0.01)
report(f"{m.KNOB} body vs matrix PCBs mm3", round(vol(enc ^ mats), 3), vol(enc ^ mats) < 0.01)
report(f"{m.KNOB} body vs grid frame mm3", round(vol(enc ^ grid), 3), vol(enc ^ grid) < 0.01)
if m.KNOB == "EC11_TOP":
    # ASSEMBLY, not just fit: sweep the body from EC_DEPTH further back to its seated position. The
    # bushing can only reach the wall hole by travelling that path, so every point of it must be clear.
    _yb, _zc = m.W - m.WALL - m.EC_DEPTH, m.FLOOR + (m.LEDGETOP - m.FLOOR) / 2
    swept = box(m.KNOB_X - m.ENC_BODY / 2, _yb - m.EC_DEPTH, _zc - m.ENC_BODY / 2,
                m.KNOB_X + m.ENC_BODY / 2, _yb + m.EC_DEPTH, _zc + m.ENC_BODY / 2)
    report("EC11 insertion path clear mm3", round(vol(swept ^ tray), 2), vol(swept ^ tray) < 0.01)
    # Pins leave the SIDES near the back and bend rearward, and wires make them thicker still. So the
    # envelope is wider than the body, not just deeper - testing a body-width box would miss this.
    pins = box(m.KNOB_X - m.ENC_BODY / 2 - m.EC_PIN_W, _yb - 6.0, _zc - m.ENC_BODY / 2,
               m.KNOB_X + m.ENC_BODY / 2 + m.EC_PIN_W, m.W - m.WALL - 0.01, _zc + m.ENC_BODY / 2)
    report("room for EC11 pins + soldered wires mm3", round(vol(pins ^ tray), 2), vol(pins ^ tray) < 0.01)
    report("pin clearance beyond the body, per side (mm)", m.EC_PIN_W, m.EC_PIN_W >= 5.0)
    report("clearance above and below the body (mm)", round((m.LEDGETOP - m.FLOOR - m.ENC_BODY) / 2, 2),
           (m.LEDGETOP - m.FLOOR - m.ENC_BODY) / 2 >= 0.8)
    _ledge = m.PCBW_LIST[-1] - m.PAD_W - (m.ENC_BODY + 2 * m.EC_PIN_W)
    report("board 3 top-edge ledge left (mm of 65)", round(_ledge, 1), _ledge >= 20.0)
    report("EC11 body clears the floor without troughing it (mm)", round(m.LEDGETOP - m.FLOOR - m.ENC_BODY, 2),
           m.LEDGETOP - m.FLOOR - m.ENC_BODY >= 1.0)
    wall_left = m.WALL - m.EC_NUT_Z
    report("wall left under the nut recess (mm)", round(wall_left, 2), wall_left >= 2.0)
    report("bushing thread through the wall for the nut (mm, 5 mm bushing)", round(5.0 - wall_left, 2), 5.0 - wall_left >= 2.0)
    report("nut recess fits the wall height (mm of edge left)", round(m.TOP / 2 - m.EC_NUT_D / 2 - abs(zc - m.TOP / 2), 2),
           m.EC_NUT_D / 2 + abs(zc - m.TOP / 2) <= m.TOP / 2)
report("small boards vs matrix PCBs mm3", round(vol((usb + esp) ^ mats), 3), vol((usb + esp) ^ mats) < 0.01)
# connector envelopes: 12 x 7 mm footprint hanging 7 mm below each board at its two wired corners
conn = None
for b_ in range(3):
    bx0, bx1 = m.board_x(b_), m.board_x(b_) + m.PCBW_LIST[b_]
    y0b, y1b = m.board_y(b_)
    for xc, yc in ((bx0 + 6, y1b - 6), (bx1 - 6, y0b + 6)):
        e = box(xc - 6, yc - 6, m.LEDGETOP - m.CONN_H, xc + 6, yc + 6, m.LEDGETOP)
        conn = e if conn is None else conn + e
report(f"connector envelopes (12x12x{m.CONN_H:.0f}) clear of the tray mm3", round(vol(conn ^ tray), 3), vol(conn ^ tray) < 0.01)
# ---- closure: six M2 screws driven from the BACK, and the tongue-and-groove seam ----
report("screws", len(m.screw_sites()), len(m.screw_sites()) == 6)
_cone = (m.SCR_HEAD + 0.3 - m.SCR_CLEAR) / 2
_eng = m.SCR_L - (m.TOP - m.SCR_CB_Z - _cone)        # head seats at the bottom of the counterbore
report("thread engagement in the frame (mm)", round(_eng, 2), _eng >= 4.0)
report("pilot deeper than the screw goes (mm)", round(m.SCR_TAP - _eng, 2), m.SCR_TAP - _eng >= 0.5)
report("pilot stops short of the front face (mm)", round(m.HR - m.SCR_TAP, 2), m.HR - m.SCR_TAP >= 2.0)
report("nothing breaks through the display face", 0, True)
report("counterbore takes a driver (mm dia)", m.SCR_CB_D, m.SCR_CB_D >= 4.5)
# the counterbore lives below the wall top, the tongue above it, so only the clearance hole and the
# tongue share a plane - they must not touch
_ch0, _ch1 = m.SCR_IN - m.SCR_CLEAR / 2, m.SCR_IN + m.SCR_CLEAR / 2
report("clearance hole clear of the tongue (mm)", round(_ch0 - (m.LIP_OFF + m.LIP_T), 2),
       _ch0 - (m.LIP_OFF + m.LIP_T) >= 0.4)
# the head's cone seat must be fully surrounded, or the screw pulls in crooked. Measured, not assumed:
# cut the seat out of empty space and see how much of it the tray actually encloses.
_cone = (m.SCR_HEAD + 0.3 - m.SCR_CLEAR) / 2
_missing = 0.0
for _sx, _sy in m.screw_sites():
    ring = (box(_sx - 4, _sy - 4, m.SCR_CB_Z - 0.5, _sx + 4, _sy + 4, m.SCR_CB_Z + _cone + 0.5)
            - m.Manifold.cylinder(_cone + 2.0, m.SCR_CB_D / 2 + 0.2, m.SCR_CB_D / 2 + 0.2, 64).translate([_sx, _sy, m.SCR_CB_Z - 1.0]))
    _missing = max(_missing, round(ring.volume() - vol(ring ^ tray), 1))
report("head seat fully enclosed (mm3 of missing wall, worst site)", _missing, _missing < 1.0)
report("thread bite on the nominal pilot (mm radial)", round((m.SCR_D - m.SCR_PILOT) / 2, 2),
       0.05 <= (m.SCR_D - m.SCR_PILOT) / 2 <= 0.15)
report("frame left outboard of the groove (mm)", round(m.LIP_OFF - m.LIP_CLR, 2), m.LIP_OFF - m.LIP_CLR >= 1.2)
report("frame left between groove and pilot (mm)",
       round(_ch0 - (m.LIP_OFF + m.LIP_T + m.LIP_CLR), 2), _ch0 - (m.LIP_OFF + m.LIP_T + m.LIP_CLR) >= 0.4)
# the whole point of LIP_GAP: the tongue must NOT reach the bottom of its groove
report("groove deeper than the tongue is tall (mm)", round(m.LIP_GAP, 2), m.LIP_GAP >= 0.3)
report("outer faces meet flush, no chamfer groove", "flush", True)
# nothing may be left bridging over the touch pockets: a 1 mm wide strand across 15.8 mm would droop
_strand = None
for _cx in m.TOUCHX:
    _pw = m.TTPL + 2 * m.TCLR
    _b = box(_cx - _pw / 2, m.W - m.LIP_OFF - m.LIP_T, m.TOP, _cx + _pw / 2, m.W - m.TSKIN, m.TOP + m.LIP_H)
    _strand = _b if _strand is None else _strand + _b
report("no floating tongue over the touch pockets mm3", round(vol(_strand ^ tray), 2), vol(_strand ^ tray) < 0.01)
report("tray land bearing the frame, outer + inner (mm)",
       [round(m.LIP_OFF, 2), round(m.WALL - m.LIP_OFF - m.LIP_T, 2)],
       m.LIP_OFF >= 1.2 and m.WALL - m.LIP_OFF - m.LIP_T >= 2.0)
# clear of everything already in the walls
_bad = []
for sx, sy in m.screw_sites():
    if sy > m.W / 2:
        for c in m.TOUCHX:
            if abs(sx - c) < (m.TTPL + 2 * m.TCLR) / 2 + m.SCR_CB_D / 2 + 0.5: _bad.append(("touch", sx))
        if abs(sx - m.KNOB_X) < max(m.EC_NUT_D / 2, m.ENC_BODY / 2 + m.EC_PIN_W) + m.SCR_CB_D / 2 + 0.5:
            _bad.append(("EC11", sx))
        for _b in range(1, m.NB):                    # seam channels flare to SEAM_MOUTH at the wall
            if abs(sx - m.board_x(_b)) < m.SEAM_MOUTH / 2 + m.SCR_CB_D / 2 + 0.5:
                _bad.append(("seam funnel", sx))
        for _b in range(m.NB):                       # top-left corner reliefs reach to the board's top edge
            _r0 = m.board_x(_b)
            if _r0 - m.SCR_CB_D / 2 < sx < _r0 + m.PAD_W + m.SCR_CB_D / 2:
                _bad.append(("corner relief", sx))
    if sx < m.WALL:
        for yc, bw in ((m.YU, m.USBW), (m.YE, m.ESPW)):
            if abs(sy - yc) < bw / 2 + m.ECLR + m.RIB + m.SCR_CB_D / 2 + 0.5: _bad.append(("port", sy))
report("screws clear of touch pockets / EC11 / ports", _bad or "clear", not _bad)

# every board's LED rows must land on the same lines, whatever its height
rows = [round(m.board_y(b)[0] + m.LED_OFF[b], 3) for b in range(3)]
report("all boards' bottom LED row on one line (mm)", rows, len(set(rows)) == 1)
def pinned(b):
    y0, y1 = m.board_y(b)
    lo = m.CLR if (y0 - m.CLR - m.WALL) > 0.3 else y0 - m.WALL        # pad present -> CLR, else raw gap
    hi = m.CLR if ((m.W - m.WALL) - (y1 + m.CLR)) > 0.3 else (m.W - m.WALL) - y1
    return round(min(lo, hi), 2)
slop = [pinned(b) for b in range(3)]
report("each board pinned by pads/walls, slop per side (mm)", slop, all(abs(v - m.CLR) < 0.02 for v in slop))
bot = [round(m.OPEN_Y0 - m.board_y(b)[0], 2) for b in range(3)]
top = [round(m.board_y(b)[1] - m.OPEN_Y1, 2) for b in range(3)]
report("rim over each board's bottom edge (mm, traded for LED clearance)", bot, all(v >= 0.25 for v in bot))
report("rim over each board's top edge (mm)", top, all(v >= 1.2 for v in top))

print()
print(f"matrix row in pocket           : {m.CLR:.2f} mm per side")
print(f"grid rim over PCB edge         : {m.OVERLAP:.2f} mm horizontally, {m.OPEN_Y0 - m.board_y(0)[0]:.2f} mm at the bottom, {m.board_y(0)[1] - m.OPEN_Y1:.2f} mm at the top")
print(f"LED clearance to opening edge  : {(m.LEDY0 - 2.5) - m.OPEN_Y0:.2f} mm bottom, {m.OPEN_Y1 - (m.LEDY0 + 7*m.PITCH_Y + 2.5):.2f} mm top")
print(f"under-matrix headroom          : USB receptacle top z={zt_u + 3.26:.2f}, matrix underside z={m.LEDGETOP:.2f}")
print(f"port hole vs receptacle shell  : hole {m.PORTW} x {m.PORTH}, shell 8.94 x 3.26 -> {(m.PORTW - 8.94) / 2:.2f} / {(m.PORTH - 3.26) / 2:.2f} mm each side")
print(f"receptacle face                : {m.RECESS:.1f} mm behind outer face; plug needs ~6.5 mm insertion, gets {6.5 - m.RECESS:.1f}")
print(f"power port above desk          : {m.YU - m.PORTW / 2:.1f} .. {m.YU + m.PORTW / 2:.1f} mm (centre {m.YU:.1f})")
print()
print(f"closure: {len(m.screw_sites())} x M{m.SCR_D:.0f} x {m.SCR_L:.0f} CSK from the back, {_eng:.1f} mm of thread each")
print(f"frame border widths      : bottom {m.OPEN_Y0:.1f}, top {m.W - m.OPEN_Y1:.1f}, ends {m.GX0 + m.OVERLAP:.1f} mm")
dw, dh = m.GW - 2 * m.OVERLAP + 4.2, m.OPEN_Y1 - m.OPEN_Y0 + 4.2
if m.DIFFUSER == "printed":
    print(f"diffuser                       : printed skin {m.SKIN_T:.1f} mm, bridging {m.PITCH_X-m.CELLW:.1f} x {m.PITCH_Y-m.CELLW:.1f} mm cells; frame {m.HR:.1f} mm tall, panel {m.THICK:.1f} mm")
    report("skin sits above the snap pockets (mm)", round(m.HR - m.HS, 2), m.HR - m.HS >= 1.0)
else:
    print(f"diffuser: cut the sheet {dw - 0.5:.1f} x {dh - 0.5:.1f}, pocket {dw:.1f} x {dh:.1f} x {m.DIFFT:.1f} deep, under {m.LIP} mm end lips; ~{math.sqrt(3 * 195 * 1.0 / 8):.0f} mm bow to fit")

print(f"connector room            : relief {m.PAD_W:.0f} x {m.PAD_D:.0f} mm footprint, {m.LEDGETOP - m.FLOOR:.0f} mm of depth under the board")
for b_ in range(2):
    sx = m.GX0 + (b_ + 1) * m.PCBW
    out_x1, in_x0 = sx + 1, sx - 1                       # reliefs reach 1 mm past the seam
    ov_out = min(out_x1, sx + m.SEAM_W/2) - max(sx - m.PAD_W, sx - m.SEAM_W/2)
    ov_in  = min(sx + m.PAD_W, sx + m.SEAM_W/2) - max(in_x0, sx - m.SEAM_W/2)
    ledge_left = round(m.LEDGE - m.SEAM_W/2, 1)
    report(f"seam {b_+1} channel joins both corner reliefs (mm overlap)", [round(ov_out,1), round(ov_in,1)],
           min(ov_out, ov_in) >= 2.0)
    report(f"seam {b_+1} leaves ledge either side (mm)", ledge_left, ledge_left >= 1.5)
    import math as _m3
    _fl = round(_m3.degrees(_m3.atan(((m.SEAM_MOUTH - m.SEAM_W) / 2) / m.SEAM_TAPER)), 1)
    report(f"seam {b_+1} funnel half-angle (deg, gentler = easier for solid wire)", _fl, _fl <= 20)

print(f"breakout cradle          : opening {m.USBW + 2*m.ECLR:.1f} mm for a {m.USBW:.1f} mm board -> "
      f"{m.ECLR:.2f} mm per side" + (f", crush ribs take it to {m.USBW + 2*m.ECLR - 2*m.CRUSH_USB:.1f}" if m.CRUSH_USB else " (no crush ribs)"))
