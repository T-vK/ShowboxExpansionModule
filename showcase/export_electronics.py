"""Tessellate the assembly electronics in world coordinates.

Run with:
  freecadcmd showcase/export_electronics.py

Writes showcase/elec/*.stl in the STEP assembly frame, plus case.json
with the case bounding box so prepare_model.py can seat them in the viewer.
"""

import json
from pathlib import Path

import FreeCAD as App
import Import
import Mesh
import MeshPart

STEP = Path("/home/tvk/Projects/MackieShowboxReverseEngineering/3dModel/ShowboxExpansionModule/ShowboxExpansionModule v1.step")
OUT = Path(__file__).resolve().parent / "elec"

SKIP_BITS = (
    "0402", "0603", "0805", "r0402", "c0402", "c1206", "sot-223", "qfn",
    "m1.6", "din965", "iso_10642", "m3,0", "ws2812", "yc164",
)
SHELL = ("case", "lid", "raiser 1", "raiser 2")


def label_chain(obj) -> str:
    names = [obj.Label.lower()]
    parent = obj.getParent() if hasattr(obj, "getParent") else None
    seen = set()
    while parent is not None and getattr(parent, "Name", None) not in seen:
        seen.add(parent.Name)
        names.append(parent.Label.lower())
        parent = parent.getParent() if hasattr(parent, "getParent") else None
    return " ".join(names)


def own_label(chain: str) -> str:
    for name in SHELL:
        if chain == name or chain.startswith(name + " "):
            return name
    return chain


def world_shape(obj):
    return obj.Shape.transformed(obj.getGlobalPlacement().toMatrix())


def classify(chain: str):
    if own_label(chain) in SHELL:
        return None
    if any(bit in chain for bit in SKIP_BITS):
        return None
    if "push button" in chain:
        return "button"
    if "relay" in chain:
        return "relay"
    if any(k in chain for k in ("esp32", "wroom", "esp-s3")):
        return "esp"
    if any(k in chain for k in ("cap_lg", "capacitor", "e-cap", "cap_sm")):
        return "cap"
    if any(k in chain for k in ("jack", "sma", "usb", "header", "pin", "rj45", "midi", "2.1mm")):
        return "metal"
    if any(k in chain for k in ("pcb", "w5500", "breakout", "sdmpcb", "39 x 84")):
        return "board"
    if any(k in chain for k in ("housing", "clamp", "terminal", "4pin")):
        return "plastic"
    return "other"


def leaves(obj):
    kids = list(getattr(obj, "Group", None) or [])
    if kids:
        for kid in kids:
            yield from leaves(kid)
        return
    shape = getattr(obj, "Shape", None)
    if shape is not None and not shape.isNull() and shape.Solids:
        yield obj


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    doc = App.newDocument("asm")
    print("importing", STEP, flush=True)
    Import.insert(str(STEP), "asm")
    doc.recompute()
    objs = []
    for obj in doc.Objects:
        objs.extend(leaves(obj))
    print("leaves", len(objs), flush=True)

    case_box = None
    for obj in objs:
        if own_label(label_chain(obj)) == "case":
            case_box = world_shape(obj).BoundBox
            print(
                "case",
                round(case_box.XLength, 1),
                round(case_box.YLength, 1),
                round(case_box.ZLength, 1),
                "min",
                round(case_box.XMin, 1),
                round(case_box.YMin, 1),
                round(case_box.ZMin, 1),
                flush=True,
            )
            break
    if case_box is None:
        raise SystemExit("Case solid not found")

    groups = {name: Mesh.Mesh() for name in ("board", "esp", "metal", "relay", "cap", "plastic", "button", "other")}
    jack_z = []
    button_pts = []
    kept = 0
    for obj in objs:
        chain = label_chain(obj)
        kind = classify(chain)
        if kind is None:
            continue
        shape = world_shape(obj)
        bb = shape.BoundBox
        span = max(bb.XLength, bb.YLength, bb.ZLength)
        if span < 4.0 and kind not in ("button", "relay"):
            continue
        try:
            piece = MeshPart.meshFromShape(
                Shape=shape, LinearDeflection=0.9, AngularDeflection=0.6, Relative=False
            )
        except Exception as exc:
            print("mesh fail", obj.Label, exc, flush=True)
            continue
        groups[kind].addMesh(piece)
        kept += 1
        if "jack body" in chain:
            jack_z.append((bb.ZMin + bb.ZMax) / 2)
        if kind == "button":
            button_pts.append([
                round((bb.XMin + bb.XMax) / 2, 2),
                round((bb.YMin + bb.YMax) / 2, 2),
                round((bb.ZMin + bb.ZMax) / 2, 2),
            ])
        if kept % 40 == 0:
            print("kept", kept, obj.Label[:40], kind, flush=True)

    for name, mesh in groups.items():
        if mesh.CountFacets == 0:
            print(name, "empty", flush=True)
            continue
        path = OUT / f"{name}.stl"
        mesh.write(str(path))
        print(name, "facets", mesh.CountFacets, flush=True)

    meta = {
        "case": {
            "min": [case_box.XMin, case_box.YMin, case_box.ZMin],
            "max": [case_box.XMax, case_box.YMax, case_box.ZMax],
        },
        "jack_z_mean": (sum(jack_z) / len(jack_z)) if jack_z else None,
        "buttons": button_pts,
    }
    (OUT / "case.json").write_text(json.dumps(meta))
    print("kept", kept, "jacks", len(jack_z), "buttons", len(button_pts), flush=True)
    print("done", flush=True)


main()
