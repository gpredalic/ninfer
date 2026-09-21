# NInfer

NInfer is a C++/CUDA inference engine built for a single GPU. It runs a small set of Qwen
checkpoints — Qwen3.6-27B, Qwen3.8-27B, and Qwen3.6-35B-A3B — for text, image/video, tool
calling, and long-context workloads on one SM120 Blackwell GPU.

It is written from scratch. With one model resident on one GPU, there is no framework overhead
to hide behind — every millisecond of latency comes from the engine itself. NInfer handles the
whole path: hand-tuned attention and linear-attention (GDN) kernels, CUDA Graph decode,
speculative heads shipped in the checkpoint, and a context cache that moves KV pages and state
together between device and pinned host memory. There is no Python runtime, no plugin layer, and
no weight repacking at load time — the model arrives as one `.ninfer` file and runs as-is.

Three design choices stand out:

- **One engine for every surface.** The CLI, the OpenAI-compatible server, and the
  Anthropic-compatible server all run the same code and the same numerics — behavior never
  depends on which one you use.
- **The artifact is the model.** Weights, tokenizer, chat template, and metadata ship in one
  file; identity is read from the file, not from configuration, so there is nothing to mismatch.
- **Long context is a first-class feature.** 262k tokens natively, roughly 600k on a 32 GB GPU
  with YaRN scaling, NVFP4 KV, and host-memory caching.

## Features

- **Reasoning control.** Toggle thinking on or off, set reasoning effort, and cap thinking
  budgets.
- **Multimodal input.** Send images and video from the CLI or over HTTP — local paths, URLs, or
  data URIs.
- **Tool calling.** Function tools are parsed and returned to the client; well-formed calls
  recover as structured tool calls even when wrapped in prose or quoted in values.
- **Fast decoding.** Speculative decoding (MTP, draft windows 1–5) on every target; text-only
  DFlash (windows 1–15) on 35B-A3B.
- **Flexible KV cache.** Choose the format that fits your memory budget: BF16, INT8 group-64,
  row-scaled FP8, or NVFP4.
- **Long context.** 262,144 tokens out of the box, extendable with YaRN linear position scaling.
- **Context reuse.** Repeated prefixes — shared system prompts, private multi-turn history — are
  cached on device and pinned host instead of recomputed.
- **Memory headroom.** When memory gets tight, inactive sessions spill to pinned host memory and
  restore on reuse.
- **Concurrent decoding.** Up to eight active requests via CUDA Graph decode and chunked
  prefill.
- **Compatible APIs.** OpenAI Chat Completions and Responses Core, Anthropic Messages —
  streaming, token counting, usage, local response state, `/health`, and `/stats`.
- **Tunable sampling.** Defaults come from the model, with per-request overrides; a post-thinking
  preset lowers temperature after the reasoning block closes.
- **Reproducible results.** Deterministic seeds, greedy mode, stop conditions, and an offline
  perplexity evaluator.

## Quick Start

Build (Linux, SM120 Blackwell GPU, CUDA 13.1 or newer):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Download an artifact and send a one-shot request:

```bash
hf download neroued/Qwen3.8-27B-NVFP4-NInfer --local-dir models

./build/apps/ninfer models/qwen3_8_27b_nvfp4.ninfer \
  --prompt "Explain speculative decoding in two sentences." \
  --max-context 8192 --max-new 256 \
  --kv-dtype fp8 --spec mtp --draft-tokens 3 --lm-head-draft
```

The answer streams to stdout. Loading progress, reasoning, timings, and throughput go to stderr,
so you can keep them separate (`> answer.txt 2> run.log`). Run `--help` on any binary for its
options; [docs/cli.md](docs/cli.md) covers thinking, sampling, vision, and media in depth.

## Running a Server

`ninfer-serve` loads one artifact and serves OpenAI- and Anthropic-compatible HTTP endpoints.
Here is a long-running configuration: 240k context, two concurrent requests, MTP speculative
decoding (3 draft tokens), FP8 KV, and a host cache that holds inactive sessions:

```bash
./build/apps/ninfer-serve models/qwen3_8_27b_nvfp4.ninfer \
  --host 127.0.0.1 --port 8080 \
  --max-context 240000 --kv-capacity auto \
  --max-concurrency 2 \
  --kv-dtype fp8 \
  --spec mtp --draft-tokens 3 --lm-head-draft \
  --device-state-slots 2 --host-state-slots 8 --host-kv-mib 8192 \
  --preserve-thinking
```

`--kv-capacity auto` sizes the shared KV pool from the memory left after weights, leaving 1 GiB
of headroom. `--preserve-thinking` keeps a session's closed reasoning in later prompts, which is
what agent clients want. Add `--vision` for image/video input, `--api-key` to require a bearer
token, and `--request-log-jsonl` for full-precision per-request records.

```bash
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "qwen3.8-27b",
    "messages": [{"role": "user", "content": "Reply with one short sentence."}],
    "max_tokens": 64
  }'
```

