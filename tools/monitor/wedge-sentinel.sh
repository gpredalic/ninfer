#!/bin/bash
# ninfer wedge sentinel (v3, 2026-09-17).
#
# Restarts ninfer.service when the engine is wedged: work outstanding, no
# engine progress. Three classes per poll (15s):
#
#   Class A: request queued, engine idle        (waiting>=1, materializing=0,
#                                                r=p=d=0)
#   Class B: stuck in deferred materialization  (materializing>=1, r=p=d=0)
#   Class C (v3): work in flight/queued but no engine progress. The
#          2026-09-17 12:20 zombie: a fatal CUDA error (cudaStreamSynchronize
#          -> SIGABRT) froze the engine mid-prefill. A/B never armed — they
#          need r=p=d=0, but the zombie last reported prefilling=1 and then
#          went fully silent for 13 min. C arms when, with work in
#          flight/queued, the engine's monotonic progress counters (the
#          /stats "counters" object: prefill/decode tokens, rounds, capture
#          completions) have not advanced for 150s. Any real engine progress
#          advances at least one counter at the 15s poll scale, so flat
#          counters with work outstanding is a wedge — including a frozen
#          prefilling=1 (the 12:20 case) and a fully silent process.
#
# Signal sources per poll, in order:
#   1. HTTP /stats — scheduler gauges + counters sum.
#   2. journal "throughput interval" line within 60s — fallback (gauges
#      only; it is NOT progress evidence for C — a live ticker with a dead
#      engine would otherwise mask the wedge).
#   3. no signal at all — A/B hold (a wedged engine goes silent, so absence
#      of data must NOT clear an armed timer; v1 bug #1). For C, silence
#      with work outstanding IS the no-progress signal (c_last_advance
#      simply stops advancing).
#
# Fresh-server reset: journal "listening on http" within 120s => a (re)start
# just happened — disarm all classes, clear the progress baseline, enter a
# 90s grace. Covers our own restarts AND systemd's on-failure auto-restarts
# (which v2's grace_until never saw — v2 could have restarted a still-
# loading server after a crash).
#
# The 150s threshold MUST exceed the engine's 120s fit-gate defer deadline:
# a deferring request self-aborts at 120s, drains the gauges (the all-zero
# line disarms C), and completes before C's threshold. A prefill or decode
# in flight always advances counters, so a healthy busy server never arms C.
#
# Arm/disarm:
#   A/B (v2): active line (r|p|d>=1) -> disarm; all-zero line -> disarm;
#            pending-only line -> arm; restart at arm+150s.
#   C: counter advance -> disarm; all-zero line -> disarm; arm requires
#      150s of stale progress with work outstanding, restart at arm+15s
#      (total ~165s from wedge start).
#
# Deployment: the running process never picks up on-disk edits — bash
# parses the whole `while` compound at startup. Any change to this file
# requires `systemctl restart ninfer-wedge-sentinel.service`.
#
# Safety rails:
# - 90s grace after a fresh server (model load window) — no restarts.
# - 3 restarts within 30 minutes (any class) stops auto-restart and alerts.
PORT=8080
if [ "$(id -u)" != "0" ]; then JC="sudo -n journalctl"; else JC="journalctl"; fi

poll_stats() {
  # prints "r p d w m counters_sum" or nothing
  curl -s --max-time 5 "http://127.0.0.1:$PORT/stats" 2>/dev/null \
    | python3 -c '
import json, sys
try:
    s = json.load(sys.stdin)
    sc = s.get("scheduler", {})
    c = s.get("counters", {})
    total = sum(c.get(k, 0) for k in
        ("computed_prefill_tokens", "committed_decode_tokens",
         "decode_rounds", "decode_row_rounds",
         "active_captures_completed", "active_captures_aborted"))
    print(sc.get("running", 0), sc.get("prefilling", 0),
          sc.get("decode_ready", 0), sc.get("waiting", 0),
          sc.get("materializing", 0), total)
except Exception:
    sys.exit(1)
' 2>/dev/null
}

