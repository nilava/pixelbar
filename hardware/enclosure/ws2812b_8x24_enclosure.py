#!/usr/bin/env python3
"""
Snap-fit desk enclosure (stands upright on its front edge) joining three 8x8 WS2812B
matrices (65 x 65 mm, 8 mm pitch) into one 8x24 panel. Electronics sit on the back
under board 1 (ESP32-C3 SuperMini, 7Semi USB-C breakout), touch modules stand in the
top wall, an MPU-6050 under board 2, a bare EC11 behind the top wall at the right.

Closure: six M2 x 10 countersunk screws, heads flush in the frame's border. A 10 mm screw
cannot cross a 10 mm frame and still bite, so the tray carries a boss that rises INTO a
blind bore in the frame; the screw passes through the frame's remaining 5 mm and threads
5 mm down the boss. Four tapered corner keys locate the frame before the screws go in, and
a continuous lip round the outer edge closes the seam against light and dust.

Outputs (print flat as exported, no supports):
    tray.stl   - matrices, electronics, posts
    grid.stl   - 8x24 light grid, film pocket with end lips, screw bores and countersinks

Requires:  pip install manifold3d numpy trimesh
Run:       python ws2812b_8x24_enclosure.py
"""
import numpy as np
import trimesh
from manifold3d import Manifold

# ------------------------------ PARAMETERS (mm) ------------------------------
PCBT = 1.6                              # matrix thickness
# Matrix width along the chain, PER BOARD. This was one 65.0 for all three with a single 0.3 mm
# clearance per side across the whole 195 mm row - a tolerance stack-up error, because three
# separately-cut PCBs each carry their own width tolerance and it ADDS. Three 65.5 mm boards need
# 196.5 mm and the pocket was 195.6, so they had to be ground down. Measure each board and put the
# real numbers here; everything downstream, including where the cell walls land, follows from them.
PCBW_LIST = [65.0, 65.0, 65.0]
PCB_GAP = 0.4                           # designed gap between adjacent boards, so they never fight
CLR_X = 0.8                             # clearance at each END of the row (was CLR, 0.3)
PCBH_LIST = [67.0, 67.0, 67.0]          # matrix height (vertical when standing), per board, left to right
# Distance from each board's bottom (-y) edge to the CENTRE of its bottom LED row.
# None = assume the 8-row grid is centred on the board. Measure one 67 mm board; if the extra 2 mm
# is all on one edge instead, put the real numbers here and everything re-aligns.
LED_OFF_LIST = [3.5, 3.5, 3.5]          # bottom edge -> bottom LED centre = 1.0 bare + 2.5 half-body
NB = 3                                  # boards in a row -> 8 x 24
PITCH_X = 8.0                           # LED pitch along the chain (horizontal, across the 65 mm width)
PITCH_Y = 59.0 / 7                      # LED pitch up the board = 8.4286; measured 59 mm across 7 pitches
# Measured on the boards: 61 mm across the LED bodies horizontally (2 mm bare each side of 65 mm),
# and 59 mm centre-to-centre over 7 pitches vertically. The vertical grid is NOT 8 mm.
# The board is asymmetric: 1 mm bare at the bottom edge, 2 mm at the top.
WALL = 7.0                              # tray wall. 7.0, not 6.0, because the screw bosses and the lip
# both live in it, and because the wall sets the frame's border width: at 6.0 the bottom border is only
# 6.6 mm, too narrow for a boss bore with printable walls either side of it.
CLR = 0.3                               # clearance around matrix row, per side
FLOOR = 2.0                             # tray floor
BACKCLR = 14.5                          # compartment height. 12.0 was set to "just fit" a 12.4 EC11 and
# it did not: the body had to be recessed into the floor, which meant it grounded out before its bushing
# reached the hole, and left nothing spare in z for pins or wires. 14.5 clears it by 2.1 mm with the
# floor intact. It costs 2.5 mm of panel thickness and buys a bigger touch dish as well.
LEDGE = 5.0                             # ledge supporting matrix PCB edges
CELLW, CELLH = 1.0, 8.0                 # grid wall thickness / height above PCB
# Diffuser. "printed" closes the top of every cell with a thin printed skin, so there is no acrylic
# and no vinyl; print the frame body in black and swap to white for the last few layers. "sheet"
# keeps the old pocket for a cut acrylic sheet.
DIFFUSER = "sheet"      # <- switch back to "printed" once the swatch settles SKIN_T
SKIN_T = 0.8                            # printed skin over the cells (4 layers at 0.2); thinner = brighter
DIFFT = 0.5                             # sheet mode: pocket depth. OHP film ~0.1 + vinyl ~0.1 = 0.2;
                                        # 1.3 was sized for 1 mm acrylic and would let film rattle
