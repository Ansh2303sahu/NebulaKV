#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${BUILD_DIR:-build/distributed-release}
DATA_ROOT=${DATA_ROOT:-data/cluster}
mkdir -p "$DATA_ROOT" logs

start() {
  local id=$1 port=$2 metrics=$3 peer1=$4 peer2=$5
  "$BUILD_DIR/nebulakv-node" \
    --node-id "$id" \
    --listen "0.0.0.0:$port" \
    --advertise "127.0.0.1:$port" \
    --peer "$peer1" --peer "$peer2" \
    --data-dir "$DATA_ROOT/$id" \
    --metrics-port "$metrics" \
    >"logs/$id.jsonl" 2>&1 &
  echo $! >"logs/$id.pid"
}

start node-1 5001 9101 node-2=127.0.0.1:5002 node-3=127.0.0.1:5003
start node-2 5002 9102 node-1=127.0.0.1:5001 node-3=127.0.0.1:5003
start node-3 5003 9103 node-1=127.0.0.1:5001 node-2=127.0.0.1:5002

echo "Three NebulaKV nodes started. Logs and PID files are under ./logs."
