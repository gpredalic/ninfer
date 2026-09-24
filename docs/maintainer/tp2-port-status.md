# TP2 (dual-GPU tensor parallelism) port status

This branch ports dual-GPU TP2 support from the `wamansou/ninfer-tp2-1m` fork into the local
`ninfer` `tp2` branch for two RTX PRO 4000 Blackwell GPUs, preserving TP1 behavior.

## What is in place

### Ops / collectives / graph layer (complete)
- Split-op kernels ported from the fork (86 compatible `src/ops/` files copied; diverged
  FP8/NVFP4/linear/rope files manually merged while preserving local TMA strategy and the
  local-only `kFp8VocabularyFirstA16GemmT` constant).
- Dual-device collectives (`allreduce`, `allgather`) and split-launch infrastructure.
- Dual-device CUDA Graph capture/replay mechanics.
- `ExecutionContext` (one process, up to two CUDA devices) in `src/core/device.h`.
- Per-device artifact materialization (`MaterializationPlan.device_capacity_bytes` indexed by
  device; `DeviceMaterialization` with device index and `PlaneCopy` ranges; `Binder` supports
  `Replicated`/`Rows`/`Columns` placements). TP1 remains whole-object placement on device 0.

### Engine / CLI / serve options (complete)
- `EngineOptions.tp` (1 or 2) and `EngineOptions.devices` (one id per rank; empty derives
  `{device}`).
- CLI `--tp 1|2` and `--devices N,M`; serve `--tp 1|2` and `--devices N,M` (parsing, help text,
  and `EngineOptions` mapping all wired).
- Engine device resolution: `resolve_execution_device_ids` + `require_supported_tp_features`
  (rejects `tp` not in {1,2}; rejects `--tp 2` with Vision or the DFlash speculative backend,
  which have no split path in this build).
- The engine constructs an `ExecutionContext` and references its `primary()` device. TP1
  (`tp == 1`) is bit-identical to the prior single-`DeviceContext` path: both end with device 0
  current, and the same `DeviceContext` is handed to the target layer.

## What remains (target-layer 2-rank wiring)

The local target/runtime layer is still single-device: `Program` has no peer program, no
`tp`/`ExecutionContext` members, and the 27B/35B packages thread a single `DeviceContext`
through `construct_target` and the Core constructors. The fork's target layer, by contrast,
threads `ExecutionContext` through ~19 files and gives `Program` a rank-1 **peer program** that
runs in lockstep (dual-device graph bridge, per-rank rope on each device/stream, `TpPeerCore`,
peer SSM/MTP state checks).

Completing TP2 end-to-end therefore requires porting that peer-program architecture into the
local `Program`/schedule/graph-capture layer and re-threading `ExecutionContext` through the
target packages. This is a large, cross-cutting refactor whose correctness (numerical parity
with TP1, dual-device graph replay, per-device materialization) can only be validated on a
two-GPU machine. It is intentionally not attempted blindly in a CPU-only environment.

Until that wiring lands, `--tp 2` constructs a two-device `ExecutionContext` but the target
layer executes on the primary device only. The CLI/serve options, engine device resolution,
and the ops/collectives/graph infrastructure are all in place and building; the remaining work
is the target-layer 2-rank program wiring and its GPU validation.

## Validation performed (CPU-only environment)
- Full build: `cmake --build build -j` → `EXIT=0` (all 167 targets: `ninfer`, `ninfer-serve`,
  `ninfer-perplexity`, 112 test binaries).
- CPU tests pass: `ninfer_cli_options_test`, `ninfer_serve_options_test`, `artifact_reader`.
- `--tp`/`--devices` verified present in both `ninfer` and `ninfer-serve` help/usage strings.
- GPU-dependent tests and numerical TP2 validation could not run here: the environment is
  CPU-only and lacks `libcuda.so.1`.
