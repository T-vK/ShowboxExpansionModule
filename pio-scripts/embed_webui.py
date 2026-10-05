Import("env")

from pathlib import Path
import gzip
import os
import shutil
import subprocess

ROOT = Path(env["PROJECT_DIR"])
UI = ROOT / "ui"
HEADER = ROOT / "src" / "WebUi" / "Page.h"
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


def build_ui():
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
