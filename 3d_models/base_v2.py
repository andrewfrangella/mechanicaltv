"""Mechanical TV base, version 2: thicker plates, motor cradle, wider disk gap.

Built from bitluni's `mechanical tv base.stl` (frame below), so every feature that is not
deliberately changed keeps its exact position.

Frame (bitluni's STL, unchanged):
    z   disk axis. The disk plate is centred on z = 0; the motor is on +z, the screen on -z.
    y   up from the bottom strip ("spine"). The spine's underside, y = -41, sits on the deck.
    x   along the spine. The motor axis is x = y = 0; LED and screen at x = -100; sensor at x = -126.

What changes (all parameters below):
    * Disk gap: z -2..+2 (4 mm) -> z -GAP_LO..+GAP_HI (2.5 + 4.5 = 7 mm). The lower plate (screen
      holder) moves down by GROW_LO, kept small so the screen stays close to the disk (a screen
      further away gives a softer picture); the LED plate moves up by GROW_HI. Both plates keep
      their own thickness and features.
    * Sensor seat: bitluni's geometry is copied unchanged up to z = 9 (same place relative to the
      disk), so the same GODIYMODULES slotted speed sensor fits as before. Its 4 mm faces still
      limit the rim to 1.1 mm per side where it passes the sensor, exactly as in bitluni's base.
    * Spine: 4 -> 6 mm (grown inward, underside unchanged). The four deck screws stay where they
      were: three get a 2 mm counterbore so the M3x10 screws reach the deck nuts as before; the
      one at (x 18, z 9) sits under the thicker motor plate, so it is driven from under the deck
      into a captive nut that slides in from the end of the spine.
    * Motor plate: 3.5 -> 6 mm, all four NEMA 17 holes (bitluni used two), screw heads counterbored
      flush on the disk side.
    * Motor support: the 6 mm plate, four screws and gussets on both sides of the motor carry it
      (bitluni: 3.5 mm, two screws, one gusset). A two-rail cradle stands 0.3 mm under the motor
      body as a backstop that takes over if the motor sags; the motor cable runs between the rails.

Run:  python3 base_v2.py            (writes base_v2.stl and base_v2_print.stl next to this file)
"""
import os
import numpy as np
import trimesh
from manifold3d import Manifold, Mesh, CrossSection

HERE = os.path.dirname(os.path.abspath(__file__))
ORIG = os.path.expanduser(
    "~/Downloads/Existing Folders/MechanicalTV-main 2/Models/mechanical tv base.stl")

# ------------------------------------------------------------------ parameters
GROW_LO = 0.5            # screen-holder face moves this far away from the disk (bitluni face: z -2)
GROW_HI = 2.5            # LED plate / motor plate face moves this far away (bitluni face: z +2)
GAP_LO = 2.0 + GROW_LO   # gap faces at z = -GAP_LO and +GAP_HI
GAP_HI = 2.0 + GROW_HI
MOTOR_T = 6.0            # motor plate thickness (bitluni: 3.5)
SPINE_IN = -35.0         # spine inner face y (bitluni: -37 -> 4 mm; now 6 mm)
SPINE_X1 = 26.0          # spine end on the motor side (bitluni: 22), carries the second gusset
GUSSET_T = 4.0           # gusset thickness (bitluni's one gusset: 4)
MOTOR_HALF = 42.3 / 2    # NEMA 17 frame
MOTOR_LEN = 23.0         # 17HE08-1004S body length
CRADLE_CLEAR = 0.3       # cradle top to motor underside
CRADLE_RAILS = ((-21.0, -11.0), (11.0, 21.0))   # x spans; the cable channel is between them

FN = 64
M3 = 3.4 / 2             # clearance radius
CBORE = 6.5 / 2          # motor screw counterbore (M3 socket head 5.5)
SPINE_CBORE = 6.2 / 2    # deck screw counterbores (leaves >= 0.9 mm wall at the spine edges)
SENSOR_CBORE = 6.2 / 2   # access above the sensor bolts (the LED plate now stands 2.5 mm higher)
NUT_AF, NUT_T = 5.5, 2.4 # M3 nut

