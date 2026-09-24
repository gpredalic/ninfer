#!/usr/bin/env bash
#
# host-setup-gpu-passthrough.sh
#
# Configure an Incus container for NVIDIA GPU passthrough so that CUDA works
# inside the container. Run this ON THE HOST (the NixOS machine running Incus),
# as root or a user with Incus admin rights.
#
# Root cause this fixes:
#   The container has no /dev/nvidia* nodes and its device cgroup denies the
#   NVIDIA devices, so cudaGetDeviceCount() returns 0 and nvidia-smi is absent.
#   Incus only exposes GPU devices to a container when they are explicitly
#   declared; by default a container is GPU-less.
#
# What this does:
#   1. Detects the NVIDIA GPUs present on the host.
#   2. Declares the NVIDIA device nodes on the container via
#      `raw.access.nvidia` (device nodes + device-cgroup allowlist in one key).
#   3. (Optional) Adds modern `gpu` devices as an alternative.
#   4. Restarts the container so the device changes take effect.
#   5. Verifies the container now sees the GPUs.
#
# The container must also have the NVIDIA *userspace* driver libraries
# (libcuda.so.1, libnvidia-ml.so.1, ...) and a CUDA toolkit. This repository's
# validation harness (validate-tp2.sh) wires up the library path; see README.md.
#
# Usage:
#   ./host-setup-gpu-passthrough.sh [container-name]
#   CONTAINER=vibe-workspace ./host-setup-gpu-passthrough.sh
#
set -euo pipefail

CONTAINER="${1:-${CONTAINER:-vibe-workspace}}"
# Device nodes required for CUDA + NVML across two GPUs. Adjust the nvidiaN
# list if the host has a different GPU count.
NVIDIA_NODES="/dev/nvidiactl,/dev/nvidia0,/dev/nvidia1,/dev/nvidia-uvm,/dev/nvidia-uvm-tools"
RESTART="${RESTART:-1}"

log()  { printf '[gpu-passthrough] %s\n' "$*"; }
die()  { printf '[gpu-passthrough] ERROR: %s\n' "$*" >&2; exit 1; }

command -v incus >/dev/null 2>&1 || die "incus CLI not found on host"

# --- 0. Sanity: is the NVIDIA driver actually loaded on the host? ----------
if [ ! -e /proc/driver/nvidia/version ]; then
  die "NVIDIA kernel module not loaded on host (/proc/driver/nvidia/version missing). Install/load the driver first."
fi
log "host driver: $(head -1 /proc/driver/nvidia/version)"

GPU_COUNT="$(ls -1 /proc/driver/nvidia/gpus 2>/dev/null | wc -l | tr -d ' ')"
log "host GPUs visible to the kernel: ${GPU_COUNT}"
[ "${GPU_COUNT}" -ge 1 ] || die "no GPUs under /proc/driver/nvidia/gpus"

# --- 1. Confirm the container exists ---------------------------------------
incus info "${CONTAINER}" >/dev/null 2>&1 || die "container '${CONTAINER}' not found (incus list)"

# --- 2. Declare the NVIDIA device nodes ------------------------------------
# `raw.access.nvidia` is the supported key for NVIDIA passthrough: Incus creates
# the device nodes inside the container AND allowlists them in the device cgroup.
log "setting raw.access.nvidia on '${CONTAINER}'"
incus config set "${CONTAINER}" raw.access.nvidia "${NVIDIA_NODES}"

# --- 3. (Optional, modern alternative) `gpu` devices -----------------------
# Newer Incus (>= 5.10) supports a structured `gpu` device type that also
# auto-mounts the host driver userspace libraries. Enable with USE_GPU_DEVICE=1.
# NOTE: exact property names vary by Incus version; verify with
#       `incus config device add --help` / the Incus GPU docs for your version.
if [ "${USE_GPU_DEVICE:-0}" = "1" ]; then
  i=0
  for gpu in /proc/driver/nvidia/gpus/*; do
    [ -e "${gpu}" ] || continue
    uuid="$(basename "${gpu}")"
    devname="gpu${i}"
    log "adding gpu device '${devname}' (nvidia.id=${uuid})"
    incus config device remove "${CONTAINER}" "${devname}" 2>/dev/null || true
    incus config device add "${CONTAINER}" "${devname}" gpu "nvidia.id=${uuid}"
    i=$((i + 1))
  done
fi

# --- 4. Apply (device changes require a restart) ---------------------------
if [ "${RESTART}" = "1" ]; then
  log "restarting '${CONTAINER}' to apply device changes"
  incus restart "${CONTAINER}"
  log "waiting for container to reach RUNNING"
  for _ in $(seq 1 60); do
    state="$(incus info "${CONTAINER}" 2>/dev/null | awk -F': ' '/^State:/{print $2}')"
    [ "${state}" = "Running" ] && break
    sleep 2
  done
  [ "${state:-}" = "Running" ] || die "container did not reach Running state"
else
  log "RESTART=0 -> not restarting; run 'incus restart ${CONTAINER}' manually"
fi

# --- 5. Verify from the host ------------------------------------------------
log "verifying device nodes inside the container"
if incus exec "${CONTAINER}" -- ls -l /dev/nvidia* 2>/dev/null; then
  log "OK: /dev/nvidia* nodes present"
else
  log "WARN: could not list /dev/nvidia* via incus exec (container may still be starting)"
fi

log "done. Next: run tools/tp2-validation/validate-tp2.sh INSIDE the container."
log "Reminder: the container needs the NVIDIA userspace driver libs + CUDA toolkit"
log "(see README.md); validate-tp2.sh sets the library path automatically."
