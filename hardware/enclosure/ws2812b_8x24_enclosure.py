#!/usr/bin/env python3
"""
Snap-fit desk enclosure (stands upright on its front edge) joining three 8x8 WS2812B
matrices (65 x 65 mm, 8 mm pitch) into one 8x24 panel. Electronics sit on the back
under board 1 (ESP32-C3 SuperMini, 7Semi USB-C breakout), touch modules stand in the
top wall, an MPU-6050 under board 2, a bare EC11 behind the top wall at the right.

Closure (print-friendly): the TRAY carries 11 rigid posts that rise from the wall and
never bend. The FRAME carries, inside each pocket, a thin horizontal spring beam that
flexes sideways IN THE PRINT PLANE to let the post's barb pass, then springs back under
it. Bending loads the plastic along its layers, not across them. Pry slots (back face
and right end) take a flat screwdriver; the barb's 55-degree underside cams the beam
aside under firm pressure so it opens without breaking anything.

Outputs (print flat as exported, no supports):
    tray.stl   - matrices, electronics, posts
    grid.stl   - 8x24 light grid, diffuser pocket with end lips, snap pockets with beams

Requires:  pip install manifold3d numpy trimesh
Run:       python ws2812b_8x24_enclosure.py
"""
import numpy as np
import trimesh
from manifold3d import Manifold

# ------------------------------ PARAMETERS (mm) ------------------------------
PCBW, PCBH, PCBT = 65.0, 65.0, 1.6     # one 8x8 matrix
NB = 3                                  # boards in a row -> 8 x 24
PITCH = 8.0                             # LED pitch
WALL = 5.0                              # tray wall (posts rise from it)
CLR = 0.3                               # clearance around matrix row, per side
FLOOR = 2.0                             # tray floor
BACKCLR = 12.0                          # compartment height: lets a bare EC11 (12.4 body) lie behind the top wall
LEDGE = 5.0                             # ledge supporting matrix PCB edges
CELLW, CELLH = 1.0, 8.0                 # grid wall thickness / height above PCB
DIFFT = 1.3                             # diffuser pocket: 1 mm clear sheet + vinyl + clearance
LIP = 1.5                               # lips over the diffuser at both ends (bow sheet to fit)
LIPT = 1.5                              # lip thickness
OVERLAP = 1.6                           # grid rim overlaps the PCB edge by this (leaves a 1.0 mm lip inside the snap pockets)

# electronics (under board 1, ports through the left end wall)
ESPL, ESPW, ESPT = 22.5, 18.0, 1.0      # ESP32-C3 SuperMini
USBL, USBW, USBT = 21.0, 16.0, 1.6      # 7Semi USB-C breakout
OVERHANG = 1.0                          # receptacle sticks out past its PCB edge
RECESS = 0.3                            # receptacle face behind the outer wall face
SKIN = 1.2                              # wall thickness left at the port cutouts (wall is pocketed from inside)
PORTW, PORTH, PORTR = 9.8, 4.2, 1.4     # USB-C cutout, rounded rectangle (receptacle shell is 8.94 x 3.26)
RIB, PADH, ECLR = 1.5, 1.0, 0.3         # cradle rib, pad height, board clearance

# touch zones: HW-763 TTP223 modules stand in pockets in the top wall, TOUCH pad against a thin skin
TTPL, TTPW, TTPT = 15.0, 11.0, 3.2      # module: long side, short side (vertical in pocket), thickness incl. parts
TSKIN, TCLR = 1.5, 0.4                  # wall left in front of the pad, clearance around the module
DIMPLE_R, DIMPLE_D = 12.0, 0.4          # locating dimple on the top edge: sphere radius, depth (6 mm dish)
MPUL, MPUW = 21.2, 15.6                 # GY-521 MPU-6050 module footprint
FENCE_T, FENCE_H, FCLR = 1.2, 3.0, 0.3  # module fences on the compartment floor

