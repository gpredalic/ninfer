#!/bin/bash
# ninfer wedge sentinel.
#
# The engine wedges with a request queued and nothing running — GPU 0% while
# the session is active (the user's manual restart trigger: "GPU load stays 0%
# for some minutes"). Journal signature: running=0 prefilling=0 decode_ready=0
# materializing=0 waiting>=1. If that persists 90s, restart the service.
#
# Safety rails:
# - No recent throughput lines (idle session or model loading) never triggers.
# - After a restart, a 60s quiet period lets the model load.
# - 3 restarts within 30 minutes stops auto-restart and alerts (a restart
#   loop would be worse than a wedge: each restart cold-starts every big
#   conversation ~100s).
stuck_since=0
restart_count=0
window_start=0
stopped=0
while true; do
  line=$(sudo -n journalctl -u ninfer.service --since "40 sec ago" --no-pager 2>/dev/null \
         | grep "throughput interval" | tail -1)
  if [ -z "$line" ]; then
    stuck_since=0   # idle or loading — never restart from this
    sleep 15
    continue
  fi
  wedge=0
  echo "$line" | grep -q "running=0 " && \
  echo "$line" | grep -q "prefilling=0 " && \
  echo "$line" | grep -q "decode_ready=0 " && \
  echo "$line" | grep -q "materializing=0 " && \
  ! echo "$line" | grep -q "waiting=0 " && wedge=1
  now=$(date +%s)
  if [ "$wedge" = "1" ]; then
    [ "$stuck_since" = "0" ] && stuck_since=$now
    if [ $((now - stuck_since)) -ge 90 ] && [ "$stopped" = "0" ]; then
      if [ "$window_start" = "0" ] || [ $((now - window_start)) -ge 1800 ]; then
        window_start=$now
        restart_count=0
      fi
      restart_count=$((restart_count + 1))
      if [ "$restart_count" -ge 3 ]; then
        stopped=1
        echo "WEDGE REPEAT: 3 restarts in 30 min — auto-restart stopped, needs investigation"
      else
        echo "WEDGE: request queued, engine idle $(( $(date +%s) - stuck_since ))s — restarting ninfer (restart #$restart_count)"
        sudo -n systemctl restart ninfer.service
        sleep 60   # model load window
      fi
    fi
  else
    stuck_since=0
  fi
  sleep 15
done
