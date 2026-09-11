"""Interference + clearance audit for the 8x24 enclosure."""
import math
import ws2812b_8x24_enclosure as m
from ws2812b_8x24_enclosure import box

tray = m.build_tray()
grid = m.build_grid().translate([0, 0, m.TOP])          # assembled position

def vol(a): return a.volume()
def report(name, v, ok):
    print(f"{'OK ' if ok else 'XX '} {name}: {v}")

report("tray/grid interference volume mm3", round(vol(tray ^ grid), 3), vol(tray ^ grid) < 0.01)

mats = None
for b in range(3):
    x0 = m.GX0 + b * m.PCBW
    pcb = box(x0, m.GY0, m.LEDGETOP, x0 + m.PCBW, m.GY0 + m.PCBH, m.TOP)
    mats = pcb if mats is None else mats + pcb
leds = None
for b in range(3):
    for i in range(8):
        for j in range(8):
            cx, cy = m.GX0 + b * m.PCBW + 4.5 + 8 * i, m.GY0 + 4.5 + 8 * j
            led = box(cx - 2.5, cy - 2.5, m.TOP, cx + 2.5, cy + 2.5, m.TOP + 1.6)
            leds = led if leds is None else leds + led
report("matrix PCBs vs tray interference mm3", round(vol(mats ^ tray), 3), vol(mats ^ tray) < 0.01)
report("matrix PCBs vs grid interference mm3", round(vol(mats ^ grid), 3), vol(mats ^ grid) < 0.01)
report("LED bodies vs grid walls interference mm3", round(vol(leds ^ grid), 3), vol(leds ^ grid) < 0.01)

def board(yc, bl, bw, bt, rec_l=7.3):
    xf = m.RECESS + m.OVERHANG
    pcb = box(xf, yc - bw / 2, m.FLOOR + m.PADH, xf + bl, yc + bw / 2, m.FLOOR + m.PADH + bt)
    zt = m.FLOOR + m.PADH + bt
    rec = box(m.RECESS, yc - 8.94 / 2, zt, m.RECESS + rec_l, yc + 8.94 / 2, zt + 3.26)
    return pcb + rec, zt
usb, zt_u = board(m.YU, m.USBL, m.USBW, m.USBT)
esp, zt_e = board(m.YE, m.ESPL, m.ESPW, m.ESPT)
report("USB-C breakout (+receptacle) vs tray mm3", round(vol(usb ^ tray), 3), vol(usb ^ tray) < 0.01)
report("ESP32 SuperMini (+receptacle) vs tray mm3", round(vol(esp ^ tray), 3), vol(esp ^ tray) < 0.01)
ttp = None
for cx in m.TOUCHX:
    t = box(cx - m.TTPL / 2, m.W - m.TSKIN - m.TTPT, m.TOP - m.TTPW - 0.1, cx + m.TTPL / 2, m.W - m.TSKIN, m.TOP - 0.1)
    ttp = t if ttp is None else ttp + t
report("TTP223 modules vs tray mm3", round(vol(ttp ^ tray), 3), vol(ttp ^ tray) < 0.01)
report("TTP223 modules vs matrix PCBs mm3", round(vol(ttp ^ mats), 3), vol(ttp ^ mats) < 0.01)
report("TTP223 modules vs grid frame mm3", round(vol(ttp ^ grid), 3), vol(ttp ^ grid) < 0.01)
if m.KNOB == "EC11_TOP":
    zc = 0.9 + m.EC_CLR + m.ENC_BODY / 2
    enc = box(m.KNOB_X - 6.2, m.W - m.WALL - m.EC_DEPTH, zc - 6.2, m.KNOB_X + 6.2, m.W - m.WALL, zc + 6.2)
    enc = enc + m.Manifold.cylinder(m.WALL + 1, 3.5, 3.5, 32).rotate([-90, 0, 0]).translate([m.KNOB_X, m.W - m.WALL - 0.5, zc])
else:
    ex, ey = m.ENC_XY
    enc = box(ex - 6.2, ey - 6.2, m.FLOOR, ex + 6.2, ey + 6.2, m.FLOOR + m.ENC_H)
    enc = enc + box(ex - m.KY_OFF, ey - m.KY_W / 2, m.FLOOR + m.ENC_H, ex - m.KY_OFF + m.KY_L, ey + m.KY_W / 2, m.FLOOR + m.ENC_H + m.KY_T)