# knob: "EC11_TOP" = bare EC11 behind the top wall, shaft out of the top edge, nut under the knob
#       "KY040_BACK" = KY-040 module flat under board 3, shaft through the back
KNOB = "EC11_TOP"
KNOB_X = 176.0                          # shaft position along the top edge
EC_CLR = 0.4                            # clearance around the EC11 body in its pocket
EC_DEPTH = 7.0                          # EC11 body depth behind the wall (without bushing)
ENC_BODY, ENC_BUSH, ENC_H = 12.4, 7.0, 7.0
ENC_XY = (170.0, 50.0)                  # KY-040 shaft centre on the back (KY040_BACK only)
KY_L, KY_W, KY_T, KY_OFF = 32.0, 19.0, 1.6, 10.0

# snap fit: rigid posts on the tray, horizontal spring beams in the frame pockets
POSTW = 10.0                            # post width along the wall
POSTT = 1.6                             # post thickness across the wall
POST_OUT = 3.3                          # post outer face, inward from the wall's inner face (so 1.7 mm of wall stays outside it)
HOOKH = 6.5                             # post rises this far above the tray wall top
BARB = 0.6                              # barb protrusion, pointing inward
BEAMT = 1.4                             # spring beam thickness (across the wall)
BEAMH = 2.1                             # spring beam height (frame z 0..BEAMH), sits under the barb
BEAML = 14.0                            # spring beam free length (cantilever, anchored at one end)
FLEX = 0.6                              # beam deflection to let the barb pass
PRYW, PRYD, PRYH = 14.0, 2.0, 2.5       # pry slot width, depth into wall, height

# ------------------------------ derived --------------------------------------
L = 2 * WALL + NB * PCBW + 2 * CLR
W = 2 * WALL + PCBH + 2 * CLR
LEDGETOP = FLOOR + BACKCLR
TOP = LEDGETOP + PCBT                                # tray wall top = PCB top
GX0, GY0 = WALL + CLR, WALL + CLR                    # LED-area / PCB origin
GW, GH = NB * PCBW, PCBH
HR = CELLH + DIFFT + LIPT                            # frame rim height
THICK = TOP + HR                                     # assembled panel thickness
PORT_Y = 21.0                                        # power port centre above the desk (right-angle plug clears)
YU = PORT_Y
YE = W - GY0 - LEDGE - RIB - ECLR - ESPW / 2         # ESP32 centre y
TOUCHX = [55.0, L / 2, L - 55.0]
MPU_XY = (L / 2, 30.0)
# snap geometry in n = distance measured INWARD from the wall's inner face (negative = inside the wall)
P_OUT, P_IN = -POST_OUT, -POST_OUT + POSTT           # post body, e.g. [-3.3, -1.7]
BARB_TIP = P_IN + BARB                               # -1.1
B_IN = BARB_TIP - 0.4                                # beam outer face 0.4 under the barb tip, 0.2 clear of the post body -> beam [-1.5, -0.1]
B_OUT = B_IN + BEAMT
POCKET_OUT = P_OUT - 0.3                             # frame pocket outer boundary
POCKET_IN = B_OUT + FLEX + 0.4                       # frame pocket inner boundary (room for the beam to flex)
HS = HOOKH + 0.5                                     # pocket depth in the frame
BZ = (2.3, 3.0, 4.0, 4.5)                            # barb profile in frame z: 55-deg underside 2.3..3.0, flat 3.0..4.0, ramp 4.0..4.5


