#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCHMARK_BINARY="${ROOT_DIR}/build/bin/ps_transport_benchmark"
RUNNER_SCRIPT="${ROOT_DIR}/src/test/scripts/run_rdma_transport_benchmarks.py"

if [[ ! -x "${BENCHMARK_BINARY}" ]]; then
  echo "benchmark binary not found: ${BENCHMARK_BINARY}" >&2
  echo "build it first:" >&2
  echo "  cmake --build ${ROOT_DIR}/build --target ps_transport_benchmark" >&2
  exit 1
fi

cd "${ROOT_DIR}"

exec python3 "${RUNNER_SCRIPT}" \
  --benchmark-binary "${BENCHMARK_BINARY}" \
  --iterations 300 \
  --batch-keys 500 \
  --rounds 10 \
  --rdma-warmup-rounds 2 \
  --report-mode summary \
  --rdma-only \
  --rdma-thread-num 1 \
  --rdma-put-protocol-version 2 \
  --rdma-put-v2-transfer-mode read \
  --rdma-wait-timeout-ms 20000 \
  --rdma-client-timeout-sec 60 \
  --show-runner-logs \
  --use-local-memcached auto \
  "$@"
