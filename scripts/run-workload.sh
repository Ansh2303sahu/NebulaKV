#!/usr/bin/env bash
set -euo pipefail
BUILD_DIR=${BUILD_DIR:-build/distributed-release}
"$BUILD_DIR/nebulakv-benchmark" \
  --host "${HOST:-127.0.0.1}" \
  --port "${PORT:-5001}" \
  --duration-seconds "${DURATION_SECONDS:-30}" \
  --clients "${CLIENTS:-16}" \
  --keyspace "${KEYSPACE:-100000}" \
  --value-bytes "${VALUE_BYTES:-256}" \
  --read-ratio "${READ_RATIO:-0.8}"
