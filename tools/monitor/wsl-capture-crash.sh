#!/bin/bash
# wsl-capture-crash — core-dump capture for WSL2.
#
# The kernel core_pattern on this box is:
#   |/wsl-capture-crash %t %E %p %s
# but /wsl-capture-crash did not exist, so every core dump was silently
# discarded (the 2026-09-17 12:20 SIGABRT core was lost this way).
#
# Install (root, takes effect on the NEXT crash — no reboot needed):
#   sudo install -m 755 -o root tools/monitor/wsl-capture-crash.sh /wsl-capture-crash
# (installed 2026-09-17)
#
# The kernel pipes the core image on stdin; args are %t (epoch), %E (exe),
# %p (pid), %s (signal number). Cores land in ~/crash-cores (a 27B-model
# core can be 10-20 GiB; the last 3 are kept, older ones deleted).
set -u
OUTDIR=/home/zenz/crash-cores
mkdir -p "$OUTDIR"
safe=$(basename "${2:-unknown}" | tr -c 'A-Za-z0-9._-' '_')
out="$OUTDIR/$(date '+%Y%m%d-%H%M%S')-pid${3:-?}-sig${4:-?}-${safe}.core"
cat > "$out"
echo "$(date -Iseconds) captured exe=${2:-?} pid=${3:-?} sig=${4:-?} size=$(stat -c %s "$out" 2>/dev/null || echo '?') -> $out" >> "$OUTDIR/capture.log"
# P4.1 (2026-09-17): snapshot GPU + kernel state at the moment of death, into a
# sidecar next to the core. nvidia-smi can hang on a wedged GPU, so it is
# timeout-guarded; dmesg carries the dxgkrnl sync-object warnings. Captured
# AFTER the core (the core is the priority; a hung GPU must not lose it).
side="${out%.core}.diagnostics"
{
  echo "=== captured $(date -Iseconds) exe=${2:-?} pid=${3:-?} sig=${4:-?} ==="
  echo "=== nvidia-smi ==="
  timeout 10 nvidia-smi 2>&1 || echo "nvidia-smi FAILED or hung (timeout 10s)"
  echo "=== dmesg (last 40) ==="
  timeout 5 dmesg 2>/dev/null | tail -40 || echo "dmesg unavailable"
  echo "=== gpu processes ==="
  ps -o pid,rss,etime,cmd -C ninfer-serve 2>/dev/null || true
} > "$side" 2>&1
# keep the 3 most recent cores
ls -1t "$OUTDIR"/*.core 2>/dev/null | tail -n +4 | xargs -r rm -f --
ls -1t "$OUTDIR"/*.diagnostics 2>/dev/null | tail -n +4 | xargs -r rm -f --
exit 0
