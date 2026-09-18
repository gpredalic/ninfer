# NInfer — ops rules

Prod = the user's live Claude Code session on this WSL2 host
(`ninfer.service`, Qwen3.8-27B). A deploy/restart interrupts the user.
Current state lives in `plan.md` — keep its status sections current with the code.

## Monitoring
- Keep a journal monitor armed during a soak/prod. It expires at 30 min —
  re-arm on expiry and after any stop.
- Pattern (full, not crash-only):
  `CUDA error|cudaError|bad_alloc|terminate called|Assertion|Segmentation|core dumped|Killed [0-9]|WEDGE|WORKER RECOVER|planner-no-plan|relief-shared|host-state-pool|host-arena|single_alloc`
  on `journalctl -u ninfer.service -f -q --output=short-iso`.
- One monitor at a time (duplicates double-notify). After stopping one,
  `pkill -f 'journalctl -u ninfer'` any orphaned process.

## Working
- Soak data (net/census/evictions) accrues only while driving the server with
  requests. Never offer "wait / let it accumulate" as an option — proceed, and
  keep working.

## Deploys
- E2E swap = ONE blocking foreground command, ~8 min session freeze:
  `E2E_TIMEOUT=420 bash ~/ninfer-e2e/e2e-swap.sh`. Never split it, never detach.
- The Bash classifier routes through NInfer; it may block swap/restart during
  prefills or thrash. Don't hammer retries — the user runs it with `! <cmd>`.
- A restart wipes the safety net; freshly spilled entries classify `dead`
  (unmatched) until their first hit — correct, not a bug.
- Build: `cd build && cmake --build . --target ninfer-serve
  ninfer_qwen3_6_host_kv_safety_net_test`; run the safety-net test after net
  changes.

## Observability
- Journal: `journalctl -u ninfer.service --since "..." --no-pager`.
- Stats: `curl -s http://127.0.0.1:8080/stats` → `host_kv.tier_census`
  (per-tier {entries,bytes}: dead/live/idle/active), `host_kv.unit_bytes`,
  `pressure.*`, `scheduler.*`.
- Eviction lines carry `tier=` (dead/live/idle/active); the reaper logs
  `[host-state-pool] reap=stale`.

## Commits
- User-directed. `fix(scope):` / `feat():` / `chore():` / `docs(plan):` +
  descriptive body, no attribution line. Commit plan.md status updates with
  the change they describe.
