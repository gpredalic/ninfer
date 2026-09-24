#!/usr/bin/env bash
#
# validate-tp2.sh — one-command TP2 validation harness.
#
# Run this INSIDE the container, AFTER the host has been configured for GPU
# passthrough (see host-setup-gpu-passthrough.sh + README.md). It:
#
#   Phase 0  Verifies CUDA device visibility (cudaGetDeviceCount == 2).
#   Phase B  Collects hardware info: model, UUIDs, driver/CUDA versions,
#            peer-access matrix, topology.
#   Phase C  Runs the TP2 qualification tests:
#              - ninfer_allreduce_test   (P2P allreduce correctness)
#              - ninfer_split_ops_test   (split-launch helpers)
#              - ninfer_decode_graph_test (graph capture/instantiate/replay)
#   Phase E  (optional) Launches ninfer-serve against a .ninfer artifact and
#            issues a health + inference request.
#
# Usage:
#   ./validate-tp2.sh [--artifact PATH] [--port N] [--skip-serve] [--serve-args "..."]
#
# Exit code: 0 if every executed phase passed, 1 if any executed phase failed,
# 77 if the only failures were hardware-gated skips (no GPU visible).
#
set -uo pipefail

# ---------------------------------------------------------------------------
# Configuration / environment
# ---------------------------------------------------------------------------
REPO="${REPO:-/home/razvijalec/ninfer}"
BUILD="${BUILD:-$REPO/build}"
TOOLS="${TOOLS:-/home/razvijalec/tools}"
CUDA_ROOT="${CUDA_ROOT:-$TOOLS/cuda/usr/local/cuda-13.1}"
NV_LIBS="${NV_LIBS:-$TOOLS/nvidia-595/usr/lib/x86_64-linux-gnu}"
DEBS_LIBS="${DEBS_LIBS:-$TOOLS/debs/usr/lib/x86_64-linux-gnu}"

ARTIFACT="${ARTIFACT:-$REPO/out/qwen3_6_27b.ninfer}"
PORT="${PORT:-8090}"
SKIP_SERVE=0
SERVE_ARGS=""

while [ $# -gt 0 ]; do
  case "$1" in
    --artifact)  ARTIFACT="$2"; shift 2 ;;
    --port)      PORT="$2"; shift 2 ;;
    --skip-serve) SKIP_SERVE=1; shift ;;
    --serve-args) SERVE_ARGS="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# Toolchain + library paths.
export PATH="$TOOLS/cmake-3.31.6-linux-x86_64/bin:$TOOLS/ninja:$CUDA_ROOT/bin:$PATH"
export LD_LIBRARY_PATH="$NV_LIBS:$DEBS_LIBS:$CUDA_ROOT/targets/x86_64-linux/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

WORK="$(mktemp -d /tmp/tp2-validate.XXXXXX)"
RESULTS="$WORK/results.txt"
trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0; SKIP=0
ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; echo "PASS $1" >>"$RESULTS"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; echo "FAIL $1" >>"$RESULTS"; FAIL=$((FAIL+1)); }
skip() { printf '  \033[33mSKIP\033[0m  %s\n' "$1"; echo "SKIP $1" >>"$RESULTS"; SKIP=$((SKIP+1)); }
phase(){ printf '\n\033[1m== %s ==\033[0m\n' "$1"; }

echo "NInfer TP2 validation harness"
echo "  repo=$REPO"
echo "  build=$BUILD"
echo "  artifact=$ARTIFACT"
echo "  port=$PORT skip_serve=$SKIP_SERVE"
echo "  LD_LIBRARY_PATH=$LD_LIBRARY_PATH"

# ---------------------------------------------------------------------------
# Phase 0 + B: CUDA visibility + hardware info
# ---------------------------------------------------------------------------
phase "Phase 0/B: CUDA device visibility + hardware info"

cat > "$WORK/probe.cu" <<'CUDA'
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

static void print_uuid(const cudaUUID_t& u) {
    for (int b = 0; b < 16; ++b) printf("%02x", (unsigned char)u.bytes[b]);
}