LIP = 1.5                               # sheet mode: lips over the sheet at both ends
LIPT = 1.5                              # sheet mode: lip thickness
OVERLAP = 1.3                           # rim overlap in x; leaves 0.7 mm to the LED bodies
FILM_OVL = 1.6                          # how far the film pocket reaches past the opening (was 2.1;
                                        # trimmed to stay clear of the screw clearance holes)

# electronics (under board 1, ports through the left end wall)
ESPL, ESPW, ESPT = 22.5, 18.0, 1.0      # ESP32-C3 SuperMini
USBL, USBW, USBT = 20.0, 16.0, 1.6      # 7Semi USB-C breakout; length measured, width/thickness from the listing
OVERHANG = 1.0                          # receptacle sticks out past its PCB edge
RECESS = 0.3                            # receptacle face behind the outer wall face
SKIN = 1.2                              # wall thickness left at the port cutouts (wall is pocketed from inside)
PORTW, PORTH, PORTR = 9.8, 4.2, 1.4     # USB-C cutout, rounded rectangle (receptacle shell is 8.94 x 3.26)
RIB, PADH, ECLR = 1.5, 2.5, 0.3         # cradle rib, pad height, board clearance
PAD_KEEPOUT = 10.0                      # pad strip stops this far short of the rear edge, clear of
                                        # the through-hole pads you solder wires into
# Crush ribs: off. They were added to take up the slack on the breakout when its width was only known
# to the nearest mm off a ruler; removed at Nilava's request. Both cradles are now a plain ECLR slip
# fit. Set CRUSH_USB back to ~0.55 to bring them back.
CRUSH_USB, CRUSH_ESP = 0.0, 0.0
CRUSH_N = 3                             # ribs per side
                                        # the through-hole pads you solder wires into
# The matrices carry their connectors at DIAGONAL CORNERS: with the LEDs facing you, input is at the
# top-left and output at the bottom-right. The ledge is cut away at both of those corners on every
# board so the connector shells clear. Board N's output (bottom-right) sits at the same seam as board
# N+1's input (top-left), so the chain still runs left to right; each seam jumper crosses diagonally.
PAD_W, PAD_D = 16.0, 14.0               # corner relief: clears a 10 x 12 mm connector either way round
SEAM_W = 6.0                            # full-height wire channel on each seam (ledge is 2 x LEDGE wide there)
SEAM_MOUTH, SEAM_TAPER = 15.0, 26.0     # the channel flares to this at each end, over this length.
# A straight wire needs no extra width; it needs room where it TURNS. So the width stays at SEAM_W
# (which keeps a 2 mm ledge either side carrying the board edges) and the ends flare instead.
CONN_H = 9.0                            # how far a connector may hang below the board before it fouls the floor

# touch zones: HW-763 TTP223 modules stand in pockets in the top wall, TOUCH pad against a thin skin
TTPL, TTPW, TTPT = 15.0, 11.0, 2.5      # module: long side, short side (vertical in pocket), thickness incl. parts (measured)
TSKIN, TCLR = 2.0, 0.4                  # wall left in front of the pad, clearance around the module
# A capacitive pad reads through solid plastic, not through air, so the module must end up against the
# skin. Rather than squeeze it (which fights you when the wires are already on), the pocket is simply
# no deeper than the module: it drops in freely and has nowhere to float to. The room for the solder
# joints and the wire tails is a local deepening at the header end only.
# Pocket depth is deliberately generous: the gap behind the module is the sensitivity adjustment. Pack
# it out BEHIND the module to hold the pad hard against the skin (most sensitive), or shim IN FRONT of
# the pad to back it off. 1.7 mm of range, ~1.55 once printed. The module must always be packed one way
# or the other - left loose it floats back and the reading gets worse, not better.
TDEPTH = TTPT + 1.7                     # pocket depth into the wall
TRELIEF, TRELIEF_W = 1.6, 6.0           # extra depth x width at the header end, for joints and wire
# Locating dish on the top edge, centred on the pad. It has to be OVAL, not round: the wall is only
# 15.6 mm tall and the lip starts at the top of it, so a 16 mm circle would cut straight through the
# seal. Wide across the edge, where there is room, and shorter up it - which is a fingertip's shape
# anyway. Deeper also means less plastic between finger and pad at the point the finger actually lands.
# The dish runs 14 mm FRONT-TO-BACK, which is the way a finger naturally lies on a top edge, and it is
# centred on the tray's usable band rather than on the pad - the pad is 11 mm tall and the finger only
# has to land on it, whereas the dish has to be findable. 14.0 is the most the tray can give: the frame
# owns z above TOP, and carrying the dish across that seam would put a ~0.15 mm step in its deepest
# part, right where the fingertip sits.
DIMPLE_WX, DIMPLE_WZ, DIMPLE_D = 16.0, 16.0, 1.2    # dish width along the edge, front-to-back, depth
DIMPLE_TOP = 0.7                        # flat left between the dish and the wall top
DIMPLE_R = 20.0                         # base sphere; only sets the curvature, the scale sets the size
MPUL, MPUW = 21.2, 15.6                 # GY-521 MPU-6050 module footprint
FENCE_T, FENCE_H, FCLR = 1.2, 3.0, 0.3  # module fences on the compartment floor

