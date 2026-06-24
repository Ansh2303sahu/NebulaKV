#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${1:-build/distributed-release}
ROOT=$(mktemp -d)
declare -A PIDS PORTS METRICS PEER_ONE PEER_TWO

PORTS[node-1]=5501
PORTS[node-2]=5502
PORTS[node-3]=5503
METRICS[node-1]=9501
METRICS[node-2]=9502
METRICS[node-3]=9503
PEER_ONE[node-1]=node-2=127.0.0.1:5502
PEER_TWO[node-1]=node-3=127.0.0.1:5503
PEER_ONE[node-2]=node-1=127.0.0.1:5501
PEER_TWO[node-2]=node-3=127.0.0.1:5503
PEER_ONE[node-3]=node-1=127.0.0.1:5501
PEER_TWO[node-3]=node-2=127.0.0.1:5502

cleanup() {
  for id in node-1 node-2 node-3; do
    if [[ -n "${PIDS[$id]:-}" ]]; then
      kill "${PIDS[$id]}" 2>/dev/null || true
    fi
  done
  wait 2>/dev/null || true
  rm -rf "$ROOT"
}
trap cleanup EXIT

start_node() {
  local id=$1
  "$BUILD_DIR/nebulakv-node" \
    --node-id "$id" \
    --listen "127.0.0.1:${PORTS[$id]}" \
    --advertise "127.0.0.1:${PORTS[$id]}" \
    --peer "${PEER_ONE[$id]}" \
    --peer "${PEER_TWO[$id]}" \
    --data-dir "$ROOT/$id" \
    --metrics-host 127.0.0.1 \
    --metrics-port "${METRICS[$id]}" \
    --election-min-ms 180 \
    --election-max-ms 360 \
    --heartbeat-ms 40 \
    --rpc-timeout-ms 100 \
    --snapshot-threshold 8 \
    >"$ROOT/$id.log" 2>&1 &
  PIDS[$id]=$!
}

stop_node() {
  local id=$1
  if [[ -n "${PIDS[$id]:-}" ]]; then
    kill "${PIDS[$id]}" 2>/dev/null || true
    wait "${PIDS[$id]}" 2>/dev/null || true
    PIDS[$id]=""
  fi
}

status_for() {
  local id=$1
  "$BUILD_DIR/nebulakv-cli" \
    --host 127.0.0.1 \
    --port "${PORTS[$id]}" \
    --timeout-ms 500 \
    status 2>/dev/null || true
}

find_leader() {
  local excluded=${1:-}
  for _ in $(seq 1 100); do
    for id in node-1 node-2 node-3; do
      [[ "$id" == "$excluded" ]] && continue
      if grep -q '^raft_role=leader$' <<<"$(status_for "$id")"; then
        echo "$id"
        return 0
      fi
    done
    sleep 0.1
  done
  return 1
}

for id in node-1 node-2 node-3; do
  start_node "$id"
done

leader=$(find_leader) || {
  echo "No leader elected" >&2
  cat "$ROOT"/*.log >&2
  exit 1
}

"$BUILD_DIR/nebulakv-cli" --host 127.0.0.1 \
  --port "${PORTS[$leader]}" --timeout-ms 3000 \
  put smoke:key smoke-value >/dev/null
"$BUILD_DIR/nebulakv-cli" --host 127.0.0.1 \
  --port "${PORTS[node-3]}" --timeout-ms 3000 \
  get smoke:key | grep -q '^value=smoke-value$'

curl --fail --silent "http://127.0.0.1:${METRICS[$leader]}/metrics" \
  | grep -q 'nebulakv_raft_term'

lagging=node-3
if [[ "$leader" == "$lagging" ]]; then
  lagging=node-2
fi
stop_node "$lagging"

for number in $(seq 1 12); do
  "$BUILD_DIR/nebulakv-cli" --host 127.0.0.1 \
    --port "${PORTS[$leader]}" --timeout-ms 3000 \
    put "snapshot:key:$number" "value-$number" >/dev/null
done

start_node "$lagging"
snapshot_caught_up=false
for _ in $(seq 1 120); do
  value=$("$BUILD_DIR/nebulakv-cli" --host 127.0.0.1 \
    --port "${PORTS[$lagging]}" --timeout-ms 1000 \
    get snapshot:key:12 2>/dev/null || true)
  snapshot_index=$(status_for "$lagging" | awk -F= '/^raft_snapshot_index=/ {print $2}')
  if grep -q '^value=value-12$' <<<"$value" \
      && [[ "${snapshot_index:-0}" =~ ^[0-9]+$ ]] \
      && (( snapshot_index > 0 )); then
    snapshot_caught_up=true
    break
  fi
  sleep 0.1
done

if [[ "$snapshot_caught_up" != true ]]; then
  echo "Restarted follower did not catch up through a snapshot" >&2
  cat "$ROOT"/*.log >&2
  exit 1
fi

initial_leader=$leader
stop_node "$initial_leader"
replacement=$(find_leader "$initial_leader") || {
  echo "No replacement leader elected" >&2
  cat "$ROOT"/*.log >&2
  exit 1
}

survivor=node-1
if [[ "$survivor" == "$initial_leader" ]]; then
  survivor=node-2
fi

"$BUILD_DIR/nebulakv-cli" --host 127.0.0.1 \
  --port "${PORTS[$survivor]}" --timeout-ms 3000 \
  put failover:key survived >/dev/null
"$BUILD_DIR/nebulakv-cli" --host 127.0.0.1 \
  --port "${PORTS[$replacement]}" --timeout-ms 3000 \
  get failover:key | grep -q '^value=survived$'

printf 'cluster_smoke=passed initial_leader=%s replacement_leader=%s snapshot_follower=%s\n' \
  "$initial_leader" "$replacement" "$lagging"
