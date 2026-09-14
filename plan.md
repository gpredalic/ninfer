# Host-KV plan — concise (2026-09-14)

Step-for-step working plan. Shipped work: `implemented.md`. Design context,
full invariant text, and investigation history: `plan-reference.md`.

## Goal — unity

**A cache unit is {KV + state}, atomically.** Captured, retained, budgeted,
evicted, and restored as ONE indivisible unit — never store, budget, or evict
either half on its own (full invariant: `plan-reference.md`, "Standing plan").

Every failure class we've chased is an instance of the unit being split across
three stores with separate capacities, separate LRU, and separate lifecycles
(host-KV pool 30 GiB / host state pool 24 × ~147 MiB / compact-prefix safety
net), or torn apart mid-eviction:

| Symptom | Split that caused it |
|---|---|
| Restart/wedge loop (09-14) | state slot evicted, KV kept → "no resident state" → error loop |
| Mid-conversation re-prefills (12.17M tokens today) | state-slot LRU evicts a live continuation; KV pool 25% full |
| 19s safety-net ttft | continuation gone, only the shared-prefix half survives |
| `rewrite_checkpoint_invalid` ckpt-miss | half-unit stored (KV + prefix without a valid checkpoint) |
| Fit-gate deadline aborts | state pool saturated while KV pool has room |
| **`inactive capability` request errors** | **evicted source's state image not released from its publication slot before the fallback republishes** |

**Unity is the main goal.** Fix the split and the follow-up list collapses.

## Status (2026-09-14, post-deploy 20:04, restarted 20:46)

- The dominant loop is fixed: `private source result is missing` 44 → **0**
  (adopt fix, `f6a5bec3`+`b60c37eb`).
- **But the server was restarted again at 20:46** (manual SIGKILL, not a
  crash). Two causes, both still open:
  1. **120s deadline hang** — req 134 (a 176k-token conversation) queued 120s
     then aborted: `KV capacity defer: need 3252 pages, free 1032`, frozen the
     whole time with `running=0`. Idle sessions' **device KV was pinned** and
     never freed, so the fit gate could never fit. The user's own request
     hanging 2 min then erroring is what triggered the restart.
  2. **The root-fallback path still fails downstream.** The adopt fix cleared
     the adopt stage, but every `no resident state` fallback now hits
     `active publication cell retained an inactive capability`
     (`resource_manager.h:2617-2619`) — the evicted source's state image is not
     released from its publication slot before the fallback republishes. 1:1
     with fallbacks (11× post-deploy).
- **Three sporadic request-error classes** remain (pre-existing, all in the
  eviction/fallback territory): `inactive capability` (RM publication slot),
  `replacement effect changed` (`program_impl.h:9243`), `entitlement is
  inconsistent` (`program_impl.h:11565`).
- **1 true orphan** observed post-deploy: `LEAK (orphan)`, `shared_refs=0`,
  `endpoint-write`, `ckpt_refs=1` — recurring, stable signature. Leaks a state
  slot forever → feeds pool saturation.

## Tasks, in execution order

### P0 — quick wins + gate (hours each, do first)

