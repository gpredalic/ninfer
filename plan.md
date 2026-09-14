# Host-KV plan — concise (2026-09-14)

Step-for-step working plan. Shipped work: `implemented.md`. Design context,
full invariant text, and investigation history: `plan-reference.md`.

## Goal — unity

**A cache unit is {KV + state}, atomically.** Captured, retained, budgeted,
evicted, and restored as ONE indivisible unit — never store, budget, or evict
either half on its own (full invariant: `plan-reference.md`, "Standing plan").

Every failure class we chased this week is an instance of the unit being split
across three stores with separate capacities, separate LRU, and separate
lifecycles (host-KV pool 30 GiB / host state pool 24 × ~147 MiB / compact-prefix
safety net):

| Symptom | Split that caused it |
|---|---|
| Restart/wedge loop (09-14) | state slot evicted, KV kept → "no resident state" → error loop |
| Mid-conversation re-prefills (12.17M tokens today) | state-slot LRU evicts a live continuation; KV pool 25% full |
| 19s safety-net ttft | continuation gone, only the shared-prefix half survives |
| `rewrite_checkpoint_invalid` ckpt-miss | half-unit stored (KV + prefix without a valid checkpoint) |
| Fit-gate deadline aborts | state pool saturated while KV pool has room |

**Unity is the main goal.** Fix the split and the follow-up list collapses.

## Status (2026-09-14, post-deploy 20:04)

- Server stable: 0 adopt errors, 0 orphans at deploy; bounded deadline aborts
  work; slow-path fallbacks complete. Restart causes eliminated (see
  `implemented.md`).
- Residual: the state-slot pool is the binding constraint (24 × 147 MiB);
  ~11 slow fallbacks/day; **1 true orphan observed post-deploy**
  (`LEAK (orphan)`, `shared_refs=0`, `endpoint-write`, `ckpt_refs=1` — recurring,
  stable signature).

## Tasks, in execution order

### P0 — quick wins (hours each, do before starting #7)

