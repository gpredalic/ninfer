"""Grouped symmetric quantization used by NInfer artifact converters.

The persistent numeric format fixes the code range, group size, and binary16
scale.  Model-specific recipes decide which tensors use those formats; this
module only performs the registered numeric transform.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import torch

from tools.artifact.layouts import encode_row_split, row_split_geometry
from tools.artifact.numeric import QuantFormat, get_format


_FP16_MIN_SUBNORMAL = 2.0**-24


@dataclass(frozen=True, slots=True)
class QuantizedMatrix:
    """Physical code groups and binary16 scales for one logical matrix."""

    codes: torch.Tensor
    scales: torch.Tensor


def _canonical_scale_words(
    max_abs: torch.Tensor,
    qmax: int,
) -> tuple[torch.Tensor, torch.Tensor]:
    """Return canonical binary16 scales and binary32 reciprocals on the host.

    CUDA division is not correctly rounded at every binary16 scale boundary.
    The host oracle performs the specified division in binary64, explicitly
    rounds through binary32 and binary16, then computes the reciprocal in the
    same way.  A binary32 input divided by these small integer denominators has
    enough binary64 precision for the final binary32 rounding to be exact.
    """

    host_max = max_abs.detach().cpu().numpy().astype(np.float32, copy=False)
    if not np.isfinite(host_max).all():
        raise ValueError("grouped quantization source contains NaN or infinity")
    with np.errstate(over="ignore", invalid="ignore", divide="ignore"):
        raw_scale = (host_max.astype(np.float64) / float(qmax)).astype(np.float32)
        scale = raw_scale.astype(np.float16)
    underflow = (scale == 0) & (host_max > 0)
    if underflow.any():
        scale = scale.copy()
        scale[underflow] = np.array(_FP16_MIN_SUBNORMAL, dtype=np.float16)
    if np.any((host_max > 0) & (~np.isfinite(scale) | (scale <= 0))):
        raise ValueError("grouped quantization scale is not finite and positive")

    reciprocal = np.zeros(host_max.shape, dtype=np.float32)
    positive = scale > 0
    reciprocal[positive] = (
        1.0 / scale[positive].astype(np.float64)
    ).astype(np.float32)
    return torch.from_numpy(scale), torch.from_numpy(reciprocal)


def pick_device(preferred: str | torch.device = "cuda") -> torch.device:
    device = torch.device(preferred)
    if device.type == "cuda" and not torch.cuda.is_available():
        return torch.device("cpu")
    return device


def quantize_matrix(
    weight: torch.Tensor,
    format: str | QuantFormat,
    *,
    device: str | torch.device | None = None,
) -> QuantizedMatrix:
    """Quantize logical ``[N,K]`` values, including registered K padding.

    Scales are rounded to binary16 before codes are selected because those are
    the exact scales consumed after loading.  Padding values are zero and do
    not affect a partially populated final group.
    """

    spec = get_format(format) if isinstance(format, str) else format
    if not isinstance(spec, QuantFormat):
        raise ValueError("grouped quantization requires a quantized numeric format")
    if weight.dim() != 2:
        raise ValueError(f"grouped quantization requires rank 2, got {tuple(weight.shape)}")
    if not weight.dtype.is_floating_point:
        raise TypeError(f"weight must be floating point, got {weight.dtype}")

    geometry = row_split_geometry(spec, weight.shape)
    target = pick_device() if device is None else pick_device(device)
    n = geometry.n
    # Quantize in row-chunks so the live FP32 working set is bounded by one
    # chunk instead of the whole matrix. Each row's groups are quantized
    # independently (per-group scales), so this is bit-for-bit identical to the
    # full-matrix path. The chunk is sized to keep the live working set (FP32
    # logical + int8 codes) near 1 GiB.
    bytes_per_row = geometry.k_pad * 5  # FP32 logical + int8 codes
    chunk_rows = max(1, min(n, (1 << 30) // max(1, bytes_per_row)))
    codes_chunks: list[torch.Tensor] = []
    scales_chunks: list[torch.Tensor] = []
    for start in range(0, n, chunk_rows):
        end = min(start + chunk_rows, n)
        rows = end - start
        logical = weight[start:end].detach().to(device=target, dtype=torch.float32)
        if geometry.k_pad != geometry.k:
            physical = torch.zeros(
                (rows, geometry.k_pad), dtype=torch.float32, device=target
            )
            physical[:, : geometry.k].copy_(logical)
            logical = physical
        grouped = logical.reshape(rows, geometry.groups_per_row, spec.group_size)
        # max|x| per group via two small reductions (no full-size abs temporary).
        max_abs = torch.maximum(grouped.amax(dim=2), grouped.amin(dim=2).abs())
        host_scales, host_reciprocal = _canonical_scale_words(max_abs, spec.qmax)
        reciprocal = host_reciprocal.to(target)
        grouped.mul_(reciprocal.unsqueeze(-1))
        grouped.round_()
        grouped.clamp_(spec.qmin, spec.qmax)
        codes_chunks.append(grouped.to(torch.int8))
        scales_chunks.append(host_scales.to(target))
        del logical, grouped, max_abs, reciprocal
    codes = torch.cat(codes_chunks, dim=0)
    scales = torch.cat(scales_chunks, dim=0)
    return QuantizedMatrix(codes=codes, scales=scales)


def quantize_and_encode(
    weight: torch.Tensor,
    format: str | QuantFormat,
    *,
    device: str | torch.device | None = None,
) -> bytes:
    """Quantize a logical matrix and encode ``row-split-k128-v1`` bytes."""

    spec = get_format(format) if isinstance(format, str) else format
    if not isinstance(spec, QuantFormat):
        raise ValueError("grouped quantization requires a quantized numeric format")
    quantized = quantize_matrix(weight, spec, device=device)
    return encode_row_split(quantized.codes, quantized.scales, spec, weight.shape)


__all__ = [
    "QuantizedMatrix",
    "pick_device",
    "quantize_and_encode",
    "quantize_matrix",
]
