Fusion MCP benchmark helpers (the Fusion "MCP Server Adapter" listens on the guest's 127.0.0.1:27182).
Tunnel: ssh -i ~/.ssh/win11_vm -p 4568 -N -L 127.0.0.1:27182:127.0.0.1:27182 User@10.0.0.1
Session: POST initialize with Host: 127.0.0.1:27182, keep Mcp-Session-Id in /tmp/claude-1000/mcp_sid.txt.
fusion_big.py  - new doc with 9x xyzrgb_dragon + lucy + happy + armadillo (2.55M tris; OBJs in C:\Users\User\Downloads\models)
fusion_check.py- read-only: list mesh bodies + triangle counts (result comes back in the raised exception)
fusion_orbit.py- read-only: 72-step camera orbit with viewport refresh, reports ms/frame
Scripts must stay < ~4 KB, must not print(), and mutating scripts must not raise.