- [ ] **P0.1 — e2e deterministic checkpoints** (old #3). Force reasoning in
      phases 5/6 like phase 4 (`s.args.thinking_mode = True` +
      `reasoning: {effort: low}` in the payload) — ~5 lines in
      `tools/e2e/ninfer-e2e.py`.
      *Exit:* two consecutive full e2e runs with 0 `rewrite_checkpoint_invalid`
      flaps.
- [ ] **P0.2 — retry detection** (old #4c). Identical prompt re-sent within ~60s
      re-prefills from scratch (seen 3× at 16:36–16:46, 385k tokens each; 2× at
      19:45–19:46, 400k each — ~2.5M tokens of pure retry waste today). Detect
      the repeat and serve from the just-completed continuation.
      *Exit:* e2e retry scenario; 0 full re-prefills on repeated prompts.
- [ ] **P0.3 — state-pool stopgap** (old #1b-lite). `--host-state-slots 24→48`
      in `~/.config/ninfer.conf` (147 MiB/slot → ~7 GiB RAM). One-line ops
      change that halves state-pool pressure until #7 lands.
      *Exit:* fewer "no resident state" fallbacks in the journal; no host OOM.
      (Superseded by #7 — do not build on it.)

### P1 — #7: the unit (checkpoint + KV as one unit) — the main development effort

The standing plan already declares the invariant; the code violates it in four
ways: (1) two independent host budgets (KV pool vs state-slot pool); (2)
independent LRU — the state pool evicts a live continuation while the KV pool
sits 25% full; (3) half-units get stored (`ckpt-miss:
rewrite_checkpoint_invalid`, `ckpt_frontier=0`); (4) "state without KV" is a
retained form (the 19s safety-net class). #7 implements the invariant.
**Subsumes:** #1a (planner-side H2D demand), #1b (pool right-sizing), #4a
(per-session slot guarantee), #4b (idle-aware retention), the 19s safety-net
class, the ckpt-miss class.

- [ ] **P1.1 — scope in plan mode.** Refactor spans `StateImageStore`/
      `HostStatePool`, the host-KV pool, spill/safety-net paths, RM accounting,
      and the planner cost model. *Exit:* approved plan with slice boundaries.
- [ ] **P1.2 — Slice 1: unit identity + cost model.** A continuation owns one
      unit {KV replica, frontier checkpoint, compact prefix}; one cost =
      `kv_bytes + state_image_bytes`; RM accounts one number.
      *Exit:* unit-identity unit tests; accounting matches the sum of parts.
- [ ] **P1.3 — Slice 2: atomic admission.** Admit only when the whole unit fits
      the shared host budget; `rewrite_checkpoint_invalid` becomes an admission
      *failure*, not a degraded half-spill.
      *Exit:* 0 `ckpt_frontier=0` units in e2e + journal.
- [ ] **P1.4 — Slice 3: atomic spill/restore.** The unit moves to host / back as
      one; "KV on host + no resident state" becomes impossible.
      *Exit:* 0 "no resident state" lines under pool pressure (e2e phase 12).
- [ ] **P1.5 — Slice 4: unit LRU/retention + per-session guarantee.** Evict
      whole units, cost-aware smallest-first; protect an interactive session's
      unit (≥2 slots: turn-closure + checkpoint); idle-aware recency weight.
      *Exit:* 0 mid-instance full-root re-prefills in a long-session e2e; the
      19s safety-net class is gone.
- [ ] **P1.6 — config.** `--host-state-slots` derived from (or replaced by) the
      shared budget; document the single `--host-cache-mib`.

*#7 exit criteria:* e2e full suite + 3-session long conversation: 0 full-root
mid-conversation re-prefills, 0 ckpt-miss half-units, 0 "no resident state",
no restarts during a 1-hour saturated run.

### P2 — #2: strip-collapse (`--strip-thinking-cache`)

Re-prefill only the 7 RoPE-bound attention layers after a thinking-strip;
restore the 21 GDN/SSM layers from the position-independent state image.
4× faster cancellation, ~80× less arena. Design: `plan-reference.md`,
"Strip-Thinking-Cache". **After #7** — its state-image-restore path sits on the
unit model; building the unit first makes this cheaper.

- [ ] Scope (spill path, state-image restore, device-KV free, new CLI flag,
      identity/entitlement interaction).
- [ ] Implement + e2e-verify (cancellation turn ~4× faster; arena ~80× less).

### P3 — long tail

- [ ] **True-orphan investigation (live, post-deploy).** First
      `LEAK (orphan)` (`shared_refs=0`, `endpoint-write`, `ckpt_refs=1`)
      observed after the 20:04 deploy — recurring with a stable signature.
      Trace the unbalanced retain/release pair on the endpoint-write
      checkpoint-reference path. Every orphan leaks a state slot forever →
      feeds the pool saturation that drives #4-class losses.
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
   pressure), and makes e2e a trustworthy gate before the big refactor.
2. **#7 is the unifying work:** every other cache pain is an instance of the
   unit being split; fix the split and the follow-up list collapses.
3. **#2 after #7:** strip-collapse's restore path must sit on the unit model
   anyway.
4. **Long tail last:** each item is either subsumed by #7 or shrunk by it.

## Evidence base (for #7 sizing)

From the 09-14 request-log reconstruction (commit `2e83ec50`): e2e 156
mid-conversation roots / 2.78M tokens (all full roots); prod 119 / 12.17M
tokens — 75 true full roots (27 restart-induced, gone with the adopt-fix
deploy; 48 mid-instance losses concentrated in 200k–400k-token conversations,
every turn) + 44 safety-net roots (0.29M tokens but 19s ttft). Mechanism:
state-slot pool saturation (hs sawtooths 0→24→0 with no requests in flight) →
LRU evicts a live continuation before its next turn (1–4 min human gaps) →
no candidate → full root prefill.