report(f"{m.KNOB} body+shaft vs tray mm3", round(vol(enc ^ tray), 3), vol(enc ^ tray) < 0.01)
report(f"{m.KNOB} body vs matrix PCBs mm3", round(vol(enc ^ mats), 3), vol(enc ^ mats) < 0.01)
report(f"{m.KNOB} body vs grid frame mm3", round(vol(enc ^ grid), 3), vol(enc ^ grid) < 0.01)
report("small boards vs matrix PCBs mm3", round(vol((usb + esp) ^ mats), 3), vol((usb + esp) ^ mats) < 0.01)
# snap-specific: the posts must sit inside the pockets with clearance; beams must clear the posts at rest
report("beam clears post body at rest (mm)", round(m.B_IN - m.P_IN, 2), m.B_IN - m.P_IN >= 0.2)
report("beam top below barb underside (mm)", round(m.BZ[0] - m.BEAMH, 2), m.BZ[0] - m.BEAMH >= 0.19)
report("beam overlaps barb tip (mm)", round(m.BARB_TIP - m.B_IN, 2), m.BARB_TIP - m.B_IN >= 0.3)

print()
print(f"matrix row in pocket           : {m.CLR:.2f} mm per side")
print(f"grid rim over PCB edge         : {m.OVERLAP:.2f} mm; LED body starts {4.5 - 2.5 - m.OVERLAP:.2f} mm inside the rim edge")
print(f"under-matrix headroom          : USB receptacle top z={zt_u + 3.26:.2f}, matrix underside z={m.LEDGETOP:.2f}")
print(f"port hole vs receptacle shell  : hole {m.PORTW} x {m.PORTH}, shell 8.94 x 3.26 -> {(m.PORTW - 8.94) / 2:.2f} / {(m.PORTH - 3.26) / 2:.2f} mm each side")
print(f"receptacle face                : {m.RECESS:.1f} mm behind outer face; plug needs ~6.5 mm insertion, gets {6.5 - m.RECESS:.1f}")
print(f"power port above desk          : {m.YU - m.PORTW / 2:.1f} .. {m.YU + m.PORTW / 2:.1f} mm (centre {m.YU:.1f})")
print()
overlap = m.BARB_TIP - m.B_IN
strain = 3 * m.BEAMT * m.FLEX / (2 * m.BEAML ** 2) * 100
I = m.BEAMH * m.BEAMT ** 3 / 12
F = 3 * 3000 * I * m.FLEX / m.BEAML ** 3
print(f"snap: beam overlaps barb by {overlap:.2f} mm; deflects {m.FLEX} mm IN THE PRINT PLANE -> strain {strain:.2f}% (PLA ok < 1.5%)")
print(f"snap: spring force ~{F:.1f} N per site x {len(m.snap_sites())} sites; retention via 55-deg cam ~{F*1.4*len(m.snap_sites()):.0f} N total")
print(f"snap: post {m.POSTT} x {m.POSTW} rises {m.HOOKH} above the wall, rigid: root stress ~{F*(m.HOOKH+3)/(m.POSTW*m.POSTT**2/6):.2f} MPa vs layer bond ~20 MPa")
print(f"snap: pocket depth {m.HS} vs post height {m.HOOKH} -> {m.HS - m.HOOKH:.1f}; beam top {m.BEAMH} vs barb underside {m.BZ[0]} -> {m.BZ[0]-m.BEAMH:.1f}")
print(f"rim skin outside pocket        : {m.WALL + m.POCKET_OUT:.2f} mm; rim lip inside pocket {m.CLR + m.OVERLAP - m.POCKET_IN:.2f} mm")
print(f"diffuser: sheet 195.5 x 65.5 in pocket {m.GW + 1:.1f} x {m.GH + 1:.1f}, {m.DIFFT:.1f} deep, under {m.LIP} mm end lips; ~{math.sqrt(3 * 195 * 1.0 / 8):.0f} mm bow to fit")