SPINE_SCREWS = [(18.0, 9.0), (18.0, 30.0), (-146.0, 13.0), (-146.0, 30.0)]  # (x, z), unchanged
CAPTIVE = (18.0, 9.0)    # driven from below into a captive nut
MOTOR_HOLES = [(sx * 15.5, sy * 15.5) for sx in (-1, 1) for sy in (-1, 1)]
SENSOR_BOLTS = [(-126.0, -18.25), (-126.0, -5.75)]

# sensor seat copied from bitluni: this box keeps his geometry exactly
SEAT = dict(x=(-135.0, -119.5), y=(-22.0, -2.0), z=(-6.0, 9.0))

assert GROW_LO > 0 and GROW_HI > 0, "the seat chamfers need both plates moved"
assert GAP_HI <= CAPTIVE[1] - (NUT_AF + 0.3) / 2 - 1.0, "GROW_HI too big: the gap would meet the nut slot"
assert GAP_LO <= 5.0, "GROW_LO too big: the screen-holder plate would leave the seat's lower pocket"



# ------------------------------------------------------------------ helpers
def box(x, y, z):
    return Manifold.cube([x[1] - x[0], y[1] - y[0], z[1] - z[0]]).translate([x[0], y[0], z[0]])


def cyl_z(cx, cy, r, z0, z1):
    return Manifold.cylinder(z1 - z0, r, r, FN).translate([cx, cy, z0])


def cyl_y(cx, cz, r, y0, y1):
    # cylinder along +y from y0 to y1, centred on (x=cx, z=cz)
    c = Manifold.cylinder(y1 - y0, r, r, FN)          # along +z, base at 0
    c = c.rotate([-90, 0, 0])                          # +z -> +y
    return c.translate([cx, y0, cz])


def prism_x(poly_yz, x0, x1):
    """Extrude a polygon given in (y, z) along x from x0 to x1."""
    cs = CrossSection([np.asarray(poly_yz, float)])
    m = Manifold.extrude(cs, x1 - x0)                  # polygon in XY, extruded along +z
    # map (X=y, Y=z, Z=x) -> (x, y, z)
    m = m.transform(np.array([[0, 0, 1, x0], [1, 0, 0, 0], [0, 1, 0, 0]], float))
    return m


def fix_t_junctions(V, F, tol=1e-3):
    """bitluni's STL has T-junctions (a vertex lying on the edge of the neighbouring triangle).
    Split every open edge at the open vertices that lie on it until the mesh is closed."""
    F = [list(f) for f in F]
    for _ in range(20):
        edges = {}
        for fi, f in enumerate(F):
            for k in range(3):
                a, b = f[k], f[(k + 1) % 3]
                edges.setdefault((min(a, b), max(a, b)), []).append((fi, k))
        open_e = [(e, v[0]) for e, v in edges.items() if len(v) == 1]
        if not open_e:
            return np.array(F)
        open_v = sorted({i for e, _ in open_e for i in e})
        P = V[open_v]
        new_F, split = [], {}
        for (a, b), (fi, k) in open_e:
            pa, pb = V[a], V[b]
            d = pb - pa
            L = np.linalg.norm(d)
            t = (P - pa) @ d / (L * L)
            dist = np.linalg.norm(P - (pa + np.outer(t, d)), axis=1)
            on = [(t[i], open_v[i]) for i in range(len(open_v))
                  if dist[i] < tol and tol / L < t[i] < 1 - tol / L]
            if on and fi not in split:
                split[fi] = (k, [v for _, v in sorted(on)])
        if not split:
            raise RuntimeError("open edges left that are not T-junctions")
        for fi, f in enumerate(F):
            if fi not in split:
                new_F.append(f)
                continue
            k, mids = split[fi]
            a, b, c = f[k], f[(k + 1) % 3], f[(k + 2) % 3]
            chain = [a] + mids + [b]
            for i in range(len(chain) - 1):
                new_F.append([chain[i], chain[i + 1], c])
        F = new_F
    raise RuntimeError("T-junction repair did not converge")


