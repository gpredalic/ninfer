# NInfer — operational rules (learned the hard way)

This repo runs the user's LIVE Claude Code sessions on this WSL2 server
(`ninfer.service`, Qwen3.8-27B QUASAR NVFP4, systemd). Every deploy/restart
interrupts the user's work. These rules exist because we keep repeating the
same mistakes.

## Monitoring — do not forget
- **Always keep a journal monitor armed while a soak/prod is running.** It
  expires after 30 min — the expiry notification is the trigger to re-arm
  immediately. Forgetting to re-arm has cost us 30+ min of blindness.
- **Full pattern** (the old crash-only pattern was blind to recoveries):
  `CUDA error|cudaError|bad_alloc|terminate called|Assertion|Segmentation|core dumped|Killed [0-9]|WEDGE|WORKER RECOVER|planner-no-plan|relief-shared|host-state-pool|host-arena|single_alloc`
  on `journalctl -u ninfer.service -f -q --output=short-iso`.
- **One monitor at a time.** Duplicates deliver double notifications. When a
  monitor is killed, its `journalctl` process can be orphaned — check
  `ps aux | grep journalctl` and kill stale ones.

## Soak data only accumulates while working
- The net/census/eviction evidence only accrues while the agent is driving
  the server with requests. **"Let it accumulate" is a fake option** —
  idling produces no data. Never offer "keep accumulating" as an alternative
  to deploying or working; just proceed and keep working.

## Deploys
- **E2E swap = ONE blocking foreground command** (~8 min session freeze):
  `E2E_TIMEOUT=420 bash ~/ninfer-e2e/e2e-swap.sh` — stops sentinel + prod,
  starts the 32k e2e server, runs the focused suite, restores prod + re-arms
  the sentinel. Never split it; never run it detached.
- **The Bash classifier routes through NInfer itself.** It may block
  swap/restart commands (especially during heavy prefills or while prod is
  thrashing). Don't hammer retries; if blocked, the user runs it with
  `! <command>`. Expect transient "classifier unavailable" during prefills —
  batch service calls.
- A restart wipes the safety net: the census restarts from zero, and freshly
  spilled entries classify as `dead` (unmatched) until their first hit —
  that's correct, not a bug.
- Build: `cd build && cmake --build . --target ninfer-serve` (+
  `ninfer_qwen3_6_host_kv_safety_net_test`; run
  `./tests/ninfer_qwen3_6_host_kv_safety_net_test`).

## Observability
- Journal: `journalctl -u ninfer.service --since "..." --no-pager`.
- Stats: `curl -s http://127.0.0.1:8080/stats` — `host_kv.tier_census`
  (per-tier {entries, bytes}: dead/live/idle/active), `host_kv.unit_bytes`,
  `pressure.*`, `scheduler.*`.
- Eviction log lines carry `tier=` (dead/live/idle/active); the O2 reaper
  logs `[host-state-pool] reap=stale ...`.

## Commits & plan
- The user directs commits; style: `fix(scope): ...` / `feat(...)` /
  `chore(...)` / `docs(plan): ...` with a descriptive body (no attribution
  line). Keep `plan.md` status sections current with the code — commit
  status corrections with the work they describe.

## Current arc (2026-09-18)
- P2.4 Slice 3 (whole-unit spill) + P2.5 host-net arc: tiering (Active >
  Catalogued > dead), O0 census, O2 soft-ceiling dead reaper all DEPLOYED;
  O1 sizing documented. Open: P1.5 phase-14 (5-conversation) verification,
  P1.6 admission wedge, P1.8 client-abandon, P3 strip-collapse, P4.2.
