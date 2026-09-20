# PackGQA INT8 prompt attention — RTX 5090 validation runbook

Temporary handoff plan for the machine that can actually build and run CUDA. Delete this file
when both phases below are complete (keep the final numbers in the commit report or
`docs/performance.md` only if they become a published claim).

## Change under validation

- `src/ops/softmax_attention/dense/causal_cache/prompt_i8.cuh` — new
  `causal_attention_prompt_i8_gqa_kernel`: PackGQA prompt kernel. One CTA per KV head; the
  `Br=64` rows are packed token-major (`packed = token * G + qh_in_group`), so each K/V tile is
  loaded once and shared by all `G` Q heads of the group. Q-quantize, softmax, PV, and scatter
  structure are identical to the per-Q-head kernel; only the Q load and output scatter decode
  `(q_head, token)` from the packed row.
- `src/ops/softmax_attention/dense/causal_cache/prompt.cu` — INT8 + `Geometry::GroupSize > 1`
  selects the packed kernel (grid `div_up(W*G, 64) x KVHeads`) for **both** entry points:
  direct (`causal_softmax_attention_cached`, `CausalPromptDirectMetadata`) and append+attend
  (`causal_softmax_attention`, `CausalPromptBatchMetadata<Masked>`).
- Instantiated geometries are GQA-only: `CausalD256H24Kv4` (G=6) and `CausalD256H16Kv2` (G=8).
  The `GroupSize == 1` launcher branch is unreachable for the registered geometries; no such
  instantiation exists.
- Untouched: BF16 / FP8 / NVFP4 prompt paths, all decode kernels, op contract, workspace sizing.
- Baseline: tag `backup/pre-packgqa-int8-port` at `9b4d5e1c` (= current `master` HEAD before the
  uncommitted two-file diff).

Static audit (done on the non-GPU machine): packed-row decode, per-row causal mask (`qabs =
base_pos + (packed_q0 + r) / G`), full-tile fast path (`tile_rows == Br && k0 + Bc - 1 <=
base_pos + min_token`), softmax state, PV accumulation, normalization, per-`(q_head, token)`
scatter, and exact-BF16-zero fill for invalid columns (`row >= tile_rows`, `token < width`)
all match the base kernel's contract, including the `verify_invalid_columns_zero` check in the
test. Numerical validation still has to happen on the GPU below.

## 0. Environment

RTX 5090, `sm_120a`, CUDA 13.1 (toolkit + runtime), working tree on `master` at `9b4d5e1c`
with exactly the two modified files (`git status` clean apart from those and the untracked
`BRANCH_INTEGRATION.md`).

## 1. Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DNINFER_BUILD_BENCHMARKS=ON
cmake --build build -j
```

Expect a clean compile of `ninfer_ops` (the only TU including `prompt_i8.cuh` is `prompt.cu`).

## 2. Correctness gate

Primary — exercises the new kernel directly (INT8 GQA, both geometries, direct + batch,
masked/unmasked, fragmented/identity tables, tile-boundary widths like 61/63/67/127/511,
exact-zero invalid columns):

```bash
ctest --test-dir build -R ninfer_softmax_attention_test --output-on-failure
```

Expect `PASS causal_softmax_attention public-contract correctness` and exit 0. This test
also covers the BF16 and FP8 prompt paths (regression guard for the untouched kernels) and
the plain/packed + context op suites.

Attention-adjacent regression set (unchanged neighbors):

```bash
ctest --test-dir build \
  -R 'ninfer_sliding_window_attention_test|ninfer_kv_cache_append_test|ninfer_prepare_masked_block_test' \
  --output-on-failure
```

Full suite (strongest gate; GPU-dependent op tests self-skip with exit 77 only when no device
is visible, so this is safe to run):

```bash
ctest --test-dir build --output-on-failure
```

The `*_real_test` / `score_real` targets need the product artifact
(`out/qwen3_6_27b.ninfer` + conversion report) present on the machine; confirm before relying
on the full-suite result.

Any failure: stop, investigate, do not commit.

## 3. Commit (only after section 2 is green)

```bash
git add src/ops/softmax_attention/dense/causal_cache/prompt_i8.cuh \
        src/ops/softmax_attention/dense/causal_cache/prompt.cu
