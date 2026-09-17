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
# keep the 3 most recent cores
ls -1t "$OUTDIR"/*.core 2>/dev/null | tail -n +4 | xargs -r rm -f --
exit 0
