#!/usr/bin/env bash
# beforeShellExecution: strip Co-authored-by / cursoragent trailers from git commit commands.
set -euo pipefail
input=$(cat)

python3 - "$input" <<'PY'
import json, re, sys

payload = json.loads(sys.argv[1])
cmd = payload.get("command") or ""

if not re.search(r"\bgit\b", cmd) or not re.search(r"\bcommit\b", cmd):
    print(json.dumps({"permission": "allow"}))
    raise SystemExit(0)

if not re.search(r"Co-authored-by:|cursoragent@cursor\.com", cmd, re.I):
    print(json.dumps({"permission": "allow"}))
    raise SystemExit(0)

cmd2 = re.sub(r"(?im)^[ \t]*Co-authored-by:[^\n]*\n?", "", cmd)
cmd2 = re.sub(r"(?im)\n[ \t]*Co-authored-by:[^\n]*", "", cmd2)
cmd2 = re.sub(r"(?im)^[ \t]*Signed-off-by:[^\n]*(?:cursoragent|Cursor)[^\n]*\n?", "", cmd2)
cmd2 = re.sub(r"(?im)\n[ \t]*Signed-off-by:[^\n]*(?:cursoragent|Cursor)[^\n]*", "", cmd2)
cmd2 = re.sub(r"\n{3,}(EOF\b)", r"\n\n\1", cmd2)

print(json.dumps({
    "permission": "allow",
    "agent_message": "Stripped Co-authored-by / cursoragent trailers from git commit command.",
    "updated_input": {"command": cmd2},
}))
PY