def box(x0, y0, z0, x1, y1, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def hull(points):
    return Manifold.hull_points([list(map(float, p)) for p in points])


def port_hole(yc, zc):
    """Rounded-rectangle USB-C cutout through the left skin, centred on (yc, zc), running along x."""
    r = PORTR
    c = Manifold.cylinder(SKIN + 2, r, r, 32).rotate([0, 90, 0]).translate([-1, 0, 0])
    corners = None
    for dy in (-(PORTW / 2 - r), PORTW / 2 - r):
        for dz in (-(PORTH / 2 - r), PORTH / 2 - r):
            k = c.translate([0, yc + dy, zc + dz])
            corners = k if corners is None else corners + k
    return corners.hull()


def fence(cx, cy, l, w, gap=5.0):
    """Low rectangular fence on the floor holding an l x w module, wire gap on its +y side."""
    li, wi = l + 2 * FCLR, w + 2 * FCLR
    outer = box(cx - li / 2 - FENCE_T, cy - wi / 2 - FENCE_T, FLOOR - 0.01, cx + li / 2 + FENCE_T, cy + wi / 2 + FENCE_T, FLOOR + FENCE_H)
    inner = box(cx - li / 2, cy - wi / 2, FLOOR - 1, cx + li / 2, cy + wi / 2, FLOOR + FENCE_H + 1)
    gapb = box(cx - gap / 2, cy, FLOOR - 1, cx + gap / 2, cy + wi, FLOOR + FENCE_H + 1)
    return outer - inner - gapb


def snap_sites():
    """("S"|"N", x) on long sides, ("W"|"E", y) on ends."""
    xs = [15.0, L - 15.0] + [GX0 + b * PCBW for b in range(1, NB)]
    sites = [("S", x) for x in xs] + [("N", x) for x in xs]
    sites += [("W", W / 2), ("E", 15.0), ("E", W - 15.0)]
    return sites


def side_geom(side):
    """Return (axis, outer_face_coord, direction_inward) for a wall."""
    if side == "S":
        return "y", 0.0, +1
    if side == "N":
        return "y", W, -1
    if side == "W":
        return "x", 0.0, +1
    return "x", L, -1


def slab(axis, c, half, n0, n1, z0, z1):
    """Box spanning c+-half along the wall, n0..n1 across it (absolute), z0..z1."""
    if axis == "y":
        return box(c - half, min(n0, n1), z0, c + half, max(n0, n1), z1)
    return box(min(n0, n1), c - half, z0, max(n0, n1), c + half, z1)


def prism(axis, c, half, profile):
    """Extrude a (n, z) profile (absolute across-wall coordinate) along the wall."""
    pts = []
    for a in (c - half, c + half):
        for n, z in profile:
            pts.append((a, n, z) if axis == "y" else (n, a, z))
    return hull(pts)


# ------------------------------ TRAY -----------------------------------------
def build_tray():
    body = box(0, 0, 0, L, W, TOP)
    cut = []
    cut.append(box(WALL, WALL, LEDGETOP, L - WALL, W - WALL, TOP + 1))                       # matrix pocket
    cut.append(box(WALL, GY0 + LEDGE, FLOOR, GX0 + PCBW - LEDGE, GY0 + PCBH - LEDGE, LEDGETOP + 1))  # board 1 compartment
    for b in range(1, NB):
        x = GX0 + b * PCBW
        cut.append(box(x + LEDGE, GY0 + LEDGE, FLOOR, x + PCBW - LEDGE, GY0 + PCBH - LEDGE, LEDGETOP + 1))
        cut.append(box(x - LEDGE - 1, W / 2 - 10, FLOOR, x + LEDGE + 1, W / 2 + 10, LEDGETOP + 1))    # seam notch
    # USB-C ports in the left wall: pocket the wall from inside down to a thin skin so the
    # board edge sits inside the wall, then punch only a rounded USB-C hole through the skin
    for yc, bw, bt in ((YU, USBW, USBT), (YE, ESPW, ESPT)):
        zt = FLOOR + PADH + bt
        cut.append(box(SKIN, yc - bw / 2 - ECLR, FLOOR, WALL + 0.01, yc + bw / 2 + ECLR, LEDGETOP + 0.01))
        cut.append(port_hole(yc, zt + 3.26 / 2))
    if YU - USBW / 2 - ECLR - RIB < GY0 + LEDGE:      # breakout overlaps the front ledge: clear it there
        cut.append(box(SKIN, WALL - 0.01, FLOOR, RECESS + OVERHANG + USBL + ECLR + RIB + 0.01, GY0 + LEDGE + 0.01, LEDGETOP + 0.01))
    # touch zones: pocket in the top wall (and into the ledge behind it) sized for a TTP223 standing on its
    # long edge, TOUCH pad toward the skin; open at the top (hidden by the frame rim); 6 mm wire channel
    # under the module's header end (header toward -x)
    for c in TOUCHX:
        pw, pd = TTPL + 2 * TCLR, TTPT + TCLR + 1.2
        cut.append(box(c - pw / 2, W - TSKIN - pd, TOP - TTPW - TCLR - 0.3, c + pw / 2, W - TSKIN, TOP + 1))
        cut.append(box(c - pw / 2 + 0.4, W - GY0 - LEDGE - 1, FLOOR, c - pw / 2 + 6.4, W - TSKIN - pd + 0.5, LEDGETOP + 0.01))
        zc = TOP - 0.1 - TTPW / 2
        cut.append(Manifold.sphere(DIMPLE_R, 64).translate([c, W + DIMPLE_R - DIMPLE_D, zc]))   # locating dimple
    # knob
    if KNOB == "EC11_TOP":
        c, zc = KNOB_X, 0.9 + EC_CLR + ENC_BODY / 2
        cut.append(box(c - ENC_BODY / 2 - EC_CLR, W - WALL - EC_DEPTH - 0.6, 0.9, c + ENC_BODY / 2 + EC_CLR, W - WALL + 0.01, LEDGETOP + 0.01))
        cut.append(Manifold.cylinder(WALL + 2, ENC_BUSH / 2 + 0.2, ENC_BUSH / 2 + 0.2, 48).rotate([-90, 0, 0]).translate([c, W - WALL - 1, zc]))
    if KNOB == "KY040_BACK":
        cut.append(Manifold.cylinder(FLOOR + 2, ENC_BUSH / 2 + 0.15, ENC_BUSH / 2 + 0.15, 48).translate([ENC_XY[0], ENC_XY[1], -1]))
    # pry slots on the outer top edge: back face and right end only (front edge it stands on stays clean)
    for side, c in (("N", 40.0), ("N", 119.0), ("E", W / 2)):
        axis, o, d = side_geom(side)
        cut.append(slab(axis, c, PRYW / 2, o - d, o + d * PRYD, TOP - PRYH, TOP + 1))
    for c in cut:
        body = body - c

    # rigid posts: the wall continues upward, POSTT thick, with an inward barb at the top
    for side, c in snap_sites():
        axis, o, d = side_geom(side)
        inner = o + d * WALL
        n = lambda v: inner + d * v
        body = body + slab(axis, c, POSTW / 2, n(P_OUT), n(P_IN), TOP - 3.0, TOP + HOOKH)
        prof = [(n(P_IN), TOP + BZ[0]), (n(BARB_TIP), TOP + BZ[1]), (n(BARB_TIP), TOP + BZ[2]), (n(P_IN), TOP + BZ[3])]
        body = body + prism(axis, c, POSTW / 2, prof)

    # cradles for the two small boards; rib tops carry board 1's edge; ribs notched for wires
    for yc, bl, bw, bt in ((YU, USBL, USBW, USBT), (YE, ESPL, ESPW, ESPT)):
        y0, y1 = yc - bw / 2 - ECLR, yc + bw / 2 + ECLR
        x0, x1 = SKIN, RECESS + OVERHANG + bl + ECLR
        body = body + box(x0 + 0.5, yc - 2, FLOOR, x1, yc + 2, FLOOR + PADH)
        outer = y0 - RIB > WALL + 0.5
        if outer:
            body = body + box(x0, y0 - RIB, FLOOR, x1 + RIB, y0, LEDGETOP)
        body = body + box(x0, y1, FLOOR, x1 + RIB, y1 + RIB, LEDGETOP)
        body = body + box(x1, y0 - RIB, FLOOR, x1 + RIB, y1 + RIB, LEDGETOP)
        zn = FLOOR + PADH + bt + 1.0
        if outer:
            body = body - box(x0 + 5, y0 - RIB - 0.01, zn, x1 + RIB - 5, y0 + 0.01, LEDGETOP + 1)
        body = body - box(x0 + 5, y1 - 0.01, zn, x1 + RIB - 5, y1 + RIB + 0.01, LEDGETOP + 1)
        body = body - box(x1 - 0.01, y0 + 3, zn, x1 + RIB + 0.01, y1 - 3, LEDGETOP + 1)

    # module fences
    body = body + fence(MPU_XY[0], MPU_XY[1], MPUL, MPUW)
    if KNOB == "EC11_TOP":    # post behind the body so it cannot be pushed back into the compartment
        body = body + box(KNOB_X - ENC_BODY / 2, W - WALL - EC_DEPTH - 0.6 - 1.2, FLOOR - 0.01, KNOB_X + ENC_BODY / 2, W - WALL - EC_DEPTH - 0.6, FLOOR + 3.0)
    if KNOB == "KY040_BACK":
        body = body + fence(ENC_XY[0] - KY_OFF + KY_L / 2, ENC_XY[1], KY_L + 1.0, KY_W + 1.0, gap=8.0)
    return body


# ------------------------------ GRID FRAME -----------------------------------
def build_grid():
    body = box(0, 0, 0, L, W, HR)
    ox, oy = GX0 + OVERLAP, GY0 + OVERLAP
    body = body - box(ox, oy, -1, GX0 + GW - OVERLAP, GY0 + GH - OVERLAP, HR + 1)            # opening
    body = body - box(GX0 - 0.5, GY0 - 0.5, CELLH, GX0 + GW + 0.5, GY0 + GH + 0.5, CELLH + DIFFT)   # diffuser pocket
    body = body - box(GX0 - 0.5 + LIP, GY0 - 0.5, CELLH, GX0 + GW + 0.5 - LIP, GY0 + GH + 0.5, HR + 1)  # through above, leaving end lips
    # snap pockets from the underside, then the horizontal spring beam in each: a cantilever along the
    # wall, anchored at its +along end (overlapping into the rim), free at the other, flexing across the wall
    for side, c in snap_sites():
        axis, o, d = side_geom(side)
        inner = o + d * WALL
        n = lambda v: inner + d * v
        body = body - slab(axis, c, BEAML / 2 + 0.5, n(POCKET_OUT), n(POCKET_IN), -1, HS)
        if axis == "y":
            body = body + box(c - BEAML / 2, min(n(B_IN), n(B_OUT)), 0, c + BEAML / 2 + 1.0, max(n(B_IN), n(B_OUT)), BEAMH)
        else:
            body = body + box(min(n(B_IN), n(B_OUT)), c - BEAML / 2, 0, max(n(B_IN), n(B_OUT)), c + BEAML / 2 + 1.0, BEAMH)
    # cell walls midway between LED centres
    off = (PCBW - 7 * PITCH) / 2 + PITCH / 2
    for b in range(NB):
        for i in range(7):
            x = GX0 + b * PCBW + off + i * PITCH
            body = body + box(x - CELLW / 2, GY0, 0, x + CELLW / 2, GY0 + GH, CELLH)
        if b > 0:
            x = GX0 + b * PCBW
            body = body + box(x - CELLW / 2, GY0, 0, x + CELLW / 2, GY0 + GH, CELLH)
    for i in range(7):
        y = GY0 + (PCBH - 7 * PITCH) / 2 + PITCH / 2 + i * PITCH
        body = body + box(GX0, y - CELLW / 2, 0, GX0 + GW, y + CELLW / 2, CELLH)
    return body


def to_trimesh(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3],
                           faces=np.asarray(mesh.tri_verts), process=False)


