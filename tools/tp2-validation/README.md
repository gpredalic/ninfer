# TP2 Validation

One-command harness for validating dual-GPU (TP2) inference on the
dual-RTX PRO 4000 Blackwell host.

## Prerequisites

### Host (NixOS machine running Incus)

1. NVIDIA driver loaded (`/proc/driver/nvidia/version` exists).
2. Two GPUs visible under `/proc/driver/nvidia/gpus/`.
3. Incus CLI available and the container (`vibe-workspace`) exists.

Apply GPU passthrough **once** from the host:

```bash
# From the host, as root or an Incus admin:
/home/razvijalec/ninfer/tools/tp2-validation/host-setup-gpu-passthrough.sh vibe-workspace
```

This sets `raw.access.nvidia` on the container and restarts it. After the
restart, `/dev/nvidia0`, `/dev/nvidia1`, `/dev/nvidiactl`,
`/dev/nvidia-uvm`, and `/dev/nvidia-uvm-tools` are visible inside the
container.

### Container

The container needs the NVIDIA userspace driver libraries and CUDA toolkit.
The validation script wires up `LD_LIBRARY_PATH` automatically:

```
/home/razvijalec/tools/nvidia-595/usr/lib/x86_64-linux-gnu   # libcuda, libnvidia-ml
/home/razvijalec/tools/debs/usr/lib/x86_64-linux-gnu         # FFmpeg / media codecs
/home/razvijalec/tools/cuda/usr/local/cuda-13.1/targets/x86_64-linux/lib  # CUDA runtime
```

The build must be up to date:

```bash
cd /home/razvijalec/ninfer
cmake --build build -j
```

The `.ninfer` artifact must exist for the serving phase:

```bash
ls -lh out/qwen3_6_27b.ninfer   # ~17 GiB
```

## Usage

```bash
cd /home/razvijalec/ninfer

# Full validation (tests + serving):
./tools/tp2-validation/validate-tp2.sh

# Skip the serving phase (tests only):
./tools/tp2-validation/validate-tp2.sh --skip-serve

# Custom artifact / port:
./tools/tp2-validation/validate-tp2.sh --artifact out/qwen3_6_27b.ninfer --port 8090
```

## Phases

| Phase | What it checks | GPU required |
|-------|---------------|--------------|
| 0 | `cudaGetDeviceCount() == 2` | yes |
| B | Hardware info: model, UUIDs, driver/CUDA versions, peer-access matrix | yes |
| C | `ninfer_allreduce_test` — P2P allreduce correctness | yes |
| C | `ninfer_split_ops_test` — split-launch helpers | yes |
| C | `ninfer_decode_graph_test` — CUDA Graph capture/instantiate/replay | yes |
| E | `ninfer-serve` health + inference request (optional) | yes |

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | All executed phases passed |
| 1 | One or more phases failed |
| 77 | All failures were hardware-gated skips (no GPU visible) |

## Troubleshooting

- **`cudaErrorNoDevice`**: GPU passthrough not applied. Run
  `host-setup-gpu-passthrough.sh` on the host and restart the container.
- **`libcuda.so.1: cannot open shared object file`**: The NVIDIA userspace
  driver libraries are missing. Install them or set `LD_LIBRARY_PATH`.
- **`ninfer-serve` crashes on startup**: Missing FFmpeg/media codec
  libraries. Ensure `/home/razvijalec/tools/debs/usr/lib/x86_64-linux-gnu`
  is in `LD_LIBRARY_PATH` (the script does this automatically).
- **Conversion OOM**: The converter needs ~10 GiB peak RAM for the largest
  tensor. The `_PACK_TEMP_BYTES` constant in `tools/artifact/layouts.py`
  controls the packing chunk size; it is set to 1 GiB to fit within a
  16 GiB cgroup limit.