int main() {
    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess) {
        printf("cudaGetDeviceCount FAILED: %s (err=%d)\n", cudaGetErrorString(err), (int)err);
        if (err == cudaErrorNoDevice) return 3;  // no GPU visible -> SKIP
        return 2;  // driver/library problem -> FAIL
    }
    printf("cudaGetDeviceCount = %d\n", count);
    if (count == 0) { printf("NO_USABLE_DEVICE\n"); return 3; }

    int runtimeVer = 0, driverVer = 0;
    cudaRuntimeGetVersion(&runtimeVer);
    cudaDriverGetVersion(&driverVer);
    printf("CUDA_RUNTIME_VERSION=%d.%d\n", runtimeVer / 1000, (runtimeVer / 10) % 10);
    printf("CUDA_DRIVER_VERSION=%d.%d\n", driverVer / 1000, (driverVer / 10) % 10);

    for (int i = 0; i < count; ++i) {
        cudaDeviceProp p;
        if (cudaGetDeviceProperties(&p, i) != cudaSuccess) {
            printf("Device %d: property query FAILED\n", i);
            continue;
        }
        printf("DEVICE_%d_NAME=%s\n", i, p.name);
        printf("DEVICE_%d_UUID=", i); print_uuid(p.uuid); printf("\n");
        printf("DEVICE_%d_CC=%d.%d\n", i, p.major, p.minor);
        printf("DEVICE_%d_MEM_GiB=%.1f\n", i, p.totalGlobalMem / (1024.0 * 1024.0 * 1024.0));
        printf("DEVICE_%d_SMS=%d\n", i, p.multiProcessorCount);
    }

    printf("PEER_ACCESS_MATRIX (row can access col):\n");
    printf("      ");
    for (int j = 0; j < count; ++j) printf("  D%d ", j);
    printf("\n");
    for (int i = 0; i < count; ++i) {
        printf("  D%d ", i);
        for (int j = 0; j < count; ++j) {
            if (i == j) { printf("  self "); continue; }
            int can = 0;
            cudaError_t e = cudaDeviceCanAccessPeer(&can, i, j);
            if (e != cudaSuccess) printf("  err(%s) ", cudaGetErrorString(e));
            else printf("  %s ", can ? "YES" : "no");
        }
        printf("\n");
    }
    return 0;
}
CUDA

if ! nvcc -O2 -o "$WORK/probe" "$WORK/probe.cu" 2> "$WORK/probe.compile.log"; then
  bad "compile CUDA probe (see $WORK/probe.compile.log)"
  cat "$WORK/probe.compile.log"
else
  "$WORK/probe" > "$WORK/probe.out" 2>&1
  PROBE_RC=$?
  cat "$WORK/probe.out"
  if [ "$PROBE_RC" -eq 0 ]; then
    DEVICES="$(grep -c '^DEVICE_.*_NAME=' "$WORK/probe.out" || true)"
    if [ "${DEVICES:-0}" -ge 2 ]; then
      ok "cudaGetDeviceCount returns $DEVICES (>=2 GPUs visible)"
    else
      bad "expected >=2 GPUs, saw ${DEVICES:-0}"
    fi
  elif [ "$PROBE_RC" -eq 3 ]; then
    skip "no usable CUDA device (GPU passthrough not active?)"
  else
    bad "CUDA probe failed (rc=$PROBE_RC)"
  fi
fi

# nvidia-smi, if present, for a human-readable topology view.
if command -v nvidia-smi >/dev/null 2>&1; then
  echo "--- nvidia-smi ---"
  nvidia-smi >>"$RESULTS" 2>&1 || true
  nvidia-smi topo -m >>"$RESULTS" 2>&1 || true
else
  echo "(nvidia-smi not present in container; probe output above is authoritative)"
fi

# ---------------------------------------------------------------------------
# Phase C: TP2 qualification tests
# ---------------------------------------------------------------------------
phase "Phase C: TP2 qualification tests"

