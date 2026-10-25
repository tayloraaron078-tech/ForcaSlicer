"""Writes the small test models Forca's guided experiments print (src/slic3r/GUI/ForcaExperiment.cpp) into
resources/forca_experiments/. The bridge test is the one from the 2026-10 bridge investigation and is kept as is;
this script makes the overhang and stringing tests. Run from the repository root: python tools/forca/make_experiment_models.py
"""
import os

OUT = os.path.join("resources", "forca_experiments")


def prism(profile, length):
    """Triangles of a profile in the XZ plane (counter-clockwise, given as a list of triangles plus its outline)
    extruded along Y from 0 to `length`."""
    caps, outline = profile
    tris = []
    for a, b, c in caps:
        tris.append(((a[0], 0, a[1]), (b[0], 0, b[1]), (c[0], 0, c[1])))  # faces -Y
        tris.append(((a[0], length, a[1]), (c[0], length, c[1]), (b[0], length, b[1])))  # faces +Y
    for i, p in enumerate(outline):
        q = outline[(i + 1) % len(outline)]
        tris.append(((p[0], 0, p[1]), (q[0], length, q[1]), (q[0], 0, q[1])))
        tris.append(((p[0], 0, p[1]), (p[0], length, p[1]), (q[0], length, q[1])))
    return tris


def moved(tris, dx=0.0, dy=0.0):
    return [tuple((v[0] + dx, v[1] + dy, v[2]) for v in t) for t in tris]


def box(w, d, h):
    o = [(0, 0), (w, 0), (w, h), (0, h)]
    return prism(([(o[0], o[1], o[2]), (o[0], o[2], o[3])], o), d)


def normal(t):
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = t
    ux, uy, uz = bx - ax, by - ay, bz - az
    vx, vy, vz = cx - ax, cy - ay, cz - az
    n = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
    l = sum(c * c for c in n) ** 0.5
    return tuple(c / l for c in n)


def volume(tris):
    return sum(a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])
               for a, b, c in tris) / 6.0


def write(name, tris):
    assert volume(tris) > 0, name  # outward-facing triangles
    lines = ["solid " + name]
    for t in tris:
        lines.append("  facet normal %g %g %g" % normal(t))
        lines.append("    outer loop")
        for v in t:
            lines.append("      vertex %g %g %g" % tuple(round(c, 3) for c in v))
        lines.append("    endloop")
        lines.append("  endfacet")
    lines.append("endsolid " + name)
    with open(os.path.join(OUT, name + ".stl"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def overhang_test():
    # A 6 mm column 2 mm high that widens to a 12 mm high top: 45 degrees from vertical on the right (most printers
    # manage it), 60 degrees on the left (where sagging and curling show). 20 mm deep.
    t60 = 8 * 3 ** 0.5  # horizontal reach of 8 mm rise at 60 degrees
    o = [(-3, 0), (3, 0), (3, 2), (11, 10), (11, 12), (-3 - t60, 12), (-3 - t60, 10), (-3, 2)]
    caps = [(o[0], o[1], o[2]), (o[0], o[2], o[7])]  # the column
    caps += [(o[7], o[i], o[i + 1]) for i in range(2, 6)]  # the convex top, fanned from (-3, 2)
    return moved(prism((caps, o), 20), dx=3 + t60)


def stringing_test():
    # Two 6 x 6 mm towers, 20 mm high, 20 mm apart: every layer travels between them.
    return box(6, 6, 20) + moved(box(6, 6, 20), dx=26)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    write("overhang_test", overhang_test())
    write("stringing_test", stringing_test())
    print("wrote", OUT)