poll_journal_state() {
  # prints "r p d w m" from the newest journal throughput line, or nothing
  $JC -u ninfer.service --since "60 sec ago" --no-pager 2>/dev/null \
    | grep "throughput interval" | tail -1 \
    | sed -nE 's/.*running=([0-9]+) prefilling=([0-9]+) decode_ready=([0-9]+) waiting=([0-9]+) materializing=([0-9]+).*/\1 \2 \3 \4 \5/p'
}

fresh_server() {
  $JC -u ninfer.service --since "120 sec ago" --no-pager 2>/dev/null \
    | grep -q "listening on http"
}

armed_since=0       # A/B
c_armed_since=0     # C
last_progress=-1
c_last_advance=0    # last counter advance (0 = none seen since reset)
grace_until=0
restart_count=0
window_start=0
stopped=0

while true; do
  now=$(date +%s)

  # fresh (re)start seen -> full reset + grace
  if fresh_server; then
    armed_since=0; c_armed_since=0; last_progress=-1; c_last_advance=0
    grace_until=$((now + 90))
  fi

  state=""; csum=""
  s=$(poll_stats)
  if [ -n "$s" ]; then
    read -r r p d w m csum <<< "$s"
    state="$r $p $d $w $m"
    if [ "$last_progress" = "-1" ] || [ "$csum" -gt "$last_progress" ]; then
      last_progress=$csum
      c_last_advance=$now
    fi
  else
    state=$(poll_journal_state)
  fi
  echo "$state" | grep -qE '^[0-9]+( [0-9]+){4}$' || state=""

  if [ -n "$state" ]; then
    read -r r p d w m <<< "$state"
    work=0
    { [ "$r" -ge 1 ] || [ "$p" -ge 1 ] || [ "$d" -ge 1 ] || [ "$w" -ge 1 ] || [ "$m" -ge 1 ]; } && work=1
    # A/B (v2 logic, unchanged)
    if [ "$r" -ge 1 ] || [ "$p" -ge 1 ] || [ "$d" -ge 1 ]; then
      armed_since=0
    elif [ "$w" -ge 1 ] || [ "$m" -ge 1 ]; then
      [ "$armed_since" = "0" ] && armed_since=$now
    else
      armed_since=0
      c_armed_since=0               # all-zero line drains work -> C disarms
    fi
    # C arm: work outstanding + >=150s without a counter advance
    if [ "$work" = "1" ] && [ "$c_last_advance" != "0" ] \
       && [ $((now - c_last_advance)) -ge 150 ] && [ "$c_armed_since" = "0" ]; then
      c_armed_since=$now
      echo "WEDGE-C ARMED: work in flight (r=$r p=$p d=$d w=$w m=$m), no engine progress for $((now - c_last_advance))s — restart in 15s"
    fi
  fi
  # no signal: A/B hold; C holds (c_last_advance simply goes stale)

  # restart decision (shared by all classes)
  ab_due=0; cd_due=0
  [ "$armed_since" != "0" ] && [ $((now - armed_since)) -ge 150 ] && ab_due=1
  [ "$c_armed_since" != "0" ] && [ $((now - c_armed_since)) -ge 15 ] && cd_due=1
  if { [ "$ab_due" = "1" ] || [ "$cd_due" = "1" ]; } \
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
      [ "$ab_due" = "1" ] && echo "WEDGE (A/B): engine idle with work pending $((now - armed_since))s — restarting ninfer (restart #$restart_count)"
      [ "$cd_due" = "1" ] && echo "WEDGE-C: no engine progress with work in flight — restarting ninfer (restart #$restart_count)"
      sudo -n systemctl restart ninfer.service 2>/dev/null || systemctl restart ninfer.service
      armed_since=0; c_armed_since=0
      c_last_advance=0; last_progress=-1
      grace_until=$((now + 90))
    fi
  fi
  sleep 15
done