# knob: "EC11_TOP" = bare EC11 behind the top wall, shaft out of the top edge, nut under the knob
#       "KY040_BACK" = KY-040 module flat under board 3, shaft through the back
KNOB = "EC11_TOP"
KNOB_X = 185.0                          # shaft position along the top edge. Moved out from 176: its
                                        # pocket was sitting exactly where the right-hand top screw
                                        # needed to be, which forced both top screws into one small
                                        # gap near the centre, 24 mm apart. 185 and not more:
                                        # the pocket must stay inside the compartment, or a
                                        # sliver of solid ledge is left behind its far side.
EC_CLR = 0.4                            # clearance around the EC11 body in its pocket
EC_DEPTH = 7.0                          # EC11 body depth behind the wall (without bushing)
ENC_BODY, ENC_BUSH, ENC_H = 12.4, 7.0, 7.0
# An EC11's 5 pins leave the body's SIDES near the back and bend rearward, and wires soldered onto them
# are thicker than the pins. The pocket is a close fit on the body, so the back of it is opened out to
# give that bulk somewhere to live. Only the front 5 mm of the pocket stays narrow - that is what
# locates the body. ORIENT THE ENCODER WITH ITS PINS POINTING ALONG THE PANEL, not up and down: the
# compartment is only 12 mm tall, so there is room sideways and almost none vertically.
EC_PIN_W = 6.0                          # pocket half-width beyond the body, its whole depth. The side
                                        # walls this leaves are 12 mm clear of the body on each side -
                                        # effectively gone. They had no job: the NUT locates and clamps
                                        # the encoder, the way every panel-mount control works, and the
                                        # walls only ever got in the way of the pins and their wires.
                                        # The cost is board 3's top-edge ledge over that span, which is
                                        # the same trade the corner reliefs already make on every board.
# NO backstop behind the body: the NUT takes the knob press. There was a gusseted web here, and it made
# the encoder impossible to fit - the bushing has to slide EC_DEPTH forward into the wall hole, so the
# body must start that far back, and the web sat in exactly that space. The EC11's pins exit backwards
# into it too. The nut does the job properly anyway: a press pulls the bushing inward, which pulls the
# nut against the floor of its recess, and the wall under it takes the load in pure compression.
# => THE NUT IS NOT OPTIONAL. Without it nothing stops the encoder being pushed into the case.
# An EC11 bushing is only 5 mm long; a 6 mm wall swallows it and the nut has nothing to bite. Recess
# the outside so the thread comes through. Set EC_NUT_Z = 0 to drop the nut and rely on the web alone.
EC_NUT_D, EC_WALL = 12.0, 2.5           # nut recess diameter; wall left under it (bushing gets the rest).
                                        # Derived from WALL, so thickening the wall cannot re-swallow
                                        # the bushing the way it did when WALL went 6.0 -> 7.0.
ENC_XY = (170.0, 50.0)                  # KY-040 shaft centre on the back (KY040_BACK only)
KY_L, KY_W, KY_T, KY_OFF = 32.0, 19.0, 1.6, 10.0