if __name__ == "__main__":
    for name, m in (("tray", build_tray()), ("grid", build_grid())):
        tm = to_trimesh(m)
        tm.export(f"{name}.stl")
        lo, hi = tm.bounds
        print(f"{name}.stl  pieces={len(m.decompose())}  watertight={tm.is_watertight}  volume={m.volume()/1000:.1f} cm3  "
              f"size={hi[0]-lo[0]:.1f} x {hi[1]-lo[1]:.1f} x {hi[2]-lo[2]:.1f} mm  tris={len(tm.faces)}")
    print(f"tray {L:.1f} x {W:.1f} x {TOP:.1f} mm (posts to z={TOP+HOOKH:.1f}); grid rim {HR:.1f}; assembled {THICK:.1f} mm thick")
    print(f"snap: post {POSTT} thick at n[{P_OUT:.1f},{P_IN:.1f}], barb tip n={BARB_TIP:.1f}; beam {BEAMT} x {BEAMH} x {BEAML} at n[{B_IN:.1f},{B_OUT:.1f}]; "
          f"pocket n[{POCKET_OUT:.1f},{POCKET_IN:.1f}]; rim lip inside pocket {CLR + OVERLAP - POCKET_IN:.2f} mm; rim skin outside pocket {WALL + POCKET_OUT:.2f} mm")
    print(f"diffuser {GW+0.5:.1f} x {GH+0.5:.1f} mm, up to {DIFFT-0.2:.1f} mm thick, under {LIP:.1f} mm end lips")
    print(f"USB breakout centre y={YU:.1f} (power port {YU-PORTW/2:.1f}..{YU+PORTW/2:.1f} mm above the desk), ESP32 centre y={YE:.1f}")
