#!/usr/bin/env bash
set -euo pipefail
for file in logs/node-*.pid; do
  [[ -e "$file" ]] || continue
  kill "$(cat "$file")" 2>/dev/null || true
done
wait 2>/dev/null || true
rm -f logs/node-*.pid
echo "NebulaKV cluster stopped."
