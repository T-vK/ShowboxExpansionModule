#!/usr/bin/env python3
"""Web-resolution export of the printed enclosure.

Reads the FreeCAD STL exports (millimetres, Z-up) and writes showcase/model.js:
quantised positions, crease-split normals, and the four lid-button centers.
The source STLs are not in the repo; pass their directory if it is not ~/Downloads.
"""

import argparse
import base64
import json
import struct
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent


def load_stl(path: Path) -> np.ndarray:
    with path.open("rb") as f:
        f.read(80)
        n = struct.unpack("<I", f.read(4))[0]
        rec = np.dtype([("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")])
        data = np.fromfile(f, dtype=rec, count=n)
    return data["v"].reshape(-1, 3).astype(np.float64)


def cluster(tris: np.ndarray, grid: float):
    flat = tris.reshape(-1, 3)
    q = np.round(flat / grid).astype(np.int64)
    q -= q.min(0)
    span = q.max(0) + 1
    keys = (q[:, 0] * span[1] + q[:, 1]) * span[2] + q[:, 2]
    _, inv = np.unique(keys, return_inverse=True)
    acc = np.zeros((inv.max() + 1, 3), np.float64)
    cnt = np.zeros(inv.max() + 1, np.float64)
    np.add.at(acc, inv, flat)
    np.add.at(cnt, inv, 1.0)
    verts = acc / cnt[:, None]
    faces = inv.reshape(-1, 3)
    keep = (faces[:, 0] != faces[:, 1]) & (faces[:, 1] != faces[:, 2]) & (faces[:, 0] != faces[:, 2])
    faces = faces[keep]
    ordered = np.sort(faces, axis=1)
    fkey = (ordered[:, 0].astype(np.int64) << 42) | (ordered[:, 1].astype(np.int64) << 21) | ordered[:, 2]
    _, idx = np.unique(fkey, return_index=True)
    faces = faces[np.sort(idx)]
    return verts, faces


def crease_split(verts, faces, cos_limit=0.55):
    tri = verts[faces]
    fn = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    ln = np.linalg.norm(fn, axis=1)
    good = ln > 1e-8
    faces = faces[good]
    fn = fn[good] / ln[good, None]
    buckets = [[] for _ in range(len(verts))]
    for fi, face in enumerate(faces):
        for corner, vi in enumerate(face):
            buckets[int(vi)].append((fi, corner))
    new_pos = []
    new_nrm = []
    new_faces = faces.copy()
    for vi, items in enumerate(buckets):
        if not items:
            continue
        groups = []
        for fi, corner in items:
            n = fn[fi]
            placed = False
            for g in groups:
                if float(np.dot(g[0], n)) >= cos_limit:
                    g[0] = g[0] + n
                    g[1].append((fi, corner))
                    placed = True
                    break
            if not placed:
                groups.append([n.copy(), [(fi, corner)]])
        for avg, members in groups:
            avg = avg / np.linalg.norm(avg)
            index = len(new_pos)
            new_pos.append(verts[vi])
            new_nrm.append(avg)
            for fi, corner in members:
                new_faces[fi, corner] = index
    return np.asarray(new_pos, np.float32), new_faces.astype(np.uint32), np.asarray(new_nrm, np.float32)


def pack_part(name: str, pos: np.ndarray, nrm: np.ndarray, idx: np.ndarray) -> bytes:
    pmin = pos.min(0)
    span = np.maximum(pos.max(0) - pmin, 1e-6)
    q = np.clip(np.round((pos - pmin) / span * 65535), 0, 65535).astype(np.uint16)
    nq = np.clip(np.round(nrm * 127), -127, 127).astype(np.int8)
    name_b = name.encode()
    blob = struct.pack("<B", len(name_b)) + name_b
    if len(blob) % 4:
        blob += b"\x00" * (4 - len(blob) % 4)
    blob += struct.pack("<6f", *pmin.tolist(), *span.tolist())
    blob += struct.pack("<II", len(pos), len(idx))
    blob += q.tobytes()
    blob += nq.tobytes()
    if len(blob) % 4:
        blob += b"\x00" * (4 - len(blob) % 4)
    blob += idx.astype(np.uint32).tobytes()
    return blob


def to_three(cad: np.ndarray) -> np.ndarray:
    out = np.empty_like(cad)
    out[:, 0] = cad[:, 0]
    out[:, 1] = cad[:, 2]
    out[:, 2] = -cad[:, 1]
    return out