[docs/serving.md](docs/serving.md) is the full reference for endpoints, streaming, tools, state,
token counting, and options.

## Long Context

Long context lets you process more input at once — longer documents, longer conversations —
without splitting them up. The registered models have a native 262,144-token context. On a 32 GB
RTX 5090 you can go well beyond it with three settings:

- **KV format** sets the VRAM cost per cached token. NVFP4 KV is 144 bytes per token per KV head
  versus 264 for INT8 — about 45% less, so more context fits in the same VRAM.
- **YaRN scaling** (`--rope-scaling-factor`) raises the context ceiling itself; the published
  checkpoints handle it with no measured quality loss in the tested range.
- **Host cache** is the safety net: when the device pool is full, inactive sessions — KV pages
  and checkpoint state together — move to pinned host memory and restore on reuse.

On a 32 GB RTX 5090, the practical ceilings are about 555k tokens per session with three
concurrent sessions and vision (YaRN factor 2.12), and about 600k for a single session (YaRN
factor 2.30).

A working configuration for the 555k case:

```bash
./build/apps/ninfer-serve models/qwen3_8_27b_nvfp4.ninfer \
  --host 127.0.0.1 --port 8080 \
  --max-context 555000 --kv-capacity auto \
  --max-concurrency 3 \
  --kv-dtype nvfp4 \
  --spec mtp --draft-tokens 5 --lm-head-draft \
  --vision \
  --rope-scaling-factor 2.12 --rope-scaling-original-context 262144 \
  --host-kv-mib 36864
```

[docs/maintainer/kv-nvfp4-yarn.md](docs/maintainer/kv-nvfp4-yarn.md) has the full VRAM math,
quality checks, and pressure-test results.

## Performance Features

### NVFP4 KV cache

The KV cache does not have to be 16-bit. NVFP4 gives the same math with about 45% less cache
memory: 144 bytes per token per KV head, versus 264 for INT8 group-64 and 512 for BF16. Select it
with `--kv-dtype nvfp4`. Under the hood, each value is a 4-bit E2M1 code with an E4M3 scale per
16 elements; queries and keys run quantized directly on Tensor Cores, and values are dequantized
to BF16 before the value projection.

### MTP speculative decoding

Speculative decoding makes generation faster: a small head proposes draft tokens, and the full
model verifies several of them in one forward pass instead of one token at a time. The published
checkpoints carry this head built in. Each decode step, it proposes up to *k* draft tokens;
accepted tokens commit together. Draft windows run 1–5 with `--spec mtp --draft-tokens N`, and
`--lm-head-draft` loads the optimized proposal head. Published measurements put draft acceptance
at 45–71% depending on model and workload. On 35B-A3B, the text-only DFlash backend proposes
1–15-token windows with `--spec dflash --draft-tokens N`.

### Host-KV caching

Host KV caching keeps more sessions alive than VRAM can hold. When the shared device pool cannot
retain a session, the whole unit — KV pages and checkpoint state together — moves to pinned host
memory instead of being discarded. When a later request reuses that prefix, the unit restores
once and runs on device again. It is a tiered cache, not a swap. Size it with `--host-kv-mib`
(arena size in MiB) and `--host-state-slots` (state images), or give a single `--host-cache-mib`
budget that splits itself between the two.

### YaRN context extension

YaRN scaling extends the context window beyond the length the model was trained on — the
difference between "the model stops at 262k" and "the model stops when the KV pool does." It
compresses RoPE positions beyond the trained context with a linear ramp: positions up to
`--rope-scaling-original-context` (default 262,144) stay unchanged, and positions beyond are
divided by `--rope-scaling-factor`. For these checkpoints linear scaling is enough — the RoPE
configuration means no frequency wrapping in the tested range.

## Models

A model is a single `.ninfer` file — weights, tokenizer, chat template, and metadata in one
container. Any NInfer binary takes it as its first argument; the model and weight profile are read
from the file, so there is no configuration and no separate weight download.