# CLOSURE: six M2 x 10 countersunk screws. The snap version held ~5 kg and broke on the way out - a
# 22-degree retention face grips hard in both directions, and prying it is the same motion as breaking
# it. Screws separate holding from opening, which is what a serviceable box wants.
#
# They go in from the BACK, not the front - the display face stays unbroken. A counterbore sinks the
# head 10 mm into the tray's wall, so a 10 mm screw's tip still lands 5.4 mm inside the frame's rim.
# Nothing is visible from the front, and the back faces away from the room.
# M3, not M2. The head sits in the tray's wall now rather than the frame's border, so the border no
# longer sets the size - and M2 was the wrong tool: a 0.4 mm pitch has to displace a lot of PLA and an
# M2 Phillips cams out long before it manages. M3's coarser thread bites better and takes ~3x the torque.
SCR_D, SCR_L = 3.0, 10.0                # M3 x 10 CSK
SCR_HEAD, SCR_CLEAR = 5.6, 3.4          # head dia, clearance hole through the tray
# Pilot: a vertical FDM hole prints 0.15-0.25 mm undersize, so the nominal is set ABOVE the tap size.
# 2.8 nominal lands near 2.6 actual, which is the tap drill for M3 and leaves ~0.2 mm of radial bite.
SCR_PILOT = 2.8
SCR_IN = 5.4                            # screw axis, in from the outer face
SCR_CB_D, SCR_ENG = 6.0, 5.5            # counterbore dia, and the thread engagement to aim for.
                                        # SCR_CB_Z is DERIVED from TOP below: it was a fixed 10.0 and
                                        # raising BACKCLR quietly cut the engagement to 3.1 mm.
SCR_TAP = 7.0                           # pilot depth into the frame
# Tongue and groove, set INBOARD of the outer face. The first attempt put the lip ON the outer edge,
# which forced the frame to be rebated there and then chamfered back out to flush - and that chamfer
# was a 1.05 mm groove running round the whole panel. Inboard, both outer faces meet flush and all you
# see is the parting line. The tongue also locates the frame continuously, so the corner keys are gone.
LIP_OFF, LIP_T, LIP_H = 1.6, 1.4, 1.8   # tongue: in from the outer face, thickness, height above the wall
LIP_CLR, LIP_GAP = 0.15, 0.4            # groove clearance per side, and how much DEEPER than the tongue
# LIP_GAP is the bug that left a gap on the first print: the groove was cut to exactly LIP_H, so the
# tongue bottomed out in it and propped the frame open before the faces could touch.

# ------------------------------ derived --------------------------------------
PCBH_MAX = max(PCBH_LIST)
LED_OFF = LED_OFF_LIST or [(h - 7 * PITCH_Y) / 2 for h in PCBH_LIST]
PCBW = PCBW_LIST[0]                                  # nominal, for the cell-spacing maths
ROW = sum(PCBW_LIST) + (NB - 1) * PCB_GAP            # full width the boards occupy
L = 2 * WALL + ROW + 2 * CLR_X
W = 2 * WALL + PCBH_MAX + 2 * CLR
LEDGETOP = FLOOR + BACKCLR
TOP = LEDGETOP + PCBT                                # tray wall top = PCB top
GX0, GY0 = WALL + CLR_X, WALL + CLR                  # matrix pocket origin
GW = ROW
LEDY0 = GY0 + max(LED_OFF)                           # bottom LED row centre, common to all three boards;
#        max() so the board with the largest bottom margin still sits inside the pocket
LEDYC = LEDY0 + 3.5 * PITCH_Y                        # LED grid centre line
# the opening is asymmetric because the board is: 1 mm of bare board at the bottom against 2 mm at the top
# Clearance to the outer LED rows. The board's own margin is 1 mm at the bottom, shared with the web
# between the opening and the snap pockets. 0.7 each way is the balance; 0.2 was far too tight.
OPEN_Y0 = LEDY0 - 2.5 - 0.7
OPEN_Y1 = LEDY0 + 7 * PITCH_Y + 2.5 + 0.8


def board_x(b):
    """Left edge of board b, from its own measured width and the gaps before it."""
    return GX0 + sum(PCBW_LIST[:b]) + b * PCB_GAP


def board_y(b):
    """(y0, y1) of board b, placed so its LED rows land on the common grid."""
    y0 = LEDY0 - LED_OFF[b]
    return y0, y0 + PCBH_LIST[b]
