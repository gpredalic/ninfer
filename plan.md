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

## Status (2026-09-15, 00:24 redeploy)

- **P1.4 true-orphan leak fixed** (`29df794f`): the recurring
  `LEAK (orphan)` (3 today, all `endpoint-write ckpt_refs=1 shared_refs=0
  DeviceOnly`, each after an error-burst recovery) — a publish-abort
  restores the recycled write destination with refs=1 set directly, and no
  path ever released that reference. Now recorded in
  `SequenceState::recycled_write_state` and released at teardown.
- **00:10:52–00:10:55: `runtime Begin summary differs from committed
  admission` — req 21 and 23 (the user's live session, erroring every
  turn).** A source-based admission (shortlist HIT, reuse 107472/108487)
  whose source's state image had been demoted to host while a sibling
  request ran degraded to the documented Root + safety-net fallback at
  materialization; the engine's Begin check only anticipated reuse
  *growing* and threw on the *downgrade* — and the recovery failed every
  other in-flight request too. Fixed `ce753306`: the documented downgrade
  (committed non-Root → runtime Root, content-verified) is accepted with a
  diagnostic; all other path/reuse changes stay fatal. Structural home:
  P2.3 atomic admission.
- **23:00–23:10 episode → 23:12:04 sentinel restart.** `materializing=1`
  sustained 22:59:45–23:03:35 (+23:08:25–23:09:30), engine ticker degraded
  to 25–30s cadence; req 21 then sat on `KV capacity defer: need 5548
  pages, free 2370→2462` until the 120s deadline abort (23:10:21, "cancelled
  generation cannot be serialized"). The 5548-page demand exceeded free
  pages even if every idle-continuation demotion succeeded — the pool is
  structurally over-committed for this working set (P1.5).
- **The wedge sentinel fired twice** (22:56:12 restart #1, 23:12:04
  restart #2 — both class-A `w≥1` wedges; the 23:12:04 stop was the
  sentinel, not manual). The user-experienced "sentinel not working" was
  the **class-B `m=1` blind spot** (22:40–22:52, 22:59–23:09 stalls — v1
  required `materializing=0`), plus two v1 weaknesses: a poll with no
  journal line reset the timer while a wedged engine degrades its ticker
  to 25–30s, and on-disk edits never reach the running process (bash
  parses the loop at startup). v2 (`478054b9`) fixes all three: /stats
  primary, journal fallback, armed timer held through silence.
- **The 23:10:21 wedge root-caused and fixed** (`20213d4b`): req 21's
  120s-defer abort released its slot (generation++); the client retry
  (req 22) hit a catalog entry still holding the old handle;
  `inspect_admission` threw `admission source continuation is stale`; the
  worker's logic_error recovery left the request pending but the one-shot
  `admission_check_pending` signal was already consumed →
  `should_attempt_admission` false forever → `waiting=1`, GPU 0%, until
  the sentinel restarted at 23:12:04. Same mechanism as the 13:28:57
  wedge (req 150, 4+ min unadmitted). Fix: recoverable worker exceptions
  re-arm the admission check (bounded fail-all after 8 consecutive
  recoveries), and a stale admission candidate is now skipped (inspect
  MISS) instead of throwing.
- **Relief ranking fix deployed** (`a53db4b4`): victims ranked by *unique*
  resident pages (text + backend), batched 3/tick — the 22:28 class
  (demote 29k mapped pages, free 77) now targets what a release actually
  frees. Helps marginal fits; cannot by itself fix the 23:09 episode
  (needs the prefix-scale lever — P1.5c/d).
- **P0.3 applied** (`--host-state-slots 48`, verified live): the 23:09
  episode's upstream cause was state-pool saturation (24/24) — the
  incoming 323k-token request's checkpoint was evicted at admission, so
  selection demanded a full-root 5548 pages instead of the ~24k-token
  delta the shortlist showed at abort time.
- Earlier (post-deploy 20:04 / restart 20:46): `private source result is
  missing` 44 → 0 (adopt fix); P1.1 `inactive capability` cleared
  (`81933c0a`); P1.3 stall relief in place (`47406f79`); 3 sporadic error
  classes + 1 true orphan remain open (P1.2/P1.4).

## Tasks, in execution order

### P0 — quick wins + gate (hours each, do first)

- [x] **P0.1 — e2e deterministic checkpoints** (old #3). Force reasoning in
      phases 5/6 like phase 4 (`s.args.thinking_mode = True` +
      `reasoning: {effort: low}` in the payload) — ~5 lines in
      `tools/e2e/ninfer-e2e.py`. `8530a45b`. *Exit:* two consecutive full e2e
      runs with 0 `rewrite_checkpoint_invalid` flaps. **This makes e2e a
      trustworthy gate for the P1 fixes.**
- [ ] **P0.2 — retry detection** (old #4c). **Re-scoped 00:15:** the 23:10
      episode showed the content-keyed shortlist already serves an
      identical re-sent prompt (`[shortlist] HIT reuse_tokens=295509` on
      req 22, the retry after req 21's abort) — no full re-prefill. The
      observed retry waste was the *wedge→restart* cycle (retry hangs →
      restart → cold re-prefill), which `20213d4b` addresses. *Exit:* after
      a live window on the fixed binary, check the request log for genuine
      same-prompt re-prefills (`reuse=root` within ~60s of an identical
      prompt); build the hash-ring detector only if the data shows them.
- [x] **P0.3 — state-pool stopgap** (old #1b-lite). `--host-state-slots 24→48`
      in `~/.config/ninfer.conf` (147 MiB/slot → ~7 GiB RAM). Applied 23:55,
      verified `host_state_capacity_slots: 48`. Justified by the 23:09
      episode: the state pool was 24/24, the incoming 323k-token request's
      checkpoint was evicted at admission, selection fell back to a full-root
      5548-page demand (vs a ~24k-token delta once the frontier was back in
      the shortlist at abort time), and the pool could not fit it → 120s
      deadline abort. (Superseded by #7 — do not build on it.)

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
      the unit torn apart mid-eviction. **Fixed 00:20 (`ce753306`):
      `runtime Begin summary differs from committed admission`** — the
      00:10:52/00:10:55 episode (req 21/23, user's live session erroring
      every turn): a source-based admission whose source's state image was
      demoted to host while a sibling request ran degraded to the documented
      Root + safety-net fallback at materialization; the engine's Begin check
      only anticipated reuse *growing* and threw on the *downgrade*, and the
      recovery also failed every other in-flight request. The runtime Begin
      is content-verified, so the documented downgrade (committed non-Root →
      runtime Root) is now accepted with a diagnostic; all other changes
      stay fatal. **Fixed 02:03 (`ea934911`): `staged MTP bridge is outside
      the reusable suffix`** (req 2, first turn after a restart, 22:10:58):
      the root fallback reset reuse to Root but left the staged BeforeSuffix
      bridge computed against the old base (the bridge invariant is
      `cursor==base && base!=0`); the bridge site threw on the now-Root
      reuse. Fix: the root-fallback reset clears `mtp_bridge = None` on both
      the prefill and the plan (a Root reuse has no reusable suffix), and the
      bridge site degrades gracefully (log + prefill without the bridge)
      instead of throwing. **Fixed 05:58 (`622c841d`): `capture replacement
      capability is stale` — a P1.5c regression**: 0× Sep 14, 38× Sep 15,
      every firing 1–3s after a `[relief-kv] … released idle shared prefix`
      line (stage 2, `f02bc048`, deployed ~01:00). Root cause: stage 2
      releases idle shared prefixes, but a pending capture may have chosen
      one as its replacement victim at admission — the victim is only pinned
      to ReservedReplacement at reserve time, so the relief releases it in
      the admission→reserve gap and `inspect_capture` threw. Fix: at reserve,
      if the victim slot is now Free, degrade to a no-replacement capture
      (the freed slot is exactly what the no-replacement branch reserves); a
      stale-but-not-Free slot is a genuine conflict and still errors.
      Structural home: P2.3 atomic admission (commit only a path
      whose unit is resident or atomically restorable). *Exit:* 0 of each in
      a saturated e2e + journal window.
- [x] **P1.3 — device KV: free pages from idle sessions.** `47406f79`. While a
      fit-gate defer is in flight and free pages have not grown for 15s, the
      gate demotes the largest idle continuation to the host safety net and
      re-arms per relief. **23:34 (`a53db4b4`):** victims ranked by *unique*
      resident pages (text + backend, quiet `resident_device_pages` probe)
      and batched 3/tick — the 22:28 episode demoted 29k *mapped* pages and
      freed only 77 (535→612) because victims' pages are shared with the
      live shared prefix; per-victim relief is nearly inert against a
      shared-prefix-pinned pool. See P1.5.
- [x] **P1.4 — true-orphan leak.** `29df794f`. Root-caused: a private
      capture with `recycles_private_state` recycles the rewrite image as the
      fork destination (refs=0); when the publish **aborts** (worker recovery
      mid-materialization — the 20:41:53 error burst), the destination is
      restored via `restore_recycled_checkpoint`, which sets refs=1 directly;
      the fork_pending reset only fires for in-place writers, so for a
      private fork `state.write` still points at the restored image — and no
      path ever releases that reference (it was never created by a
      `retain_checkpoint_reference` call). Fix: record the restored handle in
      `SequenceState::recycled_write_state` at the abort-restore site and
      release it in `release_sequence_state` (handle-match + refs!=0 guards).
      *Exit:* 0 `LEAK (orphan)` lines — 3 today, all pre-fix, all
      `endpoint-write ckpt_refs=1 shared_refs=0 DeviceOnly`, each following
      an error-burst recovery; verify in the live window.
- [ ] **P1.5 — the device-KV pool is structurally over-committed; make relief
      effective.** Quantified 22:28: a 267k-token request (req 25) needed
      4684 pages; the pool had 535 free; 7 idle-continuation demotions
      (29k mapped pages total) freed only **77** pages (535→612) — 99.7% of
      the demoted pages were shared with the live shared prefix, so
      per-conversation demotion frees only each conversation's unique tail.
      Quantified 00:32–00:34 (decisive): req 23 (227k-token conversation)
      needed 4070 pages, free 621; **13 idle-continuation demotions** (each
      reporting 6.6k–7.1k *unique resident* pages — the `a53db4b4` ranking
      works) freed **156 pages total** (621→777). The victims are stale
      turn-continuations of the SAME conversation; their pages stay
      device-resident because the **shared prefix entry** and the active
      continuation still reference them — the metric measures residency,
      not exclusivity. The single conversation's working set (prefix ~224k +
      active ~227k + incoming ~227k ≈ 675k tokens) exceeds the device pool
      (~520k tokens): no per-continuation demotion combination can fit the
      next turn.
      Quantified 23:09 (worse): req 21 needed **5548** pages, free sat at
      2370→2462 through 120s of defers — no combination of idle-continuation
      demotions could close a ~3000-page gap. The "idle continuations" are
      branches of ONE large conversation (user-confirmed), so their unique
      tails are small; the pool is pinned by the shared prefix + the live
      conversation's own working set, and the incoming turn cannot fit by
      construction. Fix direction: (a) rank relief victims by *unique*
      (non-shared) resident pages, not mapped pages — **done `a53db4b4`**;
      (b) batch-demote until the demand fits (bounded per tick), not one per
      15s — **done `a53db4b4`**; (c) make the **shared prefix itself a demotion unit** — **stage 2 shipped `f02bc048`**: when no private victim exists, relief releases the idle shared prefix entry (Catalogued, active_references==0, not the transaction's shared source) with the most resident pages; content survives in the safety net via spilled turn continuations. **Regression:** its admission→reserve race with pending captures (victim chosen at admission, pinned only at reserve) threw `capture replacement capability is stale` 38× — fixed by the `622c841d` degrade-to-no-replacement (see P1.2). Full unit-grade version (D2H spill of the shared prefix as one {KV + state} unit) is P2.4; also noted: the incoming restore allocates NEW device pages for a prefix that is already device-resident (identity-based restore is the deeper fix); (d) admission
      should see the pool's shared-prefix occupancy and queue the request
      (visible queue position) instead of a 120s silent defer. *Exit:* a
      5th-conversation e2e scenario completes (via shared-prefix demotion or
      a fast visible queue) without a deadline abort.
- [ ] **P1.6 — the admission wedge: a queued request must run or fail,
      bounded.** The user's restart trigger is GPU 0% while the session is
      active — signature `running=0 prefilling=0 decode_ready=0` sustained
      with `waiting≥1` or `materializing≥1` (the 13:28:57 wedge: req 150 never
      admitted for 4+ min; the 15:15–15:32 stall: `materializing=1` for ~15
      min; the 22:59–23:10 stall: `materializing=1` 3.5+ min, ticker degraded
      to 25–30s). Both pre-fix, both unverified-gone. **Root cause of the
      class-A wedge found and fixed (`20213d4b`, 00:15):** a recoverable
      worker exception during admission (e.g. `admission source continuation
      is stale` after an abort recycled the slot) consumed the one-shot
      `admission_check_pending` signal; recovery left the request pending but
      never re-armed the signal, so `should_attempt_admission` returned false
      forever — `waiting=1`, GPU 0%, until restart (23:10:21→23:12:04; same
      mechanism as 13:28:57). Fix: recoverable catches re-arm
      `request_admission_check()` (bounded fail-all after 8 consecutive
      recoveries) and a stale admission candidate is skipped (inspect MISS)
      instead of throwing. The wedge sentinel
      (`tools/monitor/wedge-sentinel.sh`, now the standalone
      `ninfer-wedge-sentinel.service`) is the tripwire: it restarts after 90s
      of that state (3-in-30-min cap) — **every firing is a timestamped data
      point to investigate, and the goal is zero firings.** v1 fired twice
      (22:56:12, 23:12:04 — both class-A); the user-experienced gap was the
      class-B `m=1` blind spot. v2 (`478054b9`) after the 23:12 episode:
      /stats primary, journal fallback, armed timer held through engine
      silence (v1 reset on a missing line and ran stale code — bash does not
      reload on-disk edits). *Exit:* (a) the
      15:15 stall class is diagnosed (what state holds `materializing`
      without progress, and why admission stops promoting); (b) every stuck
      state has a bounded progress guarantee — it progresses or the request
      fails within a deadline (the 120s-defer pattern generalized) —
      **shipped for the admission-wedge class by `20213d4b` (re-arm + bounded
      fail-all); verify in a live window**; (c) 0 sentinel firings over a
      full day of live use.

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