| Model | Weights | Artifact | Download | Model card |
|---|---|---|---|---|
| Qwen3.6-27B | `groupwise-int` | `qwen3_6_27b.ninfer` | [neroued/Qwen3.6-27B-NInfer](https://huggingface.co/neroued/Qwen3.6-27B-NInfer) | [card](model-cards/Qwen3.6-27B-NInfer/README.md) |
| Qwen3.6-27B | `nvfp4` | `qwen3_6_27b_nvfp4.ninfer` | [neroued/Qwen3.6-27B-nvfp4-NInfer](https://huggingface.co/neroued/Qwen3.6-27B-nvfp4-NInfer) | [card](model-cards/Qwen3.6-27B-nvfp4-NInfer/README.md) |
| Qwen3.8-27B | `groupwise-int` | `qwen3_8_27b.ninfer` | [neroued/Qwen3.8-27B-NInfer](https://huggingface.co/neroued/Qwen3.8-27B-NInfer) | [card](model-cards/Qwen3.8-27B-NInfer/README.md) |
| Qwen3.8-27B | `nvfp4` | `qwen3_8_27b_nvfp4.ninfer` | [neroued/Qwen3.8-27B-nvfp4-NInfer](https://huggingface.co/neroued/Qwen3.8-27B-nvfp4-NInfer) | [card](model-cards/Qwen3.8-27B-nvfp4-NInfer/README.md) |
| Qwen3.6-35B-A3B | `groupwise-int` | `qwen3_6_35b_a3b.ninfer` | [neroued/Qwen3.6-35B-A3B-NInfer](https://huggingface.co/neroued/Qwen3.6-35B-A3B-NInfer) | [card](model-cards/Qwen3.6-35B-A3B-NInfer/README.md) |

All five support the same text, vision, MTP, prefix-reuse, and serving features; 35B-A3B
additionally supports the text-only DFlash backend. `groupwise-int` is the INT8 weight format
and `nvfp4` is the 4-bit NVFP4 format with the same mathematics — on a 32 GB GPU the nvfp4
artifacts leave the most headroom for context. The weight profile is normally resolved from
artifact identity; `--weights-profile` on the server overrides it for alternative builds of the
same model.

## Compatibility

**Hardware.** 64-bit Linux, one SM120 Blackwell GPU, CUDA Toolkit 13.1 or newer. The build
targets `sm_120a` and is tuned on the RTX 5090, but it adapts to the GPU it finds at startup —
smaller SM120 parts (for example RTX PRO 4000 Blackwell) run the same artifacts.

**Clients.** Any OpenAI- or Anthropic-compatible SDK works — point it at the server's base URL
and set the API key when one is configured:

- OpenAI Chat Completions: history, streaming, tools, usage
- OpenAI Responses Core: typed items, local response state, prompt token counting
- Anthropic Messages: messages, streaming, counting
- `/health` and `/stats` for liveness and an operational snapshot

Tool calls are parsed and returned; NInfer does not execute tools. Vision arrives as
`image_url` / `video_url` parts on a server started with `--vision`.

**Scope.** One GPU and one resident model per process. Up to eight active requests, served in
arrival order: no preemption, no priority, no weight offload, no multi-GPU. One shared KV pool
serves every active request and every retained prefix.

## Building

Requirements: 64-bit Linux, an SM120 Blackwell GPU, CUDA 13.1 or newer, CMake 3.28 or newer, a
C++20 host compiler, Ninja, `pkg-config`, FFmpeg development libraries (`libavformat`,
`libavcodec`, `libavutil`, `libswscale`), and `libcurl`. The build rejects CUDA architectures
other than `sm_120a`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/apps/ninfer` (one-shot CLI), `build/apps/ninfer-serve` (HTTP server), and
`build/apps/ninfer-perplexity` (offline perplexity evaluator). Tests, benchmarks, and maintainer
tools are off by default; run the binaries from the build tree — there is no install target.

A Docker build is included (requires the NVIDIA Container Toolkit):

```bash
docker build --tag ninfer:local .
docker run --rm --gpus '"device=0"' -p 8080:8080 -v "$PWD/models:/models:ro" ninfer:local \
  ninfer-serve /models/qwen3_8_27b_nvfp4.ninfer --host 0.0.0.0
```

## Contributing

Start with an Issue that describes the problem, the affected contract, and the expected behavior.
[CONTRIBUTING.md](CONTRIBUTING.md) covers bug reports, the performance-evidence bar, and how scope
decisions are made; the [maintainer docs](docs/maintainer/) are the reference for architecture and
Op contracts.

## Documentation

| Document | What it covers |
|---|---|
| [docs/README.md](docs/README.md) | full documentation map |
| [CLI](docs/cli.md) | text, chat history, image/video input, sampling, MTP, runtime options |
| [HTTP serving](docs/serving.md) | endpoints, streaming, tools, state, token counting, options |
| [Performance](docs/performance.md) | RTX 5090 measurements, MTP/DFlash results, methodology |
| [Perplexity](docs/perplexity.md) | offline causal perplexity evaluation |
| [Context cache](docs/maintainer/resource-scheduling-and-context-cache.md) | KV capacity, checkpoint ownership, Device/Host replica policy |
| [NVFP4 KV + YaRN](docs/maintainer/kv-nvfp4-yarn.md) | long-context storage and scaling |
| [CLI examples](examples/cli/) | committed text, multimodal, and long-context inputs |
| [Model cards](model-cards/) | per-artifact evaluation notes |

For exact option names and defaults, run `--help`.

## License

NInfer is licensed under [Apache-2.0](LICENSE). Published artifacts are derived from Qwen
checkpoints distributed under Apache-2.0; the model cards record per-artifact provenance.
Vendored dependencies retain their own licenses under `third_party/`.