HR = CELLH + (SKIN_T if DIFFUSER == "printed" else DIFFT + LIPT)   # frame rim height
THICK = TOP + HR                                     # assembled panel thickness
PORT_Y = 21.0                                        # power port centre above the desk (right-angle plug clears)
YU = PORT_Y
YE = W - GY0 - LEDGE - RIB - ECLR - ESPW / 2         # ESP32 centre y
TOUCHX = [55.0, L / 2, L - 55.0]
MPU_XY = (L / 2, 30.0)
EC_NUT_Z = WALL - EC_WALL                            # nut recess depth
SCR_CB_Z = TOP + SCR_ENG - (SCR_HEAD + 0.3 - SCR_CLEAR) / 2 - SCR_L   # head seat depth from the back


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


def screw_sites():
    """Six M2 screws: two along the bottom edge, two along the top, one at each end.

    The top wall is the crowded one. Three touch pockets, the EC11, both seam channels (which flare to
    SEAM_MOUTH near the wall, so 15 mm wide there, not 6) and the three top-left CORNER RELIEFS between
    them account for 139 of its 209.6 mm. The pair below sit in the only symmetric gap left. The end
    pair sit between the two USB-C port pockets.
    """
    return [(L / 4, SCR_IN), (3 * L / 4, SCR_IN),
            (L / 2 - 63.0, W - SCR_IN), (L / 2 + 63.0, W - SCR_IN),
            (SCR_IN, W / 2), (L - SCR_IN, W / 2)]


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
    # tongue on the wall top, set in from the outer face so both outer faces still meet flush
    body = body + (box(LIP_OFF, LIP_OFF, TOP - 0.01, L - LIP_OFF, W - LIP_OFF, TOP + LIP_H)
                   - box(LIP_OFF + LIP_T, LIP_OFF + LIP_T, TOP - 0.02,
                         L - LIP_OFF - LIP_T, W - LIP_OFF - LIP_T, TOP + LIP_H + 0.01))
    cut = []
    cut.append(box(WALL, WALL, LEDGETOP, L - WALL, W - WALL, TOP + 1))                       # matrix pocket
    by0, by1 = board_y(0)
    cut.append(box(WALL, by0 + LEDGE, FLOOR, board_x(0) + PCBW_LIST[0] - LEDGE, by1 - LEDGE, LEDGETOP + 1))       # board 1 compartment
    for b in range(1, NB):
        x = board_x(b)
        by0, by1 = board_y(b)
        cut.append(box(x + LEDGE, by0 + LEDGE, FLOOR, x + PCBW_LIST[b] - LEDGE, by1 - LEDGE, LEDGETOP + 1))
        # full-height channel on the seam, so a jumper runs straight from the lower board's output
        # corner to the upper one's input corner instead of zig-zagging under both boards.
        # 6 mm wide leaves a 2 mm ledge either side, enough to keep carrying the board edges.
        cut.append(box(x - SEAM_W / 2, WALL, FLOOR, x + SEAM_W / 2, W - WALL, LEDGETOP + 1))
        # funnel each end out to SEAM_MOUTH over SEAM_TAPER, so a solid-core wire turns into the
        # channel on a long gentle curve instead of a right angle
        for yend, into in ((WALL, +1), (W - WALL, -1)):
            cut.append(hull([(x + sx * SEAM_MOUTH / 2, yend, z) for sx in (-1, 1) for z in (FLOOR, LEDGETOP + 1)] +
                            [(x + sx * SEAM_W / 2, yend + into * SEAM_TAPER, z) for sx in (-1, 1) for z in (FLOOR, LEDGETOP + 1)]))
    # corner reliefs at every board's input (top-left) and output (bottom-right) connector.
    # Collected separately and subtracted last, so the cradle ribs cannot fill them back in.
    reliefs = []
    for b in range(NB):
        bx0, bx1 = board_x(b), board_x(b) + PCBW_LIST[b]
        bry0, bry1 = board_y(b)
        for xc, along, yedge, inward in ((bx0, +1, bry1, -1), (bx1, -1, bry0, +1)):
            # the connector extends INBOARD from its corner, so the relief runs that way too,
            # plus 1 mm past the corner for clearance; clamped to the pocket
            x0 = max(WALL, min(xc - 1, xc + along * PAD_W))
            x1 = min(L - WALL, max(xc + 1, xc + along * PAD_W))
            # and right out to the pocket wall, so no sliver of ledge is left standing
            y0, y1 = (yedge - PAD_D, W - WALL) if inward < 0 else (WALL, yedge + PAD_D)
            reliefs.append(box(x0, y0, FLOOR, x1, y1, LEDGETOP + 1))

    # USB-C ports in the left wall: pocket the wall from inside down to a thin skin so the
    # board edge sits inside the wall, then punch only a rounded USB-C hole through the skin
    for yc, bw, bt in ((YU, USBW, USBT), (YE, ESPW, ESPT)):
        zt = FLOOR + PADH + bt
        cut.append(box(SKIN, yc - bw / 2 - ECLR, FLOOR, WALL + 0.01, yc + bw / 2 + ECLR, LEDGETOP + 0.01))
        cut.append(port_hole(yc, zt + 3.26 / 2))
    if YU - USBW / 2 - ECLR - RIB < board_y(0)[0] + LEDGE:   # breakout overlaps the front ledge: clear it there
        cut.append(box(SKIN, WALL - 0.01, FLOOR, RECESS + OVERHANG + USBL + ECLR + RIB + 0.01, board_y(0)[0] + LEDGE + 0.01, LEDGETOP + 0.01))
    # touch zones: pocket in the top wall (and into the ledge behind it) sized for a TTP223 standing on its
    # long edge, TOUCH pad toward the skin; open at the top (hidden by the frame rim); 6 mm wire channel
    # under the module's header end (header toward -x)
    late = []                                        # top-wall pockets, re-cut after the centring pads go in
    for c in TOUCHX:
        pw = TTPL + 2 * TCLR
        rx0, rx1 = c - pw / 2 + 0.4, c - pw / 2 + 0.4 + TRELIEF_W   # header end, toward -x
        zf, yb = TOP - TTPW - TCLR - 0.3, W - TSKIN - TDEPTH
        # up through the tongue, not just to TOP + 1: stopping short left the tongue's top 0.8 mm
        # spanning the open pocket as a 1 mm wide, 15.8 mm long strand with nothing under it
        late.append(box(c - pw / 2, yb, zf, c + pw / 2, W - TSKIN, TOP + LIP_H + 0.01))
        cut.append(late[-1])
        late.append(box(rx0, yb - TRELIEF, zf, rx1, W - TSKIN, TOP + LIP_H + 0.01))   # joints and tails
        cut.append(late[-1])
        cut.append(box(rx0, W - GY0 - LEDGE - 1, FLOOR, rx1, yb - TRELIEF + 0.5, LEDGETOP + 0.01))
        zc = TOP - DIMPLE_TOP - DIMPLE_WZ / 2        # as far forward as the wall allows
        r0 = (2 * DIMPLE_R * DIMPLE_D - DIMPLE_D ** 2) ** 0.5
        cut.append(Manifold.sphere(DIMPLE_R, 96)
                   .scale([DIMPLE_WX / 2 / r0, 1.0, DIMPLE_WZ / 2 / r0])
                   .translate([c, W + DIMPLE_R - DIMPLE_D, zc]))
    # knob
    if KNOB == "EC11_TOP":
        c, zc = KNOB_X, FLOOR + (LEDGETOP - FLOOR) / 2       # centred in the compartment
        late.append(box(c - ENC_BODY / 2 - EC_PIN_W, W - WALL - EC_DEPTH - 0.6, FLOOR,
                        c + ENC_BODY / 2 + EC_PIN_W, W - WALL + 0.01, LEDGETOP + 0.01))
        cut.append(late[-1])
        cut.append(Manifold.cylinder(WALL + 2, ENC_BUSH / 2 + 0.2, ENC_BUSH / 2 + 0.2, 48).rotate([-90, 0, 0]).translate([c, W - WALL - 1, zc]))
        # no floor trough any more: with BACKCLR at 14.5 the body clears the floor, and everything
        # behind the pocket is open compartment already
        if EC_NUT_Z > 0:                             # recess the outside so the bushing thread comes through
            cut.append(Manifold.cylinder(EC_NUT_Z + 1, EC_NUT_D / 2, EC_NUT_D / 2, 64).rotate([-90, 0, 0]).translate([c, W - EC_NUT_Z, zc]))
    if KNOB == "KY040_BACK":
        cut.append(Manifold.cylinder(FLOOR + 2, ENC_BUSH / 2 + 0.15, ENC_BUSH / 2 + 0.15, 48).translate([ENC_XY[0], ENC_XY[1], -1]))
    for c in cut:
        body = body - c

    # centring pads: a board shorter than the tallest gets a pad on each long wall, so it cannot
    # slide and its LED rows stay on the common grid
    for b in range(NB):
        by0, by1 = board_y(b)
        x0 = WALL if b == 0 else board_x(b)
        x1 = board_x(b) + PCBW_LIST[b]
        if by0 - CLR - WALL > 0.3:
            body = body + box(x0, WALL, LEDGETOP, x1, by0 - CLR, TOP)
        if (W - WALL) - (by1 + CLR) > 0.3:
            body = body + box(x0, by1 + CLR, LEDGETOP, x1, W - WALL, TOP)
    for c in late:                                   # keep the top-wall pockets clear of the pads
        body = body - c

    # The M3 counterbore is wider than the end walls, so it would break into the compartment there and
    # leave the head's cone seat missing on one side. A local pad on the inside fills it; it sits in the
    # empty span between the breakout and the ESP32.
    pad = SCR_CB_D / 2 + 1.2
    for sx, sy in screw_sites():
        if sx < WALL:
            body = body + box(WALL - 0.01, sy - pad, FLOOR, SCR_IN + pad, sy + pad, LEDGETOP)
        elif sx > L - WALL:
            body = body + box(L - SCR_IN - pad, sy - pad, FLOOR, L - WALL + 0.01, sy + pad, LEDGETOP)
    # screws from the back: counterbore for the driver, cone for the head, clearance hole up to the frame
    cone = (SCR_HEAD + 0.3 - SCR_CLEAR) / 2
    for sx, sy in screw_sites():
        body = body - Manifold.cylinder(SCR_CB_Z + 0.01, SCR_CB_D / 2, SCR_CB_D / 2, 48).translate([sx, sy, -0.01])
        body = body - Manifold.cylinder(cone + 0.01, (SCR_HEAD + 0.3) / 2, SCR_CLEAR / 2, 48).translate([sx, sy, SCR_CB_Z])
        body = body - Manifold.cylinder(TOP + 2, SCR_CLEAR / 2, SCR_CLEAR / 2, 32).translate([sx, sy, SCR_CB_Z])

    # cradles for the two small boards; rib tops carry board 1's edge; ribs notched for wires
    for yc, bl, bw, bt, crush in ((YU, USBL, USBW, USBT, CRUSH_USB), (YE, ESPL, ESPW, ESPT, CRUSH_ESP)):
        y0, y1 = yc - bw / 2 - ECLR, yc + bw / 2 + ECLR
        x0, x1 = SKIN, RECESS + OVERHANG + bl + ECLR
        body = body + box(x0 + 0.5, yc - 2, FLOOR, x1 - PAD_KEEPOUT, yc + 2, FLOOR + PADH)
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
        # triangular crush ribs on both side walls, starting clear of the entry so the board leads in
        if crush > 0:
            ztop = FLOOR + PADH + bt + 0.5
            for k in range(CRUSH_N):
                xr = x0 + 5.0 + k * (bl - 8.0) / max(1, CRUSH_N - 1)
                for face, sgn in ((y0, +1), (y1, -1)):
                    body = body + hull([(xr - 1.1, face, z) for z in (FLOOR, ztop)] +
                                       [(xr + 1.1, face, z) for z in (FLOOR, ztop)] +
                                       [(xr, face + sgn * crush, z) for z in (FLOOR, ztop)])

    # module fences
    body = body + fence(MPU_XY[0], MPU_XY[1], MPUL, MPUW)
    if KNOB == "KY040_BACK":
        body = body + fence(ENC_XY[0] - KY_OFF + KY_L / 2, ENC_XY[1], KY_L + 1.0, KY_W + 1.0, gap=8.0)
    for c in reliefs:                                 # last, so nothing added above blocks a connector
        body = body - c
    return body


