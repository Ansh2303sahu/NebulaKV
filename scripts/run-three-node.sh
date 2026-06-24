#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${BUILD_DIR:-build/distributed-release}
DATA_ROOT=${DATA_ROOT:-data/cluster}
LOG_ROOT=${LOG_ROOT:-logs}
mkdir -p "$DATA_ROOT" "$LOG_ROOT"

declare -a STARTED_PIDS=()
startup_complete=false

port_is_listening() {
  local port=$1
  if ss -H -ltn 2>/dev/null \
      | awk -v suffix=":${port}" '$4 ~ suffix "$" {found=1} END {exit(found ? 0 : 1)}'; then
    return 0
  fi
  timeout 0.25 bash -c "exec 3<>/dev/tcp/127.0.0.1/${port}" \
    >/dev/null 2>&1
}

show_port_owner() {
  local port=$1
  ss -ltnp 2>/dev/null | awk -v suffix=":${port}" 'NR == 1 || $4 ~ suffix "$"'
}

cleanup_partial_start() {
  local pid
  for pid in "${STARTED_PIDS[@]:-}"; do
    [[ -n "$pid" ]] || continue
    kill "$pid" 2>/dev/null || true
  done
  for pid in "${STARTED_PIDS[@]:-}"; do
    [[ -n "$pid" ]] || continue
    wait "$pid" 2>/dev/null || true
  done
  rm -f "$LOG_ROOT"/node-*.pid
}

on_exit() {
  local status=$?
  if [[ "$startup_complete" != true ]]; then
    cleanup_partial_start
  fi
  return "$status"
}
trap on_exit EXIT

if [[ ! -x "$BUILD_DIR/nebulakv-node" || ! -x "$BUILD_DIR/nebulakv-cli" ]]; then
  echo "Distributed binaries were not found under: $BUILD_DIR" >&2
  echo "Build first with: cmake --preset distributed-release && cmake --build --preset distributed-release" >&2
  exit 1
fi

for port in 5001 5002 5003 9101 9102 9103; do
  if port_is_listening "$port"; then
    echo "Cannot start NebulaKV: TCP port $port is already in use." >&2
    show_port_owner "$port" >&2 || true
    echo "Stop the previous cluster and any stale nebulakv-server process before retrying." >&2
    exit 1
  fi
done

start_node() {
  local id=$1 port=$2 metrics=$3 peer1=$4 peer2=$5
  "$BUILD_DIR/nebulakv-node" \
    --node-id "$id" \
    --listen "127.0.0.1:$port" \
    --advertise "127.0.0.1:$port" \
    --peer "$peer1" --peer "$peer2" \
    --data-dir "$DATA_ROOT/$id" \
    --metrics-host 127.0.0.1 \
    --metrics-port "$metrics" \
    >"$LOG_ROOT/$id.jsonl" 2>&1 &
  local pid=$!
  STARTED_PIDS+=("$pid")
  echo "$pid" >"$LOG_ROOT/$id.pid"
}

wait_for_identity() {
  local id=$1 port=$2 pid=$3
  local output=""
  for _ in $(seq 1 100); do
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "$id exited during startup." >&2
      cat "$LOG_ROOT/$id.jsonl" >&2 || true
      return 1
    fi
    output=$("$BUILD_DIR/nebulakv-cli" \
      --host 127.0.0.1 --port "$port" --timeout-ms 300 status \
      2>/dev/null || true)
    if grep -qx "node_id=$id" <<<"$output" \
        && grep -Eq '^raft_role=(follower|candidate|leader)$' <<<"$output"; then
      return 0
    fi
    sleep 0.1
  done
  echo "$id did not become reachable with the expected distributed identity." >&2
  echo "Last status response:" >&2
  printf '%s\n' "$output" >&2
  cat "$LOG_ROOT/$id.jsonl" >&2 || true
  return 1
}

start_node node-1 5001 9101 node-2=127.0.0.1:5002 node-3=127.0.0.1:5003
start_node node-2 5002 9102 node-1=127.0.0.1:5001 node-3=127.0.0.1:5003
start_node node-3 5003 9103 node-1=127.0.0.1:5001 node-2=127.0.0.1:5002

wait_for_identity node-1 5001 "$(cat "$LOG_ROOT/node-1.pid")"
wait_for_identity node-2 5002 "$(cat "$LOG_ROOT/node-2.pid")"
wait_for_identity node-3 5003 "$(cat "$LOG_ROOT/node-3.pid")"

startup_complete=true
trap - EXIT

echo "Three NebulaKV distributed nodes started and verified."
echo "Logs and PID files are under ./$LOG_ROOT."
echo "Run scripts/stop-three-node.sh before starting another local cluster."
