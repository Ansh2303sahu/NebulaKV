#!/usr/bin/env bash
set -euo pipefail

LOG_ROOT=${LOG_ROOT:-logs}
declare -a PIDS=()

for file in "$LOG_ROOT"/node-*.pid; do
  [[ -e "$file" ]] || continue
  pid=$(cat "$file")
  [[ "$pid" =~ ^[0-9]+$ ]] || continue
  PIDS+=("$pid")
  kill -TERM "$pid" 2>/dev/null || true
done

for _ in $(seq 1 50); do
  remaining=false
  for pid in "${PIDS[@]:-}"; do
    [[ -n "$pid" ]] || continue
    if kill -0 "$pid" 2>/dev/null; then
      remaining=true
      break
    fi
  done
  [[ "$remaining" == false ]] && break
  sleep 0.1
done

for pid in "${PIDS[@]:-}"; do
  [[ -n "$pid" ]] || continue
  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
done

for pid in "${PIDS[@]:-}"; do
  [[ -n "$pid" ]] || continue
  wait "$pid" 2>/dev/null || true
done

rm -f "$LOG_ROOT"/node-*.pid

echo "NebulaKV cluster processes from ./$LOG_ROOT were stopped."