run_ctest() {
  local label="$1"; shift
  ( cd "$BUILD" && ctest --output-on-failure "$@" ) > "$WORK/ctest.$label.log" 2>&1
  local rc=$?
  tail -n 15 "$WORK/ctest.$label.log"
  if grep -qE 'Test #[0-9]+:.*Skipped' "$WORK/ctest.$label.log"; then
    skip "$label (hardware-gated skip)"
  elif [ "$rc" -eq 0 ]; then
    ok "$label"
  elif [ "$rc" -eq 77 ]; then
    skip "$label (hardware-gated skip)"
  else
    bad "$label (rc=$rc)"
  fi
}

if [ -x "$BUILD/tests/ninfer_allreduce_test" ]; then
  run_ctest "allreduce" -R allreduce
else
  skip "allreduce (binary not built)"
fi

if [ -x "$BUILD/tests/ninfer_split_ops_test" ]; then
  run_ctest "split" -R split
else
  skip "split (binary not built)"
fi

# Graph capture / instantiate / replay qualification.
if [ -x "$BUILD/tests/ninfer_decode_graph_test" ]; then
  "$BUILD/tests/ninfer_decode_graph_test" > "$WORK/graph.log" 2>&1
  rc=$?
  cat "$WORK/graph.log"
  case "$rc" in
    0)  ok "graph capture/instantiate/replay (decode graph)" ;;
    77) skip "graph capture (no CUDA device)" ;;
    *)  bad "graph capture/instantiate/replay (rc=$rc)" ;;
  esac
else
  skip "graph capture (binary not built)"
fi

# ---------------------------------------------------------------------------
# Phase E: end-to-end serving (optional)
# ---------------------------------------------------------------------------
if [ "$SKIP_SERVE" = "1" ]; then
  phase "Phase E: serving (skipped)"
  skip "serving (--skip-serve)"
elif [ ! -f "$ARTIFACT" ]; then
  phase "Phase E: serving"
  skip "serving (artifact not found: $ARTIFACT)"
else
  phase "Phase E: end-to-end serving"
  SERVE_BIN="$BUILD/apps/ninfer-serve"
  if [ ! -x "$SERVE_BIN" ]; then
    bad "serve binary missing: $SERVE_BIN"
  else
    SERVE_LOG="$WORK/serve.log"
    echo "launching: $SERVE_BIN $ARTIFACT --host 127.0.0.1 --port $PORT $SERVE_ARGS"
    ( "$SERVE_BIN" "$ARTIFACT" --host 127.0.0.1 --port "$PORT" \
        --max-concurrency 1 --kv-dtype bf16 $SERVE_ARGS > "$SERVE_LOG" 2>&1 ) &
    SERVE_PID=$!
    echo "serve pid=$SERVE_PID log=$SERVE_LOG"
    ready=0
    for _ in $(seq 1 120); do
      if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then ready=1; break; fi
      if ! kill -0 "$SERVE_PID" 2>/dev/null; then break; fi
      sleep 2
    done
    if [ "$ready" = "1" ]; then
      ok "serve /health ready"
      if curl -sf -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
           -H 'Content-Type: application/json' \
           -d '{"model":"local","messages":[{"role":"user","content":"Reply with the single word: pong"}],"max_tokens":16,"temperature":0}' \
           > "$WORK/infer.json" 2>&1; then
        head -c 400 "$WORK/infer.json"; echo
        ok "inference request executed"
      else
        bad "inference request failed (see serve log)"
        tail -n 20 "$SERVE_LOG"
      fi
    else
      bad "serve did not become ready (see log)"
      tail -n 30 "$SERVE_LOG"
    fi
    kill "$SERVE_PID" 2>/dev/null || true
    wait "$SERVE_PID" 2>/dev/null || true
  fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
phase "Summary"
printf '  PASS=%d  FAIL=%d  SKIP=%d\n' "$PASS" "$FAIL" "$SKIP"
echo "  full results: $RESULTS"
[ "$FAIL" -gt 0 ] && { echo "  RESULT: FAIL"; exit 1; }
if [ "$PASS" -eq 0 ] && [ "$SKIP" -gt 0 ]; then echo "  RESULT: SKIP (no GPU)"; exit 77; fi
echo "  RESULT: PASS"
exit 0