def button_centers(lid_tris: np.ndarray) -> np.ndarray:
    v = lid_tris.reshape(-1, 3)
    p = v[(v[:, 2] > 1.2) & (v[:, 2] < 6.5)][:, :2]
    m = (p[:, 0] > 45) & (p[:, 0] < 110) & (p[:, 1] > -15) & (p[:, 1] < 35)
    pts = p[m]
    rng = np.random.default_rng(0)
    cent = pts[rng.choice(len(pts), 4, replace=False)].astype(np.float64)
    for _ in range(40):
        d = ((pts[:, None, :] - cent[None, :, :]) ** 2).sum(2)
        lab = d.argmin(1)
        for i in range(4):
            sel = pts[lab == i]
            if len(sel):
                cent[i] = sel.mean(0)
    return cent


ELEC_GRID = {
    "board": 0.7,
    "esp": 0.7,
    "metal": 0.75,
    "relay": 0.55,
    "cap": 0.45,
    "plastic": 0.6,
    "button": 0.4,
    "other": 1.3,
}


def load_electronics(base_v: np.ndarray):
    """Seat assembly meshes in the same frame as the printed shell."""
    elec = ROOT / "elec"
    meta_path = elec / "case.json"
    if not meta_path.exists():
        return []
    meta = json.loads(meta_path.read_text())
    cmin = np.array(meta["case"]["min"], np.float64)
    cmax = np.array(meta["case"]["max"], np.float64)
    center = (cmin + cmax) / 2
    case_span = np.maximum(cmax - cmin, 1e-6)
    # Shell is Z-up in the STL and already seated: X width, Y height, Z depth.
    bmin = base_v.min(0)
    bmax = base_v.max(0)
    shell = np.maximum(bmax - bmin, 1e-6)
    # Case STEP is Y-up: X width, Y height, Z depth. Map onto the shell box.
    scale = shell / np.array([case_span[0], case_span[1], case_span[2]])
    jack_z = meta.get("jack_z_mean")
    flip_z = jack_z is None or jack_z < center[2]
    # The lid buttons in this assembly sit on the floor, so the viewer keeps
    # the four caps that were fitted to the printed lid. Drop stray solids
    # that landed well outside the case (duplicate placements in the STEP).
    pad = 10.0
    lo = bmin - pad
    hi = bmax + pad
    # The case floor is Y = 0. Parts that landed under it are misplaced.
    lo[1] = bmin[1] - 2.0
    parts = []
    for path in sorted(elec.glob("*.stl")):
        name = path.stem
        if name == "button":
            continue
        tris = load_stl(path)
        if len(tris) < 9:
            continue
        grid = ELEC_GRID.get(name, 0.8)
        verts, faces = cluster(tris, grid)
        seated = np.empty_like(verts)
        seated[:, 0] = (verts[:, 0] - center[0]) * scale[0]
        seated[:, 1] = (verts[:, 1] - cmin[1]) * scale[1]
        z = (verts[:, 2] - center[2]) * scale[2]
        seated[:, 2] = -z if flip_z else z
        # The shell is centred on XZ and sits on Y = 0. Match that.
        seated[:, 0] += (bmin[0] + bmax[0]) / 2
        seated[:, 2] += (bmin[2] + bmax[2]) / 2
        center_tri = seated[faces].mean(1)
        inside = np.all((center_tri >= lo) & (center_tri <= hi), axis=1)
        kept = int(inside.sum())
        print(f"seat {name} faces {len(faces)} kept {kept}")
        faces = faces[inside]
        if len(faces) < 8:
            continue
        used = np.unique(faces.reshape(-1))
        remap = np.full(len(seated), -1, np.int64)
        remap[used] = np.arange(len(used))
        pos, idx, nrm = crease_split(seated[used], remap[faces], cos_limit=0.45)
        parts.append((name, pos, nrm, idx.reshape(-1)))
    return parts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stl-dir", type=Path, default=Path.home() / "Downloads")
    args = parser.parse_args()
    base_tris = load_stl(args.stl_dir / "Base.stl")
    lid_tris = load_stl(args.stl_dir / "Lid.stl")

    base_v, base_f = cluster(base_tris, 0.62)
    lid_v, lid_f = cluster(lid_tris, 0.38)
    print(f"clustered base {len(base_v)}v {len(base_f)}f  lid {len(lid_v)}v {len(lid_f)}f")

    # Sit the shell on Z=0 and centre it in XY. Flip the lid so the outer skin faces up.
    def seat(verts, z0=0.0):
        out = verts.copy()
        out[:, 0] -= (verts[:, 0].min() + verts[:, 0].max()) / 2
        out[:, 1] -= (verts[:, 1].min() + verts[:, 1].max()) / 2
        out[:, 2] -= verts[:, 2].min() - z0
        return out

    base_v = seat(base_v)
    lid_h = lid_v[:, 2].max() - lid_v[:, 2].min()
    lid_v = seat(lid_v)
    lid_v[:, 2] = lid_h - lid_v[:, 2]
    base_top = float(base_v[:, 2].max())
    lid_v[:, 2] += base_top - 1.2

    buttons_cad = button_centers(lid_tris)
    lid_raw = lid_tris.reshape(-1, 3)
    lid_cx = (lid_raw[:, 0].min() + lid_raw[:, 0].max()) / 2
    lid_cy = (lid_raw[:, 1].min() + lid_raw[:, 1].max()) / 2
    lid_zmax = lid_raw[:, 2].max()
    # Same seating as the mesh: centre, flip Z, drop onto the rim.
    b = np.column_stack([
        buttons_cad[:, 0] - lid_cx,
        buttons_cad[:, 1] - lid_cy,
        np.full(4, lid_zmax - 0.15 + base_top - 1.2),
    ])

    base_v = to_three(base_v)
    lid_v = to_three(lid_v)
    b = to_three(b)

    # Drop the closed assembly so the shell rests on Y = 0, still centred in XZ.
    stack = np.vstack([base_v, lid_v])
    shift = np.array([
        -(stack[:, 0].min() + stack[:, 0].max()) / 2,
        -stack[:, 1].min(),
        -(stack[:, 2].min() + stack[:, 2].max()) / 2,
    ])
    base_v += shift
    lid_v += shift
    b += shift

    # Buttons sit just proud of the outer skin.
    b = b.copy()
    b[:, 1] += 1.6

    base_v, base_f, base_n = crease_split(base_v, base_f)
    lid_v, lid_f, lid_n = crease_split(lid_v, lid_f)
    print(f"creased   base {len(base_v)}v {len(base_f)}f  lid {len(lid_v)}v {len(lid_f)}f")

    elec_parts = load_electronics(base_v)
    part_count = 2 + len(elec_parts)
    blob = b"SXM1" + struct.pack("<I", part_count)
    blob += pack_part("base", base_v, base_n, base_f.reshape(-1))
    blob += pack_part("lid", lid_v, lid_n, lid_f.reshape(-1))
    for name, pos, nrm, idx in elec_parts:
        blob += pack_part(name, pos, nrm, idx)
        print(f"elec {name} {len(pos)}v {len(idx)//3}f")

    # 2×2, left to right, row nearer the connector face first.
    # Colours match the finished panel: power, amber, green, blue.
    names = ["power", "record", "link", "bluetooth"]
    colors = ["#f4f1ea", "#ff9a3c", "#3ee07a", "#4aa3ff"]
    zmed = float(np.median(b[:, 2]))
    front = b[b[:, 2] >= zmed][np.argsort(b[b[:, 2] >= zmed][:, 0])]
    back = b[b[:, 2] < zmed][np.argsort(b[b[:, 2] < zmed][:, 0])]
    ordered = np.vstack([front, back])
    buttons = []
    for i, pos in enumerate(ordered):
        buttons.append({
            "id": names[i],
            "color": colors[i],
            "position": [round(float(x), 2) for x in pos],
        })

    meta = {
        "units": "mm",
        "buttons": buttons,
        "extents": {
            "min": [round(float(x), 2) for x in stack.min(0) + shift],
            "max": [round(float(x), 2) for x in stack.max(0) + shift],
        },
    }
    # stack was pre-shift; extents above double-count. Recompute.
    allp = np.vstack([base_v, lid_v])
    meta["extents"] = {
        "min": [round(float(x), 2) for x in allp.min(0)],
        "max": [round(float(x), 2) for x in allp.max(0)],
    }

    out = ROOT / "model.js"
    payload = base64.b64encode(blob).decode()
    out.write_text(
        "/* Generated by prepare_model.py from the printed-case STLs. */\n"
        "window.SXM_MODEL = {\n"
        f"  meta: {json.dumps(meta)},\n"
        f"  b64: \"{payload}\"\n"
        "};\n"
    )
    print(f"wrote {out}  bin {len(blob)/1e6:.2f} MB  js {out.stat().st_size/1e6:.2f} MB")
    print("buttons", json.dumps(buttons))
    print("extents", meta["extents"])


if __name__ == "__main__":
    main()