def load_orig():
    t = trimesh.load(ORIG)
    t.merge_vertices(digits_vertex=4)
    t.update_faces(t.nondegenerate_faces())
    t.update_faces(t.unique_faces())
    t.remove_unreferenced_vertices()
    parts = [p for p in t.split(only_watertight=False) if abs(p.volume) > 1.0]
    t = trimesh.util.concatenate(parts)
    t = trimesh.Trimesh(t.vertices, fix_t_junctions(np.asarray(t.vertices), t.faces), process=False)
    t.update_faces(t.nondegenerate_faces())
    m = Manifold(Mesh(vert_properties=np.asarray(t.vertices, np.float32),
                      tri_verts=np.asarray(t.faces, np.uint32)))
    assert m.status().name == "NoError", m.status()
    return m


def to_trimesh(m):
    me = m.to_mesh()
    return trimesh.Trimesh(vertices=me.vert_properties[:, :3], faces=me.tri_verts, process=False)


# ------------------------------------------------------------------ build
def build():
    orig = load_orig()
    BIG = 1000.0

    # 1. lower plate (screen holder, lower sensor pocket, lower spine strip): moved down by GROW_LO
    lower = (orig ^ box((-BIG, BIG), (-BIG, BIG), (-BIG, -2.0))).translate([0, 0, -GROW_LO])

    # 2. LED plate with the upper sensor block: moved up by GROW_HI
    led = (orig ^ box((-131.0, -78.0), (-37.0, 13.0), (2.0, 9.5))).translate([0, 0, GROW_HI])

    # 3. spine, 6 mm, same underside and ends (plus 4 mm on the motor end for the second gusset)
    z_bot = -6.0 - GROW_LO
    spine = box((-150.0, SPINE_X1), (-41.0, SPINE_IN), (z_bot, 34.0))
    spine -= box((-65.0, SPINE_X1 + 1), (-42.0, SPINE_IN + 1), (z_bot - 1, GAP_HI))  # no lower strip here
    spine -= box((-130.0, -64.0), (-42.0, SPINE_IN + 1), (-GAP_LO, GAP_HI))         # disk slot

    # 4. motor plate
    zp0, zp1 = GAP_HI, GAP_HI + MOTOR_T
    plate = box((-22.0, 22.0), (SPINE_IN - 2.0, 22.0), (zp0, zp1))

    # 5. gussets on both sides: from the spine top down to the plate's free edge
    g_poly = [(SPINE_IN - 2.0, zp0), (22.0, zp0), (22.0, zp1), (SPINE_IN - 2.0, 34.0)]
    gussets = prism_x(g_poly, -22.0 - GUSSET_T, -22.0) + prism_x(g_poly, 22.0, 22.0 + GUSSET_T)

    # 6. cradle under the motor body
    y_top = -MOTOR_HALF - CRADLE_CLEAR
    z_end = min(zp1 + MOTOR_LEN - 1.0, 34.0)
    cradle = Manifold()
    access = [(x, z) for x, z in SPINE_SCREWS if (x, z) != CAPTIVE and x > 0]
    for x0, x1 in CRADLE_RAILS:
        z1 = z_end
        for x, z in access:            # stop short of a deck screw so it can be driven from above
            if x0 - SPINE_CBORE <= x <= x1 + SPINE_CBORE:
                z1 = min(z1, z - SPINE_CBORE - 0.5)
        cradle += box((x0, x1), (SPINE_IN - 2.0, y_top), (zp1 - 1.0, z1))

    body = lower + led + spine + plate + gussets + cradle

    # 7. motor plate holes
    body -= cyl_z(0, 0, 12.0, zp0 - 1, zp1 + 1)                     # boss (22 mm) clearance, as bitluni
    for x, y in MOTOR_HOLES:
        body -= cyl_z(x, y, M3, zp0 - 1, zp1 + 1)
        body -= cyl_z(x, y, CBORE, zp0 - 1, zp0 + min(3.2, MOTOR_T - 2.0))  # head flush, disk side

    # 8. deck screws, same (x, z) as bitluni
    for x, z in SPINE_SCREWS:
        if (x, z) == CAPTIVE:
            continue
        body -= cyl_y(x, z, M3, -42.0, SPINE_IN + 1)
        body -= cyl_y(x, z, SPINE_CBORE, -37.0, SPINE_IN + 0.01)    # head at bitluni's height
    # captive nut: screw from under the deck (head in the deck's hex pocket), nut slides in from +x
    cx, cz = CAPTIVE
    body -= cyl_y(cx, cz, M3, -42.0, -29.0)                          # blind past the nut for the tip
    nut_w = NUT_AF + 0.3
    body -= box((cx - 3.4, SPINE_X1 + 1), (-38.4, -38.4 + NUT_T + 0.3), (cz - nut_w / 2, cz + nut_w / 2))

    # 9. sensor seat: bitluni's geometry up to z = 9, unchanged and in its original place
    seat_box = box(SEAT["x"], SEAT["y"], SEAT["z"])
    body = (body - seat_box) + (orig ^ seat_box)
    # the moved lower plate continues solid under the seat (its moved pocket copy would be a void)
    body -= box(SEAT["x"], SEAT["y"], (-6.0 - GROW_LO - 1, SEAT["z"][0]))
    body += box(SEAT["x"], SEAT["y"], (-6.0 - GROW_LO, SEAT["z"][0]))
    # above the seat the LED plate continues solid (the moved copy of the seat's top would leave
    # stray pockets), with counterbores down to bitluni's top face over the two sensor bolts
    led_top = 9.0 + GROW_HI
    body -= box(SEAT["x"], SEAT["y"], (SEAT["z"][1], led_top + 1))
    body += box((-130.0, SEAT["x"][1]), SEAT["y"], (SEAT["z"][1], led_top))
    for x, y in SENSOR_BOLTS:
        body -= cyl_z(x, y, SENSOR_CBORE, SEAT["z"][1], led_top + 1)

    # 10. 45-degree chamfers under the seat's two stubs (they stand GROW_HI / GROW_LO proud of the
    #     moved plates and would otherwise end in a flat overhang at y = -22 when printed spine-down)
    y0 = SEAT["y"][0]
    upper = prism_x([(y0, 2.0), (y0, GAP_HI), (y0 - GROW_HI, GAP_HI)], -130.0, SEAT["x"][1])
    lower = prism_x([(y0, -2.0), (y0 - GROW_LO, -GAP_LO), (y0, -GAP_LO)], SEAT["x"][0], SEAT["x"][1])
    body += upper + lower
    return orig, body


def main():
    orig, body = build()
    t = to_trimesh(body)
    assert t.is_watertight and t.body_count == 1, (t.is_watertight, t.body_count)
    t.export(os.path.join(HERE, "base_v2.stl"))
    # print orientation: spine underside (y = -41) on the bed, as bitluni's base is printed
    p = t.copy()
    p.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [1, 0, 0]))
    p.apply_translation(-p.bounds[0] + [0, 0, 0])
    p.apply_translation([-(p.bounds[0][0] + p.bounds[1][0]) / 2, -(p.bounds[0][1] + p.bounds[1][1]) / 2, 0])
    p.export(os.path.join(HERE, "base_v2_print.stl"))
    print("volume %.1f cm3 (bitluni %.1f)  bounds %s" % (
        t.volume / 1000, to_trimesh(orig).volume / 1000, np.round(t.bounds, 2).tolist()))
    print("print bounds", np.round(p.extents, 2).tolist())


if __name__ == "__main__":
    main()
