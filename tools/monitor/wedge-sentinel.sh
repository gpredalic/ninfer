#!/bin/bash
# ninfer wedge sentinel (v2, 2026-09-14).
#
# Restarts ninfer.service when the engine is wedged: work pending, nothing
# executing. Signature: running=0 prefilling=0 decode_ready=0 AND
# (waiting>=1 OR materializing>=1), sustained 150s.
#
# The threshold MUST exceed the engine's 120s fit-gate defer deadline: a
# deferring request is in progress with a bounded deadline (it aborts
# cleanly at 120s), not a wedge. 2026-09-15 06:33 episode: the old 90s
# threshold restarted the server 23s before the engine's own clean abort,
# destroying all caches (~55s root re-prefill per subsequent request).
#
#   Class A: request queued, engine idle        (waiting>=1, materializing=0)
#   Class B: stuck in deferred materialization  (materializing>=1) — the
#            22:59-23:10 episode: m=1 for 3.5+ min, ticker degraded to
#            25-30s cadence, request then died on the 120s fit-gate deadline.
#
# Signals per poll (15s), in order:
#   1. HTTP /stats "scheduler" object — primary.
#   2. journal "throughput interval" line within the last 60s — fallback
#      (the engine's 5s ticker degrades under a wedge; journal lag can
#      exceed one poll cycle).
#   3. no signal at all — HOLD state. A wedged engine goes silent or slow,
#      so absence of data must NOT clear an armed timer. (v1 bug #1: an
#      empty poll reset stuck_since, and the 22:42 w=1 window was broken by
#      a 49s line gap.)
#
# Arm/disarm:
#   - wedge line  -> arm (record armed_since once)
#   - active line (running|prefilling|decode_ready >= 1) -> disarm
#   - all-zero line (idle session, nothing pending)      -> disarm
#   - no signal   -> hold
#
# v1 bug #2: the running process never picked up on-disk edits — bash parses
# the whole `while` compound at startup. Any change to this file requires
# `systemctl restart ninfer-wedge-sentinel.service`.
#
# Safety rails:
# - 90s grace after our own restart (model load window) — no arming.
# - 3 restarts within 30 minutes stops auto-restart and alerts (a restart
#   loop is worse than a wedge: each restart cold-starts every big
#   conversation ~100s).
PORT=8080
if [ "$(id -u)" != "0" ]; then JC="sudo -n journalctl"; else JC="journalctl"; fi

poll_state() {
  # prints "running prefilling decode_ready waiting materializing" or nothing
  local out
  out=$(curl -s --max-time 5 "http://127.0.0.1:$PORT/stats" 2>/dev/null \
    | python3 -c '
import json, sys
try:
    s = json.load(sys.stdin).get("scheduler", {})
    print(s.get("running", 0), s.get("prefilling", 0), s.get("decode_ready", 0),
          s.get("waiting", 0), s.get("materializing", 0))
except Exception:
    sys.exit(1)
' 2>/dev/null)
  if [ -n "$out" ]; then
    echo "$out"
    return 0
  fi
  local line
  line=$($JC -u ninfer.service --since "60 sec ago" --no-pager 2>/dev/null \
         | grep "throughput interval" | tail -1)
  [ -n "$line" ] || return 1
  echo "$line" | sed -nE 's/.*running=([0-9]+) prefilling=([0-9]+) decode_ready=([0-9]+) waiting=([0-9]+) materializing=([0-9]+).*/\1 \2 \3 \4 \5/p'
}

armed_since=0
grace_until=0
restart_count=0
window_start=0
stopped=0
while true; do
  now=$(date +%s)
  state=$(poll_state)
  echo "$state" | grep -qE '^[0-9]+( [0-9]+){4}$' || state=""
  if [ -n "$state" ]; then
    read -r r p d w m <<< "$state"
    if [ "$r" -ge 1 ] || [ "$p" -ge 1 ] || [ "$d" -ge 1 ]; then
      armed_since=0
    elif [ "$w" -ge 1 ] || [ "$m" -ge 1 ]; then
      if [ "$armed_since" = "0" ]; then
        armed_since=$now
        echo "WEDGE ARMED: r=$r p=$p d=$d w=$w m=$m — restart in 150s if it persists"
      fi
    else
      armed_since=0
    fi
  fi
  # no signal: hold armed state (a wedged engine goes silent)
  if [ "$armed_since" != "0" ] && [ $((now - armed_since)) -ge 90 ] \
     && [ "$now" -ge "$grace_until" ] && [ "$stopped" = "0" ]; then
    if [ "$window_start" = "0" ] || [ $((now - window_start)) -ge 1800 ]; then
      window_start=$now
      restart_count=0
    fi
    restart_count=$((restart_count + 1))
    if [ "$restart_count" -ge 3 ]; then
      stopped=1
      echo "WEDGE REPEAT: 3 restarts in 30 min — auto-restart stopped, needs investigation"
    else
      echo "WEDGE: engine idle with work pending $((now - armed_since))s (r=$r p=$p d=$d w=$w m=$m) — restarting ninfer (restart #$restart_count)"
      sudo -n systemctl restart ninfer.service 2>/dev/null || systemctl restart ninfer.service
      armed_since=0
      grace_until=$((now + 90))
    fi
  fi
  sleep 15
done
