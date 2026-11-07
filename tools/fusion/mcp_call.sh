#!/bin/bash
# usage: mcp_call.sh <request.json>   (JSON-RPC body); prints the JSON result (SSE data lines stripped)
SID=$(cat /tmp/claude-1000/mcp_sid.txt)
curl -s --max-time ${MCP_TIMEOUT:-300} -H 'Host: 127.0.0.1:27182' -H "Mcp-Session-Id: $SID" -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' http://127.0.0.1:27182/mcp --data @"$1" | sed -n 's/^data: //p;/^{/p'
