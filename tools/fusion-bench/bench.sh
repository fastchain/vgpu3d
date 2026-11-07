#!/bin/bash
# Fusion viewport benchmark over the in-VM Fusion MCP server (127.0.0.1:27182, tunnelled over the VM ssh).
# Needs VGPU_SSH = ssh arguments that reach the guest, e.g. VGPU_SSH='-i ~/.ssh/<key> -p <port> <user>@<host>'.
# usage: bench.sh load     - new document with the 2.5M-triangle scene (fusion_big.py; models in %USERPROFILE%\Downloads\models)
#        bench.sh check    - triangle count of the active document
#        bench.sh orbit    - 72-step scripted orbit, prints ORBIT avg/median/p90/fps
set -eu
D=$(cd "$(dirname "$0")" && pwd)
T=${CLAUDE_JOB_DIR:-/tmp}/fusion-bench; mkdir -p "$T"
H=(-H 'Host: 127.0.0.1:27182' -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream')
pgrep -f "27182:127.0.0.1:27182" >/dev/null ||
  ssh -f -N -o ExitOnForwardFailure=yes ${VGPU_SSH:?set VGPU_SSH to the guest ssh arguments} -L 27182:127.0.0.1:27182
curl -s -D "$T/h.txt" -o /dev/null "${H[@]}" --max-time 20 http://127.0.0.1:27182/mcp \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"claude","version":"1"}}}'
SID=$(grep -i '^mcp-session-id' "$T/h.txt" | awk '{print $2}' | tr -d '\r')
curl -s -o /dev/null "${H[@]}" -H "Mcp-Session-Id: $SID" http://127.0.0.1:27182/mcp -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'

case "${1:-orbit}" in
  load) py=fusion_big.py; ro=False ;;
  check) py=fusion_check.py; ro=True ;;
  orbit) py=fusion_orbit.py; ro=True ;;
  *) echo "usage: $0 load|check|orbit" >&2; exit 2 ;;
esac
python3 -I - "$D/$py" "$ro" "$T/req.json" <<'EOF'
import json, sys
s = open(sys.argv[1]).read()
json.dump({"jsonrpc": "2.0", "id": 7, "method": "tools/call",
           "params": {"name": "fusion_mcp_execute",
                      "arguments": {"featureType": "script", "object": {"script": s, "readOnly": sys.argv[2] == "True"}}}},
          open(sys.argv[3], "w"))
EOF
curl -s --max-time 900 "${H[@]}" -H "Mcp-Session-Id: $SID" http://127.0.0.1:27182/mcp --data @"$T/req.json" |
  sed -n 's/^data: //p;/^{/p' | python3 -I -c '
import json, re, sys
t = json.loads(sys.stdin.read())["result"]["content"][0]["text"]
try:
    e = json.loads(t).get("error", "")
except ValueError:
    e = ""
m = re.search(r"Exception: (.*)", e, re.S)
print(m.group(1).strip()[:600] if m else t[:600])'