git commit -m "perf(attention): pack GQA rows in the INT8 prompt kernel"
```

Push to `fork` (SSH) when done; `origin` push when credentials are available.

## 4. Performance phase (after the commit — separate phase)

Baseline setup: build the pre-port tag in a sibling worktree on the same machine, same flags:

```bash
git worktree add /tmp/ninfer-baseline backup/pre-packgqa-int8-port
cmake -S /tmp/ninfer-baseline -B /tmp/ninfer-baseline/build \
  -DCMAKE_BUILD_TYPE=Release -DNINFER_BUILD_BENCHMARKS=ON
cmake --build /tmp/ninfer-baseline/build -j --target ninfer_causal_softmax_attention_bench \
  ninfer_bench
```

### 4.1 Op level (direct claim: prompt-kernel latency, INT8)

```bash
./build/bench/ops/ninfer_causal_softmax_attention_bench \
  --entry both --kv-dtype all --geometry all --execution graph --cache cold \
  --batch 1 --tokens 64,512,2048,8192,32768,65536,131072,262144 --context 0 \
  --warmup 10 --repeat 50 \
  --csv-out profiles/bench/packgqa-int8/op-after.csv
# identical flags on the baseline worktree -> profiles/bench/packgqa-int8/op-before.csv
```

- `--entry append` = `ops::causal_softmax_attention` (append+attend; batch metadata) and
  `--entry cached` = `causal_softmax_attention_cached` (direct metadata) — both hit the
  modified kernel for INT8.
- Compare the `median_us` column per (entry, geometry, kv, W). Do **not** compare the reported
  `logical GB/s` (logical byte count is contract-identical between base and packed).
- Expectation: INT8 `median_us` at or below baseline, improving with W as the K/V DRAM
  traffic term shrinks by G (6x for `d256-h24-kv4`, 8x for `d256-h16-kv2`); BF16/FP8 rows
  unchanged (untouched kernels). End-to-end prefill gains on the product bench (4.2) will be
  smaller than op-level because attention is a fraction of prefill (GEMMs dominate).
- If INT8 does not improve at large W, the kernel is compute-bound (s8 MMA + quantize), not
  traffic-bound: report that, keep the correctness win, do not claim a perf improvement.
  Attribute with `--profile` + Nsight Systems (`cudaProfilerApi` range) before concluding.

### 4.2 Product level (Engine route: prefill/TTFT direction at INT8 KV)

```bash
./build/bench/ninfer_bench --weights out/qwen3_6_27b.ninfer \
  -p 8192,32768,131072 --kv-dtype int8 -r 5 --warmup 1 \
  -o table --output-file profiles/bench/packgqa-int8/pp-int8-after.csv
# decode sanity (decode kernels are untouched — expect within run-to-run noise):
./build/bench/ninfer_bench --weights out/qwen3_6_27b.ninfer \
  -n 128 --kv-dtype int8 -r 5 --warmup 1 \
  -o table --output-file profiles/bench/packgqa-int8/tg-int8-after.csv
# identical invocations on /tmp/ninfer-baseline/build/bench/ninfer_bench -> *-before.csv
```

Same methodology as `docs/performance.md` (1024-token prefill chunk, INT8 group-64 KV,
CUDA Graph enabled, prefix reuse disabled). The same-machine baseline-tag build is the
comparison target; published `docs/performance.md` numbers (older revisions) are context only.

## 5. Cleanup

- Delete this file after sections 2-4 complete.
- `BRANCH_INTEGRATION.md` (untracked): keep for the follow-up sync-elimination port
  (`origin/local/sync-elimination-experiment`, tip `85f4bb86`); remove or integrate the
  surviving decisions after that port lands.