# ------------------------------ GRID FRAME -----------------------------------
def build_grid():
    body = box(0, 0, 0, L, W, HR)
    ox0, ox1 = GX0 + OVERLAP, GX0 + GW - OVERLAP
    if DIFFUSER == "printed":
        # cut the opening only up to the top of the cells; the last SKIN_T of the frame stays solid
        # and becomes the diffuser, bridging each cell
        body = body - box(ox0, OPEN_Y0, -1, ox1, OPEN_Y1, CELLH)
    else:
        body = body - box(ox0, OPEN_Y0, -1, ox1, OPEN_Y1, HR + 1)                            # opening
        dx0, dx1 = ox0 - FILM_OVL, ox1 + FILM_OVL
        dy0, dy1 = OPEN_Y0 - FILM_OVL, OPEN_Y1 + FILM_OVL
        body = body - box(dx0, dy0, CELLH, dx1, dy1, CELLH + DIFFT)                          # sheet pocket
        body = body - box(dx0 + LIP, dy0, CELLH, dx1 - LIP, dy1, HR + 1)                     # through above, end lips remain
    # blind pilot holes for the screws; nothing breaks through to the front face
    for sx, sy in screw_sites():
        body = body - Manifold.cylinder(SCR_TAP, SCR_PILOT / 2, SCR_PILOT / 2, 32).translate([sx, sy, -0.01])
    # cell walls midway between LED centres
    # cells are placed from each board's OWN origin, so an oversize board shifts its own cells with
    # it instead of pushing every later board's LEDs off their walls
    for b in range(NB):
        for i in range(7):
            x = board_x(b) + (PCBW_LIST[b] - 7 * PITCH_X) / 2 + PITCH_X / 2 + i * PITCH_X
            body = body + box(x - CELLW / 2, OPEN_Y0, 0, x + CELLW / 2, OPEN_Y1, CELLH)
        if b > 0:
            x = board_x(b) - PCB_GAP / 2
            body = body + box(x - CELLW / 2, OPEN_Y0, 0, x + CELLW / 2, OPEN_Y1, CELLH)
    for i in range(7):
        y = LEDY0 + PITCH_Y / 2 + i * PITCH_Y
        body = body + box(ox0, y - CELLW / 2, 0, ox1, y + CELLW / 2, CELLH)
    # sockets for the tray's locating keys: the same taper, offset KEY_CLR, and 0.2 deeper so the key
    # never bottoms out before the frame is down on the wall
    # groove for the tray's tongue: LIP_CLR each side, and LIP_GAP DEEPER, so it can never bottom out
    g0, g1 = LIP_OFF - LIP_CLR, LIP_OFF + LIP_T + LIP_CLR
    body = body - (box(g0, g0, -0.01, L - g0, W - g0, LIP_H + LIP_GAP)
                   - box(g1, g1, -0.02, L - g1, W - g1, LIP_H + LIP_GAP + 0.01))
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
    print(f"tray {L:.1f} x {W:.1f} x {TOP:.1f} mm (tongue to z={TOP+LIP_H:.1f}); grid rim {HR:.1f}; assembled {THICK:.1f} mm thick")
    print(f"closure: {len(screw_sites())} x M{SCR_D:.0f} x {SCR_L:.0f} CSK at {[(round(x,1), round(y,1)) for x, y in screw_sites()]}")
    print(f"         from the BACK: {SCR_CB_D} counterbore {SCR_CB_Z} deep -> {SCR_L - (TOP - SCR_CB_Z - (SCR_HEAD + 0.3 - SCR_CLEAR) / 2):.1f} mm of thread in the frame")
    print(f"tongue {LIP_T} wide x {LIP_H} tall at {LIP_OFF} in from the face; groove {LIP_GAP} deeper, {LIP_CLR} clear each side")
    print(f"boards {PCBH_LIST} tall, pitch {PITCH_X} x {PITCH_Y}; bottom LED row at y={LEDY0:.1f}; "
          f"centring pads {[round((PCBH_MAX - h) / 2, 2) for h in PCBH_LIST]} mm per side")
    print(f"LED span on one board: {7*PITCH_X:.1f} x {7*PITCH_Y:.1f} mm centre-to-centre, {7*PITCH_X+5:.1f} x {7*PITCH_Y+5:.1f} mm across the LED bodies")
    if DIFFUSER == "printed":
        print(f"diffuser: PRINTED, {SKIN_T:.1f} mm skin closing every cell; no acrylic, no vinyl; "
              f"cells bridge {PITCH_X - CELLW:.1f} x {PITCH_Y - CELLW:.1f} mm")
    else:
        print(f"diffuser: sheet {GW - 2 * OVERLAP + 4.2 - 0.5:.1f} x {OPEN_Y1 - OPEN_Y0 + 4.2 - 0.5:.1f} mm, up to {DIFFT-0.2:.1f} mm thick")
    print(f"USB breakout centre y={YU:.1f} (power port {YU-PORTW/2:.1f}..{YU+PORTW/2:.1f} mm above the desk), ESP32 centre y={YE:.1f}")
