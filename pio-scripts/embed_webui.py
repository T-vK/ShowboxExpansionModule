Import("env")

from pathlib import Path
import gzip
import os
import shutil
import struct
import subprocess
import zlib

ROOT = Path(env["PROJECT_DIR"])
UI = ROOT / "ui"
PUBLIC = UI / "public"
HEADER = ROOT / "src" / "WebUi" / "Page.h"
PWA_HEADER = ROOT / "src" / "WebUi" / "PwaAssets.h"
DIST_HTML = UI / "dist" / "index.html"


def newest_mtime(paths):
    latest = 0
    for path in paths:
        if path.is_file():
            latest = max(latest, path.stat().st_mtime)
        elif path.is_dir():
            for child in path.rglob("*"):
                if child.is_file() and "node_modules" not in child.parts and "dist" not in child.parts:
                    latest = max(latest, child.stat().st_mtime)
    return latest


def c_bytes(name, data):
    lines = ["static const uint8_t %s[] PROGMEM = {" % name]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("  " + ", ".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("static const size_t %s_LEN = %d;" % (name, len(data)))
    return "\n".join(lines)


def c_raw_string(name, text, delim):
    if (")%s\"" % delim) in text:
        raise ValueError("raw string delimiter %s appears in %s" % (delim, name))
    return "static const char %s[] = R\"%s(%s)%s\";" % (name, delim, text, delim)


def png_rgb(width, height, pixels):
    def chunk(tag, data):
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        row = pixels[y * width:(y + 1) * width]
        for r, g, b in row:
            raw.extend((r, g, b))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def make_icon(size):
    bg = (11, 12, 16)
    panel = (22, 24, 30)
    groove = (7, 8, 10)
    cap = (90, 97, 112)
    hair = (232, 230, 223)
    accent = (78, 196, 212)
    pix = [bg] * (size * size)

    def setp(x, y, color):
        if 0 <= x < size and 0 <= y < size:
            pix[y * size + x] = color

    def fill(x0, y0, x1, y1, color):
        for y in range(max(0, y0), min(size, y1)):
            for x in range(max(0, x0), min(size, x1)):
                setp(x, y, color)

    margin = max(8, size // 12)
    fill(margin, margin, size - margin, size - margin, panel)
    slots = 4
    levels = (0.78, 0.42, 0.64, 0.26)
    inner = size - 4 * margin
    for i, level in enumerate(levels):
        x = margin * 2 + int(inner * (i + 0.5) / slots)
        gw = max(2, size // 48)
        fill(x - gw, margin * 2, x + gw + 1, size - margin * 2, groove)
        cy = int(margin * 2 + (1.0 - level) * (size - 4 * margin))
        ch = max(6, size // 13)
        cw = max(7, size // 9)
        color = accent if i == 2 else cap
        fill(x - cw, cy - ch // 2, x + cw, cy + ch // 2, color)
        fill(x - cw + 1, cy - max(1, size // 180), x + cw - 1, cy + max(1, size // 180), hair)
    return png_rgb(size, size, pix)


def write_header(gz_bytes):
    HEADER.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "static const uint8_t WEBUI_INDEX_GZ[] PROGMEM = {",
    ]
    for i in range(0, len(gz_bytes), 16):
        chunk = gz_bytes[i:i + 16]
        lines.append("  " + ", ".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("static const size_t WEBUI_INDEX_GZ_LEN = %d;" % len(gz_bytes))
    lines.append("")
    HEADER.write_text("\n".join(lines), encoding="utf-8")
    print("Embedded Web UI: %d bytes gzipped -> %s" % (len(gz_bytes), HEADER))


def write_pwa_assets():
    PUBLIC.mkdir(parents=True, exist_ok=True)
    icons = {
        PUBLIC / "icon-192.png": 192,
        PUBLIC / "icon-512.png": 512,
        PUBLIC / "apple-touch-icon.png": 180,
    }
    for path, size in icons.items():
        if not path.is_file():
            path.write_bytes(make_icon(size))
            print("Wrote PWA icon %s" % path)
    manifest = (PUBLIC / "manifest.webmanifest").read_text(encoding="utf-8")
    sw = (PUBLIC / "sw.js").read_text(encoding="utf-8")
    icon192 = (PUBLIC / "icon-192.png").read_bytes()
    icon512 = (PUBLIC / "icon-512.png").read_bytes()
    icon180 = (PUBLIC / "apple-touch-icon.png").read_bytes()
    parts = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        c_raw_string("PWA_MANIFEST", manifest, "MAN"),
        "",
        c_raw_string("PWA_SW", sw, "SJS"),
        "",
        c_bytes("PWA_ICON_192", icon192),
        "",
        c_bytes("PWA_ICON_512", icon512),
        "",
        c_bytes("PWA_ICON_180", icon180),
        "",
    ]
    PWA_HEADER.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(parts)
    if not PWA_HEADER.is_file() or PWA_HEADER.read_text(encoding="utf-8") != text:
        PWA_HEADER.write_text(text, encoding="utf-8")
        print("Embedded PWA assets -> %s" % PWA_HEADER)


def build_ui():
    write_pwa_assets()
    npm = shutil.which("npm")
    if npm is None:
        print("npm not found; using existing Web UI header if present")
        return HEADER.is_file()
    sources_mtime = newest_mtime([
        UI / "package.json",
        UI / "package-lock.json",
        UI / "vite.config.js",
        UI / "index.html",
        UI / "src",
        UI / "public",
    ])
    if HEADER.is_file() and HEADER.stat().st_mtime >= sources_mtime and sources_mtime > 0:
        print("Web UI header is current")
        return True
    env_local = os.environ.copy()
    env_local["npm_config_fund"] = "false"
    if not (UI / "node_modules").is_dir():
        print("Installing Web UI npm packages")
        subprocess.check_call([npm, "ci" if (UI / "package-lock.json").is_file() else "install"], cwd=UI, env=env_local)
    print("Building Web UI")
    subprocess.check_call([npm, "run", "build"], cwd=UI, env=env_local)
    html = DIST_HTML.read_bytes()
    write_header(gzip.compress(html, compresslevel=9))
    return True


if not build_ui():
    raise SystemExit("Web UI header is missing and npm could not build it")