- [ ] **P0.1 — e2e deterministic checkpoints** (old #3). Force reasoning in
      phases 5/6 like phase 4 (`s.args.thinking_mode = True` +
      `reasoning: {effort: low}` in the payload) — ~5 lines in
      `tools/e2e/ninfer-e2e.py`. *Exit:* two consecutive full e2e runs with 0
      `rewrite_checkpoint_invalid` flaps. **This makes e2e a trustworthy gate
      for the P1 fixes.**
- [ ] **P0.2 — retry detection** (old #4c). Identical prompt re-sent within ~60s
      re-prefills from scratch (~2.5M tokens of pure retry waste today). Detect
      the repeat, serve from the just-completed continuation.
      *Exit:* e2e retry scenario; 0 full re-prefills on repeated prompts.
- [ ] **P0.3 — state-pool stopgap** (old #1b-lite). `--host-state-slots 24→48`
      in `~/.config/ninfer.conf` (147 MiB/slot → ~7 GiB RAM). One-line ops
      change that halves state-pool pressure until #7 lands.
      *Exit:* fewer "no resident state" fallbacks; no host OOM. (Superseded by
      #7 — do not build on it.)

### P1 — stop the restarts (the user's actual pain; do before the big refactor)

Until these are done the server keeps forcing restarts, which is what blocks
development. Each is verified with the P0 e2e gate + the live journal.

- [x] **P1.1 — complete the root-fallback path end-to-end.** `81933c0a`. The
      evicted source's catalog entry is now retired (`clear_catalog_entry`) in
      the summary-less Retained branch — the publication slot IS that slot, and
      the stale handle was what threw `active publication cell retained an
      inactive capability` (1:1 with every fallback).
- [ ] **P1.2 — triage + fix the sporadic error classes.**
      `replacement effect changed` (`program_impl.h:9325`), `entitlement is
      inconsistent` (`program_impl.h:11647`), and (new, 22:10:58) `staged MTP
      bridge is outside the reusable suffix` (`program_impl.h:12544`). All are
      the unit torn apart mid-eviction. *Exit:* 0 of each in a saturated e2e +
      journal window.
- [x] **P1.3 — device KV: free pages from idle sessions.** `47406f79`. While a
      fit-gate defer is in flight and free pages have not grown for 15s, the
      gate demotes the largest idle continuation to the host safety net and
      re-arms per relief. **Known limitation (22:26–22:28 episode):** 7
      demotions of ~4200-page continuations freed only 77 free pages (535→612)
      — the victims' pages are shared with the live shared prefix, so
      per-victim relief is nearly inert against a shared-prefix-pinned pool,
      and the request still hit the 120s deadline. See P1.5.
- [ ] **P1.4 — true-orphan leak.** First `LEAK (orphan)` (`shared_refs=0`,
      `endpoint-write`, `ckpt_refs=1`) — recurring. Trace the unbalanced
      retain/release pair on the endpoint-write checkpoint-reference path.
      *Exit:* 0 `LEAK (orphan)` lines.
- [ ] **P1.5 — the device-KV pool is structurally over-committed; make relief
      effective.** Quantified 22:28: a 267k-token request (req 25) needed
      4684 pages; the pool had 535 free; 7 idle-continuation demotions
      (29k mapped pages total) freed only **77** pages (535→612) — 99.7% of
      the demoted pages were shared with the live shared prefix, so
      per-conversation demotion frees only each conversation's unique tail.
      The pool is pinned by the shared prefix + 4 live conversations; a 5th
      cannot fit by construction. Fix direction: (a) rank relief victims by
      *unique* (non-shared) resident pages, not mapped pages; (b) batch-demote
      until the demand fits (bounded per tick), not one per 15s; (c) make the
      **shared prefix itself a demotion unit** — demote it to host as one
      {KV + state} unit (the standing-plan invariant applied to the shared
      side); (d) admission should see the pool's shared-prefix occupancy and
      queue the request (visible queue position) instead of a 120s silent
      defer. *Exit:* a 5th-conversation e2e scenario completes (via
      shared-prefix demotion or a fast visible queue) without a deadline abort.
- [ ] **P1.6 — the admission wedge: a queued request must run or fail,
      bounded.** The user's restart trigger is GPU 0% while the session is
      active — journal signature `running=0 prefilling=0 decode_ready=0
      materializing=0 waiting≥1` sustained (the 13:28:57 wedge: req 150 never
      admitted for 4+ min; the 15:15–15:32 stall: `materializing=1` for ~15
      min). Both pre-fix, both unverified-gone. The wedge sentinel
      (`tools/monitor/wedge-sentinel.sh`, now the standalone
      `ninfer-wedge-sentinel.service`) is the tripwire: it restarts after 90s
      of that state (3-in-30-min cap) — **every firing is a timestamped data
      point to investigate, and the goal is zero firings.** *Exit:* (a) the
      15:15 stall class is diagnosed (what state holds `materializing`
      without progress, and why admission stops promoting); (b) every stuck
      state has a bounded progress guarantee — it progresses or the request
      fails within a deadline (the 120s-defer pattern generalized); (c) 0
      sentinel firings over a full day of live use.

*P1 exit criteria:* a 1-hour saturated window (3 concurrent large
conversations) with **0 request errors, 0 deadline aborts, no restart.**

### P2 — #7: the unit (checkpoint + KV as one unit) — the main development effort

The standing plan already declares the invariant; the code violates it in four
ways: (1) two independent host budgets (KV pool vs state-slot pool); (2)
independent LRU — the state pool evicts a live continuation while the KV pool
sits 25% full; (3) half-units get stored (`ckpt-miss:
rewrite_checkpoint_invalid`, `ckpt_frontier=0`); (4) "state without KV" is a
retained form (the 19s safety-net class). #7 implements the invariant.
**Subsumes:** #1a (planner-side H2D demand), #1b (pool right-sizing), #4a
(per-session slot guarantee), #4b (idle-aware retention), the 19s safety-net
class, the ckpt-miss class — and structurally dissolves the P1 error classes
(the unit is no longer torn apart mid-eviction).

- [ ] **P2.1 — scope in plan mode.** Refactor spans `StateImageStore`/
      `HostStatePool`, the host-KV pool, spill/safety-net paths, RM accounting,
      and the planner cost model. *Exit:* approved plan with slice boundaries.
- [ ] **P2.2 — Slice 1: unit identity + cost model.** A continuation owns one
      unit {KV replica, frontier checkpoint, compact prefix}; one cost =
      `kv_bytes + state_image_bytes`; RM accounts one number.
      *Exit:* unit-identity unit tests; accounting matches the sum of parts.
- [ ] **P2.3 — Slice 2: atomic admission.** Admit only when the whole unit fits
      the shared host budget; `rewrite_checkpoint_invalid` becomes an admission
      *failure*, not a degraded half-spill.
      *Exit:* 0 `ckpt_frontier=0` units in e2e + journal.
- [ ] **P2.4 — Slice 3: atomic spill/restore.** The unit moves to host / back as
      one; "KV on host + no resident state" becomes impossible.
      *Exit:* 0 "no resident state" lines under pool pressure (e2e phase 12).
- [ ] **P2.5 — Slice 4: unit LRU/retention + per-session guarantee.** Evict
      whole units, cost-aware smallest-first; protect an interactive session's
      unit (≥2 slots: turn-closure + checkpoint); idle-aware recency weight.
      *Exit:* 0 mid-instance full-root re-prefills in a long-session e2e; the
      19s safety-net class is gone.
- [ ] **P2.6 — config.** `--host-state-slots` derived from (or replaced by) the
      shared budget; document the single `--host-cache-mib`.

*#7 exit criteria:* e2e full suite + 3-session long conversation: 0 full-root
mid-conversation re-prefills, 0 ckpt-miss half-units, 0 "no resident state",
no restarts during a 1-hour saturated run.

### P3 — #2: strip-collapse (`--strip-thinking-cache`)

Re-prefill only the 7 RoPE-bound attention layers after a thinking-strip;
restore the 21 GDN/SSM layers from the position-independent state image.
4× faster cancellation, ~80× less arena. Design: `plan-reference.md`,
"Strip-Thinking-Cache". **After #7** — its state-image-restore path sits on the
unit model; building the unit first makes this cheaper.

- [ ] Scope (spill path, state-image restore, device-KV free, new CLI flag,
      identity/entitlement interaction).
- [ ] Implement + e2e-verify (cancellation turn ~4× faster; arena ~80× less).

### P4 — long tail

- [ ] **#4d — planner pruning.** Prune long-idle/dead owners from the 4096-target
      search space (e2e symptom: `budget_exhausted=True`,
      `stop_reason=expansion_capacity`). Shrinks further after #7's unit
      lifecycle.
- [ ] **15:15–15:32 stall (pre-deploy, uninvestigated).** ~15 min at
      `waiting=1, materializing=1`. May be error-loop fallout — recheck the
      post-deploy journal before chasing.
- [ ] **jinja carries** (not on the serving path): qwen3.8 artifact embedded
      template rebuild (needs BF16 source); frontend flag exposure
      (`tool_call_format`, `auto_disable_thinking_with_tools`,
      `max_tool_arg_chars`, `max_tool_response_chars`).

## Why this order

1. **P0 first:** hours of work, removes daily waste (retry re-prefills, pool
   pressure), and makes e2e a trustworthy gate so the P1 fixes are verifiable.
2. **P1 stops the restarts** — the thing that actually blocks development. P1.1
   is the direct continuation of the adopt fix (the fallback's next stage);
   P1.3 is what triggered today's restart. Until P1 is done the server keeps
   forcing restarts.
3. **#7 is the unifying work:** every P1 error class and every re-prefill is an
   instance of the unit being split or torn apart; fix the split and the list
   collapses.
4. **#2 after #7:** strip-collapse's restore path must sit on the unit model
   anyway.
5. **Long tail last:** each item is either subsumed by #7 or shrunk by it.

## Evidence base (for sizing)

From the 09-14 request-log reconstruction (commit `2e83ec50`): e2e 156
mid-conversation roots / 2.78M tokens (all full roots); prod 119 / 12.17M
tokens — 75 true full roots (27 restart-induced, gone with the adopt-fix
deploy; 48 mid-instance losses concentrated in 200k–400k-token conversations,
every turn) + 44 safety-net roots (0.29M tokens but 19s ttft). Mechanism:
state-slot pool saturation (hs sawtooths 0→24→0 with no requests in flight) →
LRU evicts a live continuation before its next turn (1–4 min human gaps) →
no candidate → full root prefill.
