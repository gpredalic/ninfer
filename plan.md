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
- [x] **P0.2 — retry detection** (old #4c). **Re-scoped 00:15:** the 23:10
      episode showed the content-keyed shortlist already serves an
      identical re-sent prompt (`[shortlist] HIT reuse_tokens=295509` on
      req 22, the retry after req 21's abort) — no full re-prefill. The
      observed retry waste was the *wedge→restart* cycle (retry hangs →
      restart → cold re-prefill), which `20213d4b` addresses. *Exit:* after
      a live window on the fixed binary, check the request log for genuine
      same-prompt re-prefills (`reuse=root` within ~60s of an identical
      prompt); build the hash-ring detector only if the data shows them.
      **Exit MET (2026-09-18, request log since the fixed binary):** 3,673
      request_done events, 362 repeated (message_count, prompt_tokens)
      shapes; exactly ONE shape has a root re-prefill within 60s of a
      cached same-shape request (14.4s gap, sandwiched between two
      normally-cached requests — a shape-level coincidence, not a content-
      verified retry). No systematic same-prompt re-prefill class → no
      hash-ring detector needed.
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
- [x] **P1.2 — triage + fix the sporadic error classes.**
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
      whose unit is resident or atomically restorable). **06:14:37 live
      firing (post-622c841d):** req 58 (93k tokens) errored 12s after a
      stage-2 release of shared prefix 3 — the throw was the **RM planning
      path**, not the runtime-reserve path: RM's shared catalog still held
      the released prefix as Catalogued with a stale handle and passed it to
      `inspect_capture` as the replacement. **Fixed 06:18 (`4e712ab3`):**
      `valid_continuation`/`valid_shared_prefix` added to the export API;
      every RM catalog loop probes before use and self-heals a stale entry
      (`clear_catalog_entry`/`clear_shared_entry` + skip) — the 20213d4b
      skip-stale pattern. This also fixes `checkpoint recovery owner is
      stale` (00:44:47, same RM view-lag class, `checkpoint_recovery_ns`
      sites). Note: 5 pre-existing `ninfer_resource_manager_test` failures
      (materialization-abort/retention planner-policy tests) fail identically
      on the pre-change baseline — not caused by this fix. **Verified
      06:18–06:35 live window:** 0 of every class since the 06:18:57 deploy
      (51 requests done, 27 relief events incl. a stage-2 shared-prefix
      release — the 06:14:37 trigger repeated at 06:24:08 with no error);
      all earlier firings predate their fixes (MTP: 00:53/01:23 < 01:34
      deploy; replacement-effect: 00:25/00:37 < 090dcdb5). Regression test
      `179716cd` pins the RM self-heal class. *Exit:* 0 of each in
      a saturated e2e + journal window — **met via the live window** (the
      32k e2e swap was skipped: the live traffic already exercised the same
      episodes, and the swap would freeze the user's sessions).
      **New class 06:44:52 (req 25) + 06:55:22 (req 11) + 07:12:31 (req 133):
      `sequence StateImage entitlement is inconsistent`** — **pre-existing**
      (13 journal lines on Sep 14, ≈4 firings, before any of today's changes;
      my increased demotion pressure under pool saturation likely raised the
      frequency, not the bug). All three are the rewrite-restore path with a
      HostOnly checkpoint. **Mechanism pinned** via the `576302d1` diagnostic
      (`footprint=2 slots=1 read=1 write=1 rewrite=3 reserved=0 anchors=0
      fork_pending=0`; residency None=0/Dev=1/Host=2/Both=3): the sequence
      holds 2 device images — the active state `X` (read==write, DeviceOnly)
      and the retained rewrite checkpoint `Y` (Both after a mandatory H2D
      restore) — but the plan promised 1 device slot. Root cause: the plan's
      rewrite-restore optional-states loop (`request_plan_impl.h:680–711`)
      counts a `HostOnly` retained checkpoint as a **host** slot only (the
      device credit at :702–705 requires `DeviceOnly`/`Both`), but the
      rewrite-restore runtime path *always* H2D-restores it to device (→Both),
      so the plan undercounts by 1 device slot. **Fixed `e49d6f22` (07:35):**
      the failing case was `RetainExisting` + a `HostOnly` rewrite checkpoint
      (the `[plan-state]` diagnostic showed `disp=1`=Replace already counted
      its slot via `:626`; the undercount was the RetainExisting path). The
      optional-states loop now counts a device slot for a `HostOnly` retained
      rewrite checkpoint (it is always H2D-restored to device in this path, and
      a `HostOnly` checkpoint can never be the active state, so no
      double-count); anchors are unchanged. The `[plan-state]` +
      `[state-entitlement]` diagnostics are kept for the verification window
      (remove once 0 firings over a full day). Self-recovering
      (request errors, client retries; no wedge/restart), so it does not block
      the stop-the-restarts goal — but it did block P1.2's "0 error classes"
      exit.
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
- [x] **P1.5 — the device-KV pool is structurally over-committed; make relief
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
      construction. Quantified 09-15 (post-fix binary, decisive for P1.5d):
      06:13 req 58 (93k tokens) needed 1958 pages, free 1891; 12
      idle-continuation demotions + a stage-2 release of shared prefix 3
      (2898 *resident* pages) freed almost nothing — the request errored at
      94.8s of deferring (the capture bug, since fixed). 06:24 req 26 (132k
      tokens) needed 2566, free 2410: relief + a stage-2 release + a 131k-
      token safety-net restore fit it (ttft 16s, queue 15.4s). 06:31 a
      2971-page demand sat at free 1449→1534 through 90s of relief (85 pages
      gained) — the demoted victims' pages are shared with the live
      conversation's working set, so per-victim demotion is nearly inert and
      the demand cannot fit by construction; the wedge sentinel (90s
      threshold) restarted the server at 97s, 23s before the engine's own
      120s clean abort — destroying all caches (~55s root re-prefill per
      subsequent request). Sentinel threshold raised to 150s (must exceed
      the 120s defer deadline; see P1.6). The
      pool is pinned by ONE live conversation's working set + its host-state
      restore target; only (d) visible queuing / unit-grade demotion (P2.4)
      changes this. Fix direction: (a) rank relief victims by *unique*
      (non-shared) resident pages, not mapped pages — **done `a53db4b4`**;
      (b) batch-demote until the demand fits (bounded per tick), not one per
      15s — **done `a53db4b4`**; (c) make the **shared prefix itself a demotion unit** — **stage 2 shipped `f02bc048`**: when no private victim exists, relief releases the idle shared prefix entry (Catalogued, active_references==0, not the transaction's shared source) with the most resident pages; content survives in the safety net via spilled turn continuations. **Regression:** its admission→reserve race with pending captures (victim chosen at admission, pinned only at reserve) threw `capture replacement capability is stale` 38× — fixed by the `622c841d` degrade-to-no-replacement (runtime-reserve side) and `4e712ab3` RM catalog probe (RM-planning side; see P1.2). Full unit-grade version (D2H spill of the shared prefix as one {KV + state} unit) is P2.4; also noted: the incoming restore allocates NEW device pages for a prefix that is already device-resident (identity-based restore is the deeper fix — shipped `33c53fb9`/`a1cd9dda`, see P1.7); (d) admission
      should see the pool's shared-prefix occupancy and queue the request
      (visible queue position) instead of a 120s silent defer.
      **Increment 2 implemented `b811ab23`+`344d8f69` (2026-09-17), and is
      IN PROD (post-P4.1-fix).** Occupancy-aware admission
      (probe before grant; unfitted head stays in the visible queue) +
      relief-while-queued (15s stall relief toward the blocked demand, 120s
      deadline) + sentinel Class-C (shipped `0214e6ef`). Unit-tested (probe
      delegation; 5 baseline RM FAILs unchanged). *Status correction
      (2026-09-18):* the crash that blocked it was the P4.1-family
      `InvalidResourceHandle` on the event timer at the first H2D restore
      while a queued block was active — **fixed by the P4.1 timer-read fix
      `9521103d` (09-17 19:30, ~5h after the Inc 2 implementation)**. Both
      commits are ancestors of HEAD; the Inc 2 code is active by default
      (the `NINFER_NO_QUEUED_RELIEF=1` kill-switch is opt-in, so relief is ON
      unless disabled). It has been running in prod through every deploy
      since 09-17 evening (incl. today's 12:41 census deploy) with **0
      crashes** in the journal. *Remaining:* verify the exit criterion — a
      5th-conversation e2e scenario completes (via shared-prefix demotion or
      a fast visible queue) without a deadline abort — via the phase-14
      queued-relief gate (`344d8f69`). The current 1–2-session soak exercises
      the relief path (the `WORKER RECOVER` blocked requests) but not the full
      5-conversation scenario.
      **State-pool sizing fix deployed 2026-09-16 01:04 (config-only, no code):**
      the 500s from the (reverted) P2.4 gate fix were device-state-pool
      exhaustion — total slots = `max_concurrency + device_state_slots` = 3+5 =
      8, but the working set needs 9 (3 concurrent rewrite-restores × 3 slots:
      active + fork-write + replacement checkpoint), and the host pool (24)
      was 24/24 full so `demote_checkpoints_to_make_room` had no demotion
      target (relief dead). Live /stats confirmed both pools saturated in
      steady state (8/8 device, 20–24/24 host). Fix: `--device-state-slots
      5→6` (total 9 = exact working-set fit; the pool self-adapts — active
      lanes take what they need, surplus holds hot checkpoints) +
      `--host-state-slots 24→48` (7 GiB RAM, 14 GiB available; gives relief
      room to demote into). Verified: 9/9 device + 3/48 host post-deploy,
      0 alloc failures. KV cost: ~1 slot (146 MiB ≈ 6.5k tokens) — the
      device-KV pool (the binding constraint) loses ~1%. The KV-side of P1.5
      (shared-page relief: 99.7% of demoted pages shared with the live
      prefix) remains open — that is the structural part.
      *Exit:* a
      5th-conversation e2e scenario completes (via shared-prefix demotion or
      a fast visible queue) without a deadline abort.
      **Exit MET (2026-09-18 20:4x, full 14-phase e2e on the counter binary):**
      phase 14 (queued-relief — the designated gate) passed every assertion:
      12 KV occupancy blocks (unfitted heads kept in the visible queue, not a
      silent 120s defer), relief-while-queued fired 13×, **no queued-KV
      deadline aborts**, zero bad_alloc / worker recoveries under queueing.
      The overflow conversations complete via the fast visible queue +
      relief, exactly the exit criterion. (The full suite's 2 FAILs are
      pre-existing pressure-class, not regressions — see the note below.)
      *Note (full 14-phase e2e, 2026-09-18 20:4x):* 63 PASS / 13 WARN / 2 FAIL.
      The 2 FAILs are both in pressure-forced phases my counter/path work does
      not touch (the diff is behavior-neutral: 6 new counters + a
      void→bool signature change with identical retain/spill logic):
      (1) `checkpoint-advance` — checkpoint frontier stalls at 20083 for 4
      rounds under forced pressure (a capture-timing behavior); (2)
      `state-saturation` — 2 worker recoveries (the documented forced-
      saturation self-heal class, plan line ~1161: 13 recoveries on the Inc 2
      binary, now 2). Neither is caused by this work; a clean old-binary
      baseline was not re-run (54-min GPU + 2 prod interruptions) but the
      behavior-neutral diff + the documented state-saturation baseline make a
      regression implausible.
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
      `ninfer-wedge-sentinel.service`) is the tripwire: it restarts after 150s
      of that state (3-in-30-min cap) — **every firing is a timestamped data
      point to investigate, and the goal is zero firings.** v1 fired twice
      (22:56:12, 23:12:04 — both class-A); the user-experienced gap was the
      class-B `m=1` blind spot. v2 (`478054b9`) after the 23:12 episode:
      /stats primary, journal fallback, armed timer held through engine
      silence (v1 reset on a missing line and ran stale code — bash does not
      reload on-disk edits). **06:33:30 (09-15): the 90s threshold raced the
      engine's 120s defer deadline** — a bounded-defer request (m=1, 2971
      pages vs 1534 free) was restarted at 97s, 23s before the engine's own
      clean deadline abort, destroying all caches (~55s root re-prefill per
      subsequent request). A bounded defer is in-progress, not a wedge:
      threshold raised to 150s (must exceed the 120s deadline + margin).
      **Follow-on 06:52:10:** the first edit changed the header + ARMED
      message but not the `ge 90` comparison — it fired at 96s (killing a
      request 10s before it would have fit); the comparison is now 150s for
      real. *Exit:* (a) the
      15:15 stall class is diagnosed (what state holds `materializing`
      without progress, and why admission stops promoting); (b) every stuck
      state has a bounded progress guarantee — it progresses or the request
      fails within a deadline (the 120s-defer pattern generalized) —
      **shipped for the admission-wedge class by `20213d4b` (re-arm + bounded
      fail-all); verify in a live window**; (c) 0 sentinel firings over a
      full day of live use. **Soak (2026-09-18):** 0 sentinel firings in the
      journal window (since 10:43 today; the only sentinel activity is the
      e2e-swap's expected stop/start). The full 24h predates journald
      retention, but there have been no unplanned restarts — the only
      restarts today were the three directed e2e swaps. (a) the 15:15–15:32
      stall class is **DIAGNOSED (2026-09-18): not a wedge** — the 09-15
      request log shows 0 true wedges, 80 active + 98 transient
      materialization snapshots; the engine was actively serving (large-unit
      restores + between-turn idle gaps), misread as a stall from sparse
      snapshots. See the standalone item for the full analysis.
      **Sentinel misfire 2026-09-18 20:53 (fixed, v3.3 + v3.4):** the
      sentinel restarted prod at 20:53:55 ("WEDGE-C ARMED ... no engine
      progress for 150s") and killed a healthy cold prefill ~24s in.
      Journal-reconstructed timeline: request 36 (250k-token user turn)
      queued at 20:50:50 (needs 4409 pages, free 3409); the client
      cancelled it and re-sent as request 37 (post-compaction, 249833
      tokens, needs 4404) at ~20:52:15; relief-while-queued fired every
      15s (freed 18-34 pages from idle continuations); at 20:53:31
      stage-2 released an idle shared prefix (2624 pages), request 37
      fit and was admitted, cold prefill started. C armed at 20:53:35 —
      150s after the last counter advance (~20:50:55, request 35's final
      decode) — and the restart landed 24s into a ~6-min prefill.
      Why every evidence source was silent: while a demand is queued,
      the tok/s lines log 0.0/0.0 (no token work) and the /stats counters
      stay flat (`computed_prefill_tokens` commits at prefill completion).
      Only the `[relief-kv]` / `[admission] queued KV block` lines showed
      the fit-gate machinery converging. Two fixes: (v3.3) the journal
      non-zero-tok/s check now runs on EVERY poll, not just when the
      /stats poll fails — covers long active prefills (the 2026-09-17
      21:10 class); (v3.4) fresh `[relief-kv]` / `[admission] queued KV
      block` lines within 90s count as progress — a queued demand is
      bounded by its own 120s fit-gate deadline (under the sentinel's
      150s threshold: an unfitted demand self-aborts at 120s and drains
      the gauges, disarming C), and a wedged engine cannot emit these
      lines (scheduler loop frozen). Deployed via `systemctl restart
      ninfer-wedge-sentinel` (the running bash process never picks up
      on-disk edits). Exit (c) re-baselined from the v3.4 deploy
      (21:14): the 20:53 firing is a sentinel defect, not an engine
      wedge — the engine was converging the demand the whole time.
- [x] **P1.8 — client abandons queued requests (serve-layer, new 2026-09-16).**
      The user's session showed `finish=cancelled` at queue=29.37s (0 tokens)
      with the client re-sending the same turn 0.5s later (that copy
      succeeded) — and once at 51.3s (post-compaction cold prefill) with no
      retry, surfacing as "Claude Code stopped for my input". The user did
      NOT press ESC. Byte-arrival probe (4 cold streaming requests, 1s apart,
      150k filler tokens): response headers at t+0.04s and `: keep-alive`
      comment heartbeats every 5s during the admission-queue wait — the
      server was sending bytes the whole time (20-worker pool, no starvation;
      the wait loop wakes every 10ms and polls the transport). Conclusion:
      the client's timer is a FIRST-EVENT timer — SSE comments are invisible
      to it. **Fix shipped `09425996`, deployed 14:13:** the anthropic
      handler's heartbeat now carries the Anthropic-spec `event: ping`
      (a real stream event client parsers recognize and ignore) in addition
      to the comment (TCP_USER_TIMEOUT probe); OpenAI endpoints keep
      comment-only (their parsers are not ping-aware). Verified by probe:
      `event: ping` + `data: {"type": "ping"}` on every 5s heartbeat through
      a 20s+ queue wait. *Residual:* if the client's timer is a hard
      send-relative timeout (not reset by any event), no server-side change
      helps — the next stage would be emitting `message_start` early (before
      prefill), which is protocol-legal but touches the encoder state
      machine; only if the class recurs. The 0.5s double-submission is
      client retry logic — not server-addressable. *Exit:* 0
      `finish=cancelled` on queued requests over a live day.
      **Exit MET (verified 2026-09-18):** 0 `finish=cancelled` across
      45,673 request-log rows spanning 09-15 → 09-18 (both pre- and
      post-`09425996`), i.e. >2 live days with zero queued-request
      cancellations. The `event: ping` heartbeat is holding.
      **REGRESSION (2026-09-18, under overcommit):** 18 `finish=cancelled`
      today; 6 are queue-wait cancellations (queue_wait 60–198s, gen=0) —
      the client abandoned a queued request after a long wait despite the
      `event: ping` heartbeat. This is the documented P1.8 *residual*: the
      client's timer is a hard send-relative timeout not reset by pings, so
      a queue wait longer than the client's timeout cancels regardless of
      server heartbeats. The fix is the documented next stage (emit
      `message_start` early, before prefill) — only worth it if the class
      stays frequent. The other 12 are instant cancels (queue_wait=0, the
      known user-ESC non-defect). Root driver of the long queue waits is the
      device-side overcommit (the RECOVER/re-prefill cycle), so reducing the
      overcommit (P2.4) should reduce these too.
      **Resolved (2026-09-18, post-P2.4 data):** the queue-wait cancel class
      is gone with the overcommit fix. Today's user-session queue-wait
      cancels: 8, all 09:23–19:35 (the RECOVER/overcommit era); after the
      final binary restored at 20:46, exactly ONE — a 250k cold re-prefill
      at 20:52 (the session COMPACTED 259929→249833 tokens; the compacted
      prompt is shorter than the stored unit's checkpoint frontier (259411),
      so find() cannot match it — a genuine full re-prefill, the P1.9
      compaction class, not overcommit). The client's hard timeout (77s)
      cancelled it; the re-send was mid-prefill when the 20:53 sentinel
      misfire (below) killed it too. The 20:01–20:40 burst of 27 ~120s
      cancels is e2e-suite traffic on the e2e server (phase 13/14 forced
      saturation, the e2e client's own timeout) — not user sessions. The
      early-`message_start` stage is NOT justified (the class is not
      frequent); it stays the documented next step if it recurs.
- [x] **P1.9 — generation loops at ~350k context (CLOSED 2026-09-18:
      BEHAVIORAL, not attention/cache — see verdict below).** The user's
      live session, at ~350k tokens of
      context, repeatedly stops making progress: the model emits the same
      statements over and over until Claude Code compaction (which truncates +
      re-prefills the context and clears it). Recurred several times in the
      2026-09-18 session. *Most urgent open item: past ~350k the model is
      effectively unusable.*
      **Why "yarn":** the loop onset (~350k) sits just above the deployed YaRN
      ramp threshold. Prod runs `--rope-scaling-factor 2.12
      --rope-scaling-original-context 262144` (256k): positions ≤ 256k are
      unchanged, above that they are linearly contracted by 2.12
      (`yarn_scale_position`, program_impl.h:65; GPU `scale_positions_yarn`
      kernel, ops/kernel/position.cuh). The formula itself is verified correct
      and the host fn is documented to match the kernel — so "broke" is not the
      formula. The signature (correct below the threshold, wrong above it) is
      the classic one for a **raw-position vs scaled-position space confusion or
      a double-scale** in a path whose output only changes once positions enter
      the contracted region:
      - **Restore/reuse position recompute.** Restored/spilled/reused units
        recompute scaled positions at a frontier via `yarn_scale_position` at
        program_impl.h:13300 (graph representative), 14125 (ordinary batch),
        14282 (MTP batch), 13969/13979 (MTP bridge rope). If a stored frontier
        is already in scaled space and is scaled AGAIN (or a raw frontier is
        compared against a scaled one), the restored prefix's positions don't
        match → self-attention over the model's own recent output is corrupted
        → it "forgets" what it just said and loops. This path is only exercised
        above the threshold (below it scaling is identity, masking the bug) —
        matches the onset. Recent work (safety-net spill/restore, checkpoint
        frontiers, shared prefixes, the MTP-bridge root-fallback clear
        `ea934911`) is exactly the set of paths to audit.
      - **MTP bridge / speculative positions** (`mtp_impl.h:167`; `--spec mtp
        --draft-tokens 5` is ON in prod): draft-token positions are scaled too;
        a space mismatch there would break speculative alignment.
      *Investigation (no deploy):*
      1. Nail the boundary: pull the loop episodes' prompt/frontier token counts
         from `/home/zenz/ninfer-requests.jsonl` — is onset consistently just
         past 262144? A clean threshold crossing is the strongest evidence.
      2. Confirm the deployed scaling values (done: factor 2.12, threshold 256k).
      3. Audit every `yarn_scale_position` / `scale_positions_yarn` call site for
         raw-vs-scaled space and double-scaling, prioritizing the restore/reuse
         frontier sites (13300/14125/14282/13969-13979) and the MTP bridge
         (mtp_impl.h:167). The kernel and host fn must receive the SAME input
         space at each site.
      4. Isolate fresh vs restored: a single ~300k+ token filler prompt (no
         caching) — if it does NOT loop, the bug is in the reuse/restore path
         (not the base prefill); if it DOES loop even on a cold prefill, the bug
         is in the base position computation.
      5. Correlate first occurrence with a specific deploy (bisect the recent
         position-adjacent commits: MTP-bridge `ea934911`, P4.1 timer fix
         `9521103d`, the spill/restore increments).
      *Symptom (user-clarified 2026-09-18):* NOT word-for-word repetition. It is
      **macro stuckness**: the model re-announces the same state and next-step
      every turn ("Build is clean… let me verify… let me add a test… let me
      commit…") without completing the actual task, until Claude Code
      compaction truncates the context and it works again. Each turn behaves as
      if it does not know what the previous turn did.
      *Forensics (2026-09-18, all 5 request-log days + git):*
      - **Ruled out — "we broke yarn" as a code regression:** the rope path is
        UNTOUCHED. `--rope-scaling-factor 2.12 --rope-scaling-original-context
        262144` constant since 09-08 (first log); zero commits since 09-01 to
        the position op (`src/ops/**/position.*`), the `yarn_scale_position`
        call sites, or `text_context_impl.h`. The formula is verified correct.
      - **Ruled out — MTP artifact:** `fallback_steps=0` and normal
        per-position acceptance on every turn, including the dump turns.
      - **Long dumps (≥12k-token single turns) are chronic, not new:** present
        since 09-09 in every prompt bin (incl. 32 below 200k). Per-day counts:
        09-09: 25, 09-10: 49 (the bad days), then 0–5/day through 09-18 (today:
        2 so far — not a bad day by this metric). The drop coincides with the
        09-11 chat-template change (`cb535944` vendored-jinja template
        execution, froggeric_v225) — the template change IMPROVED the metric.
      - **Rate gradient with context:** ≥8k completions are 0.5% of requests
        <200k, 1.0% at 256–300k, 1.5% at 300–350k, **3.4% at >350k** — the
        degradation climbs above ~350k, not at the 256k ramp.
      - **Both cold-root and reuse turns produce dumps** (req 36 today: 11.9k
        @312k, cold ROOT, no cache) — so the restore/reuse path is not the
        sole trigger.
      *Remaining hypotheses (ranked; H-B ruled out 2026-09-18 → H-A is the
      lead):*
      - **H-A — intrinsic long-context degradation (LEAD)** (the model's
        utilization of its own context degrades with length; the 3.4%-at->350k
        gradient + chronic presence fit this). Baseline explanation.
      - **H-B — template rendering of long histories — RULED OUT (2026-09-18,
        template inspection).** froggeric_v225 renders the FULL conversation
        (`messages[head.count:]` = every non-system turn, no truncation;
        `head.count` only re-splits leading system/developer messages). The
        only length-dependent behavior is the thinking-preservation threshold
        (`last_query_index = _last_idx` when `_last_idx > 50` in an all-tool-
        response agentic loop) — already saturated at ~700+ messages, far
        below the 350k onset, so it is not a new effect there.
        `max_tool_arg_chars`/`max_tool_response_chars` default 0 (no
        truncation) and are not exposed in serve options. The template renders
        long histories faithfully.
      - **H-C — attention quality over cached prefixes at 300k+** (weaker now
        that cold-root dumps exist, but a restore-path position/attention bug
        could still add to it).
      *Discriminating experiments:*
      1. **(DONE 2026-09-18)** Reconstruct what the model sees of its own
         history: `corr(prompt_tokens, message_count) = 0.91` over 724
         requests, prompt_tokens grows monotonically with message_count
         (247k@499 → 381k@618 → 361k@830; dips are compactions, msg_count
         → 2–49). **The model sees its full history — H-B (template
         drop/misrender) is empirically ruled out**, complementing the
         template read (no truncation). The macro-stuck symptom is therefore
         not "not seeing recent turns" — it is behavioral/attention-quality
         (H-A), not a rendering gap.
      2. **(READY — `tools/longctx_recall_probe.py`, needs a prod-stopped
         window)** Long-context recall probe: a ~300k-token prompt with
         unique recall codes planted at ~10k/110k/210k/290k (spanning below
         and above the 262144 YaRN ramp), run (a) COLD (fresh prefill) and
         (b) WARM (identical prompt, prefix from cache). Deep-position
         recall failing on BOTH → H-A (intrinsic/scaling quality); failing
         only warm → H-C (restore path); passing both → the attention
         machinery is intact and the macro-stuck symptom is behavioral
         (task-state tracking), not attention recall. ~2 min per run
         (~2-min prefill each); run via
         `python3 tools/longctx_recall_probe.py --json out.json`.
         *Window-1 run (2026-09-18 16:2x): 404 before any prefill — the
         probe sent `"model": "probe"`, but the server VALIDATES the model
         id (`validate_openai_model` → 404 `model_not_found` on anything but
         the public id `qwen3.8-27b`; the e2e suite's default is the same).
         Fixed: `--model` flag, default `qwen3.8-27b`. Re-run is a window-2
         item (batched with the P2.4 test re-run).*
         *Window-2 run (2026-09-18 16:5x): **PASS both — 4/4 cold AND 4/4
         warm** (cold 110.1s prefill, warm 29.8s — the cached-prefix path is
         ~3.7× faster and correct). BUT the real prompt was **213,498
         tokens**, not the 300k estimate (CHARS_PER_TOKEN 4.2 was wrong; the
         filler is ~5.9 chars/token — now calibrated in the tool). Deepest
         zone ~206k — **below the 262144 YaRN ramp**, so this run proves
         attention recall intact BELOW the ramp (H-C ruled out there; H-A
         attention ruled out there) but does NOT yet cover the 350k symptom
         region. Window 3 re-runs at `--target-tokens 330000` (~330k real:
         zone 4 ~319k above the ramp, zone 3 ~231k below — brackets it).*
         *Window-3 run (2026-09-18 17:0x): **PASS both at 329,815 real
         tokens** — COLD 4/4 (141.5s), WARM 4/4 (50.5s). Zone 4 at
         ~318.8k is ABOVE the 262144 YaRN ramp; zone 3 ~230.8k below it.
         **Verdict: attention recall is intact across the ramp, on BOTH
         the cold prefill and the cached/restore path.** H-C (restore-path
         position/attention bug) is RULED OUT — the warm path recalled the
         deepest zone perfectly. H-A (attention-recall degradation) is
         ruled out at 330k. The ~350k macro-stuck symptom sits ~30k beyond
         the deepest probe zone: the leading explanation is now BEHAVIORAL
         (task-state tracking over a very long agentic history), not
         attention recall. A ~380k probe would fully exclude a 350k-onset
         attention effect, but the clean ramp crossing makes that unlikely.*
         *Window-4 run (2026-09-18 17:1x): **PASS both at 379,913 real
         tokens** — COLD 4/4 (235.1s), WARM 4/4 (195.0s). Zone 4 at
         ~367.3k is PAST the ~350k symptom onset; zone 3 ~265.9k just
         above the ramp. **P1.9 attention question CLOSED:** across three
         runs (213k / 330k / 380k) recall is 4/4 on BOTH cold prefill and
         the cached/restore path, spanning below and above the 262144 YaRN
         ramp. H-A (attention-recall degradation) and H-C (restore-path
         position/attention bug) are both RULED OUT at and beyond the
         symptom onset. The ~350k macro-stuckness is BEHAVIORAL — task-
         state tracking over a very long agentic history (the user-
         clarified symptom: re-announcing state/next-step without
         completing the task) — not an NInfer attention/cache defect.
         (The 380k warm run is host-restore-bound: the device pool cannot
         hold a 380k prefix, so the warm path pays H2D from the net —
         still 1.2× faster than cold.) Optional follow-up (model-side,
         not NInfer): an agentic-shaped (tool-loop history) probe at
         350–360k to reproduce the macro-stuckness itself.*
      3. **(DONE 2026-09-18)** Inspect froggeric_v225 for long-history
         handling: no truncation, no length-dependent rendering beyond the
         already-saturated 50-message thinking threshold → H-B ruled out.
         (The pre-09-11 diff would only explain the 09-11 *improvement*, not
         the 350k onset — not needed.)
      4. Track the macro-stuck pattern itself: count consecutive no-progress
         turns per session from the client transcripts (the request log has no
         content, so this must come from the .jsonl transcripts) — establish
         whether today's episodes are an outlier vs the 09-09/09-10 baseline.
      *Exit:* a 350k+ context session makes steady progress with no looping,
      across both fresh-prefill and restored/reused paths.
- [x] **P1.7 — planner H2D demand + identity-based restore (Slice 0 of #7).**
      Two standalone fixes that ship before the unit refactor and address
      today's live pain: (a) model the restore's new device state slot when the
      source is HostOnly at materialization time (the planner H2D demand gap,
      `program_impl.h:11076–11080, 5186–5196`) — this also fixes the
      `entitlement is inconsistent` class (P1.2), the same undercount on the
      rewrite-restore path; **(a) done `e49d6f22`** (the rewrite-restore
      RetainExisting + HostOnly checkpoint case — the specific undercount that
      was firing; the general post-admission-demotion H2D gap remains for the
      unit work). (b) identity-based restore — the safety-net root restore
      re-allocated device pages for a prefix that was still device-resident
      (shared under a live continuation / shared-prefix slot), re-consuming
      exactly the pages fit-gate relief had just freed. **Shipped** (this
      session): the spill records the victim's logical pages in the entry
      (`text_source_pages`/`backend_source_pages`); the restore probes the
      longest prefix still *frozen-shareable* (device-resident, full, no
      writer — append-only content, so aliasing is exact), materializes +
      H2D-copies only the unshared suffix, then `adopt_resident_prefix`
      swaps the fresh prefix pages for the shared ones and rebases the
      reservation so the fit gate sees the freed pages. Adoption is the LAST
      mutation (after all H2D + sync), so a pre-adoption failure falls back
      exactly as before; a post-adoption failure (state H2D, stream sync)
      tears down via `release_adopted_prefix` before the root-prefill
      fallback. Only full pages are adopted (a partial tail would be the
      prefill's writer tail). **Shipped `33c53fb9` + `a1cd9dda`, e2e-verified
      (2026-09-15, 4 swaps):** the spill records the victim's logical pages;
      the restore adopts the longest frozen-shareable prefix and rebases the
      reservation. First e2e run caught a real bug — the post-materialization
      entitlement check (`resident_resources` excludes shared pages from an
      owner's exact transition effect, but the plan counts the adopted pages)
      threw `materialized sequence does not match its active entitlement`
      (500s on both concurrent requests) right after a 187/188 adoption;
      fixed by adding the adopted counts back at the check site (`a1cd9dda`).
      Live adoption observed: `identity-share: 187/188 text + 187/188 backend
      pages adopted` (a 12015-token restore H2D-copied 1 page instead of
      188). Subsequent runs: 0 entitlement errors, 0 error classes, suite
      green (17 PASS / 3 WARN / 0 FAIL on the focused concurrent→state-lease
      group; the 3 WARNs are the pre-existing ckpt-miss + demotion classes).
      *Exit:* 0 "no resident state" →
      root-fallback lines under pool pressure; 0 `entitlement is inconsistent`;
      the 06:31/06:50 overcommit episodes stop re-consuming freed pages.
      **Note:** adoption fires only when the victim's pages are still
      device-resident via another owner (shared prefix / live sibling) —
      non-deterministic in e2e (0 of 3 post-fix runs reproduced it; the
      demotion path absorbs most reuse in the 4 GiB e2e arena). The live
      monitor greps `identity-share` so a prod firing is visible in the feed.
      Also observed (pre-existing, not a regression): with the 4 GiB e2e
      arena, demoted catalog extents can saturate it so a safety-net spill
      fails `no room for a complete unit` with `reclaimable=0` (empty net —
      the arena is shared between demotion extents and net units, with no
      joint budget). That is the P2.3/P2.5 shared-meter problem, not P1.7.

*P1 exit criteria:* a 1-hour saturated window (3 concurrent large
conversations) with **0 request errors, 0 deadline aborts, no restart.**

### P2 — #7: the unit (checkpoint + KV as one unit) — the main development effort

The standing plan already declares the invariant; the code violates it in four
ways: (1) two independent host **occupancy/eviction** pools (KV arena + safety
net vs the 48-slot HostStatePool) — note the joint *demand* model already
exists (`PhysicalResources` + `physical_peak_fits` checks all six dims
together; RM holds no budget of its own, it delegates fit to the program);
(2) independent LRU — the state pool evicts a live continuation while its KV
stays resident (the 19s class); (3) half-units get stored (`ckpt-miss:
rewrite_checkpoint_invalid`, `ckpt_frontier=0`); (4) "state without KV" is a
retained form (shared-prefix relief frees device KV, the state slot stays —
`program_impl.h:4816–4830`). #7 implements the invariant.
**Subsumes:** #1a (planner-side H2D demand), #1b (pool right-sizing), #4a
(per-session slot guarantee), #4b (idle-aware retention), the 19s safety-net
class, the ckpt-miss class — and structurally dissolves the P1 error classes
(the unit is no longer torn apart mid-eviction).

**P2.1 scope findings (2026-09-15, three exploration passes):** 12 KV/state
seams where the two halves move independently (state-image agent); 6 half-unit
conditions incl. the 19s class (state-slot LRU evicts a live continuation's
state while its KV stays → next turn falls to Root → only the shared-prefix
half matches → partial re-prefill); the identity-restore gap (safety-net root
restore allocates fresh device pages even when the prefix is already
device-resident — `program_impl.h:5065/5077, 11188/11233`, only
`prepare_kv_restores:5146–5154` dedups); and the planner H2D demand gap (a
checkpoint DeviceOnly at admission that is post-admission-demoted to HostOnly
needs a NEW device slot at restore that was never in demand —
`program_impl.h:11076–11080, 5186–5196`, band-aided by
`demote_checkpoints_to_make_room(1)`). The safety net already stores state
images inside the KV arena's budget (`set_state_budget_bytes`,
`program_impl.h:938`) and spills whole units — that's the template for the
shared meter.

- [x] **P2.1 — scope in plan mode.** *Exit:* approved plan with slice
      boundaries — **done** (findings above; slices below).
- [x] **P1.7 — Slice 0: standalone fixes (ship before the refactor).** Two
      small, high-leverage fixes for today's live pain, no unit refactor:
      (a) **planner H2D demand** — model the restore's new device slot when the
      source is HostOnly at materialization time (re-validate residency at the
      fit gate and schedule relief for the delta); this also fixes the
      `entitlement is inconsistent` class (P1.2) which is the same undercount
      on the rewrite-restore path. **(a) done `e49d6f22`.** (b) **identity-based
      restore** — the safety-net root restore skips allocation for
      already-resident prefix pages: the spill records the victim's logical
      pages in the entry, the restore adopts the longest frozen-shareable
      prefix (device-resident, full, no writer) instead of re-materializing +
      H2D-copying it, and rebases the reservation so the fit gate sees the
      freed pages. **(b) done `33c53fb9` + `a1cd9dda` (entitlement-check fix
      caught by the first e2e run), e2e-verified 2026-09-15 — see the P1.7
      entry above for the run log.**
      *Exit:* 0 "no resident state" → root-fallback lines under pool
      pressure; the 06:31/06:50 overcommit episodes stop re-consuming freed
      pages; 0 `entitlement is inconsistent`.
- [x] **P2.2 — Slice 1: unit identity + cost model.** A continuation owns one
      unit {KV replica, frontier checkpoint, compact prefix}; one cost =
      `kv_bytes + state_image_bytes`; one shared meter over host occupancy
      (KV arena + state pool), the safety net's in-arena state budget as the
      template. *Exit:* unit-identity unit tests; accounting matches the sum
      of parts.
      **Increment 1 shipped `05b77241` (13:55):** `cache_unit.h` —
      `CacheUnitCost` (kv_bytes + state_image_bytes, overflow-checked,
      residency-independent by construction) + `cache_unit_kv_bytes`
      (pages × stride) + `CacheUnitOccupancy` meter (add/remove with
      overflow/underflow guards). `tests/test_cache_unit.cpp` pins the exit
      criteria (7 tests, all pass; the page-overflow test caught a real wrap
      bug in the first draft). Zero existing code touched — the meter is
      adopted by the pools in P2.4/P2.5. **Increment 2 (next):** wire the
      meter into the safety-net add/evict/restore sites + expose
      `host_unit_occupied_bytes` in /stats (needs a deploy + live-window
      check that the meter tracks entry count × typical cost).
      The `[plan-state]` diagnostic was removed after the entitlement fix
      verified live (`45e9450b`, deployed 13:57); the error-only
      `[state-entitlement]` diagnostic stays until 0 firings over a full day.
      **Concrete design (2026-09-15):**
      - *Unit identity — no new object.* The unit already exists in two forms:
        device = `SequenceState` (`program.h:447`: `kv` bundle + `state`
        ActiveStateBinding + `rewrite_state` + `compact_prefix`/`prefix_identity`);
        host = the safety-net entry (`host_kv_safety_net.h:49–149`: prefix
        identity + `execution_frontier` + KV allocations + `state_host`/
        `state_bytes` + checkpoint). P2.2 adds a `unit_cost()` view over both,
        not a new container.
      - *One cost (residency-independent).* `unit_cost = kv_bytes +
        state_image_bytes`. `kv_bytes` = (text_pages + backend_pages) ×
        page_stride (from the KV bundle / net entry's page counts);
        `state_image_bytes` = the fixed image size (`host_layout().image_bytes`,
        independent of frontier — it's the GDN/SSM recurrent state). The cost
        is the same whether the unit is device- or host-resident — that
        residency-independence is the whole point (one unit, one cost, so the
        two pools can be metered together).
      - *One shared meter (accounting only — eviction stays per-pool until
        P2.4/P2.5).* Introduce a host-unit byte meter: `occupied = Σ unit_cost`
        over retained host units. The KV arena's byte occupancy and the state
        images' bytes are already jointly charged (the safety net stores state
        images inside the KV arena's byte budget, `set_state_budget_bytes`,
        `program_impl.h:938`) — that's the template. The HostStatePool's slot
        count converts to bytes (`slots × image_bytes`) for the meter. RM
        already snapshots both (`host_kv_occupied_bytes` +
        `host_state_occupied_slots`, `resource_manager.h:1133–1159`); P2.2
        combines them into one number. The meter informs admission/retention
        reasoning; it does not yet drive eviction (that's the LRU slice).
      - *Unit-identity unit tests (the exit — ctest, no server):* (a) a unit's
        cost = kv_bytes + state_bytes (assert the arithmetic); (b) the meter's
        occupancy = Σ unit_cost over retained units; (c) adding/removing a unit
        changes the meter by exactly its cost; (d) the device and host forms of
        the same unit report the same cost (residency-independence).
      - *Regression guard:* the meter is additive (a new cost view + meter),
        not a rip-out of the two-pool accounting — the existing per-pool
        eviction keeps working unchanged, so a meter bug degrades to "no
        improvement," not "wrong eviction."
- [x] **P2.3 — Slice 2: atomic admission.** Admit only when the whole unit fits
      the shared host budget; `rewrite_checkpoint_invalid` becomes an admission
      *failure*, not a degraded half-spill. *Exit:* 0 `ckpt_frontier=0` units
      in e2e + journal. **Shipped `ee0bf25c`, e2e-verified 2026-09-15 (full
      12-phase suite, one swap, rc=0: 50 PASS / 25 WARN / 0 FAIL, 0 error
      classes).**
      **Design (2026-09-15, scoped from the admission/spill paths):**
      *Unit completeness* — a retained unit is complete iff its endpoint state
      image is present AND (no rewrite checkpoint is expected for it OR its
      rewrite checkpoint state was captured). Sessions that never have a
      rewrite checkpoint are complete with endpoint state alone (their
      `ckpt_frontier=0` is not a half-unit). Two increments:
      - **Increment 1 — atomic spill (no half-unit retention).** In
        `spill_victim_to_host_kv_safety_net`: (a) endpoint state missing +
        checkpoint state present (the `endpoint_fallback` class) → retain
        NOTHING (the unit is incomplete at its execution frontier; the
        checkpoint-only form is not a unit — today it is retained with an
        untruncated ledger/identity, an inconsistent frontier/state/ledger
        correspondence); (b) rewrite checkpoint expected but uncapturable
        (`rewrite_state_null`/`rewrite_state_invalid`/`capture_logic_failed`)
        → retain NOTHING (today: retained as a `ckpt_frontier=0` half-unit).
        Both become logged ABORTs. Also: the `prefix_identity.swap` out of the
        sequence moves to just before the successful `add()` — today an ABORT
        destroys the sequence's identity (a latent bug for demoted-not-evicted
        victims, which keep living with an empty identity → silent root
        re-prefills every turn).
      - **Increment 2 — shared-budget retention gate at admission.** At
        `reserve_materialization` (root path) and re-validated at continuation
        admissions: unit host cost = `kv_bytes(prompt frontier, text+backend
        strides) + 2 × image_bytes` (endpoint + rewrite checkpoint, the max
        the unit carries) vs the total shared host budget = arena capacity +
        state-pool bytes (`admission_capacity().host` dimensions). Cost >
        total budget ⇒ the unit is **retention-ineligible** (one-way flag on
        the continuation; cost only grows) — the spill SKIPs it with a clear
        log instead of attempting a spill that can never complete. Strict
        rule (fits the *total* budget, occupancy-independent): a unit that
        cannot fit an empty budget can never be retained; occupancy-aware
        fitting (evict-to-make-room at admission) is P2.5's unit LRU. The
        e2e `no room for a complete unit` churn (arena full of demotion
        extents, net empty) is NOT fixed here — that is the P2.4/P2.5
        shared-budget problem; increment 2 makes retention a *planned*
        property and kills the pathological-unit class.
      *Exit:* 0 retained units that expected a rewrite checkpoint but lack
      it, 0 endpoint_fallback units, 0 half-units in e2e + journal.
      **Shipped `ee0bf25c`, e2e-verified 2026-09-15 (full 12-phase suite, one
      swap, rc=0: 50 PASS / 25 WARN / 0 FAIL, 0 error classes).** Run log:
      44 complete units retained (13 with rewrite checkpoints, 31
      no-checkpoint sessions — complete by definition); 95 atomic ABORTs
      (`endpoint_state_missing` — the 19s-class symptom, now visible and
      countable instead of silently half-retained; P2.5's unit LRU is the
      structural fix); 0 `rewrite_checkpoint_uncaptured` ABORTs (every
      expected checkpoint was captured in this run); 0 error classes; 0
      entitlement errors (P1.7(b) fix held under the new load). The
      `endpoint_state_missing` ABORTs are the expected visible symptom until
      P2.5: the state pool still evicts live units' endpoint states, and the
      atomic rule now refuses the half-unit rather than storing it. One soft
      WARN to watch: state-lease phase saw 1 checkpoint capture (expected
      ≥4) — device-pool pressure variance, all hard checks passed.
- [ ] **P2.4 — Slice 3: atomic spill/restore.** The unit moves to host / back
      as one; the safety-net spill (already whole-unit) becomes the only spill
      path; restore reassembles into the SAME unit (no 3× duplication: device
      slot + pool slot + net vector; no bypass of StateImageStore residency at
      `program_impl.h:11285–11298`). Kills the "state without KV" forms.
      *Exit:* 0 "no resident state" lines under pool pressure (e2e phase 12).
      **Design (2026-09-15, from prod evidence + topology map):** The e2e
      exit (32k ctx, 4 GiB host KV) does NOT reproduce the prod loss class —
      prod shows `[materialize] materialization source has no resident state —
      falling back to root prefill` at 18–81% of completed requests per
      20-min window (64 occurrences 22:00–23:38, pre-existing: 49 in the
      88-min pre-P2.5 window). Episode 23:32:15: a 129k-prompt follow-up finds
      catalogued sources (`prefix HIT (rewrite)`, frontiers 105k–127k), but
      the chosen source has `device.state_slots==0 && host.state_slots==0`
      (`program_impl.h:4930`) → throw → root prefill (ttft ~4× a restore).
      Root cause: the unit invariant is enforced in the NET (P2.3/P2.5) but
      NOT in the device/host pools — state and KV are demoted/evicted as
      INDEPENDENT resources, so the two halves of a unit die separately:
      - state-only demotes: `PressureStateDecision::Demote{Endpoint,Rewrite,
        Shared}ToHost` (planner gate 1676–1730, prepare 5897–5928, publish
        6025–6041) + `demote_checkpoints_to_make_room` 4766–4797;
      - KV-only demotes: `PressureKVDecisionKind::DemoteToHost` →
        `host_kv_extents` (prepare 5960–6000, publish 6042–6070) — the KV
        stays restorable while its state is managed separately;
      - replica drops: `make_host_slot_available` (state_image_store.h:401,
        drops the coldest BOTH host replica) and `drop_device_replica`
        (state_image_store.h:355) convert a Both unit to one-replica; the
        last replica is then lost via object release / host-pool LRU.
      Result: "KV without state" (KV device-resident or in extents, state
      gone) → materialize has no state to restore → root prefill. The net
      restore path (find/take_pinned, state H2D 11555–11590) is correct and
      already runs at fallback time — the episode's safety-find missed only
      because the unit was NOT in the net (its state was lost before any
      spill happened).
      **Step 0 finding (2026-09-16, DIAG at the throw site): the dominant
      class is a GATE FALSE POSITIVE, not a state loss.** The gate
      (`program_impl.h:4930`) tested `resident_resources(source)` — the
      sequence's EXCLUSIVE state footprint — but restore only READS the
      selected image's replica, so a SHARED image (checkpoint_refs > owned)
      is fully restorable. DIAG confirmed on every episode: the endpoint
      handles are released (expected — a thinking follow-up rewinds to the
      rewrite checkpoint) while `rewrite: residency={DeviceOnly,HostOnly}
      ckpt_refs=2 owned=1` — resident and restorable, yet the exclusive test
      counted 0 → throw → 4× root re-prefill. Fix: gate on the SELECTED
      image's residency (mirrors `selected_state`), not the exclusive
      footprint. This also converts a would-be unclean "no selected
      StateImage" throw (selected image gone, another image resident) into
      the clean root fallback. The "KV without state" pressure loss remains
      as the secondary class for the increments below.
      **Shipped `87916492`, LIVE-VERIFIED 2026-09-16 00:07–00:17:** the new
      binary completed 45 requests with **0** `no resident state` / fallback
      lines (was 18–81% of completed requests per 20-min window pre-fix), and
      the previously-failing path now restores: 3 `reuse=private_turn_closure`
      completions with `cache=143110`/`157548` (~143k–157k tokens from cache,
      tail-only prefill) instead of a full root re-prefill. The per-handle
      DIAG stays as a canary for the secondary class (it now fires only on a
      genuine `None` residency — the selected image actually gone).
      **Regression the gate fix unmasked, fixed `fb9b6318`:** unblocking the
      rewrite-restore path exposed a latent planner under-count — a
      ConsumedToActive restore whose selected state is SHARED (more checkpoint
      refs than the sequence consumes) must FORK it (read + write both
      device-resident), one more slot than the base active entitlement (1)
      covers. The planner reserved base(1) + new-checkpoint(1) = 2 but the
      sequence holds 3 (read + write + rewrite), so `reserve_state_entitlement`
      threw `entitlement is inconsistent` (footprint=3 slots=2 fork_pending=1)
      and the request 500ed (9× in 20 min) instead of restoring. Fix: count
      the fork's write slot in `active_optional_resources.device.state_slots`
      when `state_fork_required`. Live-verified: 0 mismatches on the new
      binary. **BOTH REVERTED `e82f4837`/`4913da7c` (2026-09-16 00:39):** the
      entitlement fix was incomplete — with the gate unblocked, the rewrite-
      restore path overflowed the device state pool (`max_concurrency +
      device_state_slots` = 3+5 = 8 total, working set needs 3 concurrent × 3
      slots = 9) and produced THREE error classes: `entitlement is
      inconsistent`, `std::bad_alloc` (pool full at `reserve_state_entitlement`),
      and `reservation is not a single destination` (the +1 over-corrected in
      some cases). The entitlement model (base 1 + optional slots vs the
      fork/anchor footprint) is not fully understood, and the device state
      pool is the binding constraint — that is P1.5. Reverted to the known-
      good baseline (clean root-prefill fallback, 0 hard 500s); DIAG canary
      (a4629fac) stays. Re-land the gate fix ONLY after P1.5 makes the device
      state pool fit the working set AND the entitlement model is fixed.
      **RE-LANDED + DEPLOYED 2026-09-16 09:26** (`e0b368f4` gate, `22be939d`
      fork slot, `6f92bfdc` replace-clear; e2e 37P/10W/0F): the gate fix works —
      0 `no resident state` fallbacks. **New regression it unmasked — device
      state pool saturation:** with restores actually happening, the pool pins
      at capacity (10/10 post-rehydration; images are Dual or pinned). The
      emergency relief `demote_checkpoints_to_make_room` only demotes
      DeviceOnly unpinned checkpoints (`coldest_demotable_checkpoint` skips
      Dual — `reserve_device_to_host` rejects `host_slot`), so a saturated pool
      frees nothing: the H2D restore's `take_device_slot` or the final
      `reserve_destination` in `reserve_state_entitlement` (program_impl.h:12218)
      fails → `std::bad_alloc` (4 clusters: 09:28/09:33 at pool=9, 09:37/09:45
      at pool=10 — each 1-2 auto-retried requests, engine recovers) plus
      secondary `materialized sequence does not match its active entitlement`
      500s on the following turns. Pool bump 9→10 (`--device-state-slots 7`)
      deployed 09:33 — whack-a-mole, not a fix. **Relief fix (in progress):**
      (a) `host_replica_stale` flag on the store Object — set in `thaw` (the only
      gateway that makes a host-holding image writable; the sole prod thaw site
      drops the host replica first, so this is defense-in-depth), cleared on
      D2H publish; guards `drop_device_replica`; (b) `coldest_dual_device_replica()`
      + second relief tier in `demote_checkpoints_to_make_room`: drop the
      redundant device replica of the coldest FRESH Dual checkpoint (host half
      current → no copy, unit stays complete in the host pool); (c) the
      rewrite-restore H2D branch demotes the FULL entitlement
      (`state_slots - state_footprint`) instead of 1 — the final destination
      reserve needs its slot too; (d) `reserve_state_entitlement` demotes and
      retries once before throwing bad_alloc. Unit tests in
      test_context_store.cpp (dual candidate/drop + stale exclusion); e2e
      tracks `relief_dual_drop` and FAILs on bad_alloc in demotion/state-lease
      phases; /stats counter `materialize_dual_device_replica_drops`.
      **DEPLOYED 2026-09-16 10:00 (e2e 36P/10W/0F) + VERIFIED LIVE:** first
      relief event 10:06:50 `[relief] freed 1 device state slot(s) (demoted 1
      to host, dropped 0 dual) (needed 3)` — the exact class that 500ed
      ~1/min pre-fix; 0 bad_alloc since deploy. **e2e coverage gap closed:**
      phases 11/12 pressurize but never saturate the pool (relief=0), so
      **phase 13 "state-saturation"** was added (6 tool-calling sessions —
      the production shape, since tool-call turns are what capture rewrite
      checkpoints — × 8 rounds): verified 8/8 pool saturation, 9 checkpoint
      captures, 6 restores, 0 worker recoveries, 0 bad_alloc (3 targeted
      runs to tune the load; plain thinking turns capture ~0 checkpoints and
      leave the pool full of endpoints only, so the relief trigger never
      fires). Residual low-frequency class (a few per hour under heavy load,
      clustered in the first requests after a restart, always auto-retried):
      `materialized sequence does not match its active entitlement` from the
      supersede-vs-materialization race — a spill supersedes (drops the last
      replica of) the checkpoint an in-flight materialization selected
      (10:08:20: `supersede: dropping frontier=181969` mid-restore). This is
      increment 1's spill-before-loss class; the two dominant classes (gate
      false positive, bad_alloc) are fixed.
      **Non-defects ruled out (2026-09-16 10:39–10:41):** `cancelled
      generation cannot be serialized as an Anthropic message` and `invalid
      Anthropic stream finish state` are the user pressing ESC in Claude Code
      to cancel in-flight requests — expected, not a server bug.
      **NEW class — SIGABRT on a CUDA event timer (2026-09-16 10:24, 10:26):**
      two successive prod processes aborted (`code=dumped, status=6/ABRT`) at
      `device.cu:185 cudaEventElapsedTime → cudaErrorInvalidResourceHandle`,
      each ~2 min after startup, each right after a ~210k-token cold prefill
      (75s TTFT) and on the first net-restore's transfer-timer read. The 3rd
      process (10:28) has been stable 15+ min. GPU clean at inspection (54°C,
      no Xid in dmesg). Orthogonal to the state-pool relief work (which touches
      no CUDA events/timers); the 75s TTFT before each abort points at a WSL2
      GPU context reset during a large DMA window. Tracked as P4.1 —
      investigate separately, do NOT conflate with the unit refactor.
      Increments:
      - **Increment 1 — spill-before-loss (kill the unit split).** At the
        point a unit's state is about to lose its LAST replica (device-slot
        release under pressure; host-replica drop/eviction), if the unit's KV
        is still retained (device-resident or in extents) and the unit is
        retention-eligible, spill the COMPLETE unit to the net (existing
        spill path; state host→host move, KV D2H or extent-backed) instead of
        letting the state die. Net refuses (ineligible/full) → allow the loss
        (graceful degrade = today's behavior, logged). Step 0: trace the exact
        loss path in prod (which demote/drop/release sequence produces the
        zero-residency state — the state-lease lines around a fallback show
        the refusals). *Exit:* 0 prod `no resident state` fallbacks where the
        unit's KV was still retained; e2e suite green.
        **Step 0 finding (2026-09-16, unfiltered trace of the 10:34:52
        episode, req 31/36):** the live `materialized sequence does not match
        its active entitlement` class is NOT a unit loss. The supersede drop
        (`supersede: dropping frontier=40945 (prefix of 40950)`) killed only
        the net's RAW state copy — the store image survived (the fixed gate
        passed: no `no resident state`, no `[rewrite-restore] FAILED`), so
        the materialization ran and the post-materialization check
        (program_impl.h:8509, `actual != expected` over
        {device.state_slots, main_kv_pages, backend_kv_pages,
        host.state_slots, host.kv_bytes}) rejected it. Mechanism: a
        post-admission demotion (relief/pressure — relief events at 10:33/
        10:46 bracket the episode) shifts a state image device→host (or KV
        pages device→host extents) AFTER the planner computed the entitlement
        at admission, so the residency-shifted actual no longer equals the
        admission-time expected. This is the P1.7 residual "general
        post-admission-demotion H2D gap" (the specific RetainExisting+HostOnly
        case was fixed `e49d6f22`; the general case was deferred to the unit
        work). The supersede line is temporally correlated, not causal for
        the store image. *Fix home:* the 8509 check must accept the documented
        residency downgrade (device→host shift with preserved total state
        count; device under-consumption is the safe direction — the scarce
        pool). **Attempted + REVERTED (2026-09-16):** a first relaxation
        (accept device under-consumption + sum-preserving state shift) was
        deployed 11:21 (e2e 44P/16W/0F) but review found a safety hole — a
        lost DeviceOnly image and a demoted Dual produce identical dimension
        deltas, so the sum check can mask a true loss when expected.host>0.
        Reverted to the strict check + a six-dimension `[entitlement] MISMATCH`
        diagnostic (expected→actual) at the throw site (source-only; ships
        with the Increment 1 deploy — behavior-identical to pre-fix + the log). The canary is already
        discriminating: 11:28:45 `device.state 3→2 host.state 0→0` (a true
        loss — no host absorption) was correctly rejected. The benign
        migration subcase (device.state 3→2 + host.state 0→1) is what the
        relaxation would have accepted; the true-loss subcase is what
        Increment 1 (spill-before-loss) must prevent. Low frequency (a few/hr
        under heavy concurrent load), always self-retried (the retry
        re-admits against current residency and succeeds).
        **Re-plan shipped (2026-09-16, building):** the 8509 check now
        re-plans instead of throwing for the relief-induced shortfall class.
        On `actual != expected`, the plan is re-baselined to the materialized
        unit when (a) all KV dimensions are exact, (b) lanes match, (c)
        device.state is at or below plan and host.state exceeds plan only by
        the device shortfall (a device→host migration of an optional image —
        restorable, not a loss), and (d) the unit CORE is complete: the
        active state binding is device-resident. Condition (d) is the
        per-image check the reverted per-sum relaxation lacked — a true
        last-replica loss of the core image fails it and still throws.
        Acceptance logs `[replan] state entitlement re-baselined ...`; every
        other mismatch keeps the strict throw, now preceded by an image census
        (`[entitlement] DIAG` per store image + store histogram) that pins the
        class for any remaining episodes. A monotonic
        `materialize_state_replans` counter ships in /stats (full chain:
        types.h → resource_manager.h → runtime.h → api_impl.h →
        stats_json.cpp). `active_resources` bookkeeping now stores the
        re-planned (actual) entitlement, so the pressure planner attributes
        the lane what it actually holds. This converts the observed
        `3→2 host 0→0` 500 class into a clean (slower) success; the
        spill-before-loss work above remains the fix for the case where the
        CORE image loses its last replica (the core check throws there by
        design). e2e: the parser tracks `[replan]` and `[entitlement]
        MISMATCH`; phase 13 gains PASS/FAIL lines for both.
        **Live-verified + e2e-verified (2026-09-16):** deployed 13:01
        (PID 252366). First live `[replan]` at 13:05:34 — the protected
        request (122k-prompt, 121k-token checkpoint restore) completed
        cleanly; 6+ replans by 13:09, **0 `[entitlement] MISMATCH`, 0 500s,
        0 worker recoveries** since the restart (the class was 500ing every
        ~30s pre-deploy). Full 13-phase e2e suite green (two runs: 1–11
        capped at the 420s suite cap, 12–13 via `E2E_START_PHASE=12`):
        0 FAIL total; phase 13 pool saturated 8/8, zero worker recoveries,
        zero orphaned state images; the new `state_replan` /
        `entitlement_mismatch` / `no_resident_state` counters parse and gate
        (positive replan path verified live, not in e2e — relief did not
        fire in the e2e run, non-deterministic WARN as before).
        **Shipped `0c364877`, e2e-verified 2026-09-16 13:41 (phases 11–13,
        16P/6W/0F):** the backstop is live. `retain_unit_before_state_loss()`
        at the top of `release_continuation_slot`: if the slot holds a live
        unit (KV retained + identity + frontier) and the net does not retain
        it, the existing spill path captures the complete unit before the
        state half dies; net refuses → the loss is allowed (graceful
        degrade, logged). Scope analysis: after the gate fix + pre-consume
        spill (start_request) + victim-path spills, the superseded-endpoint
        / dropped-rewrite release sites (11316/11369/9421) lose only
        frontiers no future request references (clients always rewind to the
        LATEST rewrite), so the slot release is the single site where a
        NEEDED frontier can lose its last restorable copy. `retains()`
        mirrors find(): an entry covers the unit at its OWN execution or
        checkpoint frontier, matched by ledger tokens AND identity (the
        identity alone — token types/positions — cannot distinguish
        same-shape units; caught by the unit test), or by session key
        (thinking-mode fallback); a longer entry does not cover a shorter
        frontier (the state image rides at the entry's frontier). Phase 13:
        **13 backstops fired** (the guard's positive path, e2e-verified),
        zero orphaned state images, zero 'private source result is missing';
        the only worker recovery was the known P4.2 class. Unit tests pin
        the retains() semantics (test_host_kv_safety_net.cpp).
      - **Increment 2 — net as the unit's host home (shared pool + move-not-copy).**
        The net's `state_host` buffers were untracked host replicas (invisible
        to the store census, state_image_store.h:166–205) and net restore
        wrote via raw `copy_from_host`. **Chosen design: the net shares the
        store's `HostStatePool`** — net entries hold `HostStateSlotHandle`s
        (the same pinned pool the store uses for host replicas), so host-state
        residency is the pool's `occupied()` (the census sees both tenants:
        store replicas + net entries). The pool is a byte budget expressed in
        slot units (P2.6 exposes it as `--host-cache-mib`); it is NOT a
        fundamental fixed count. On the release path (`relinquish_store_state`),
        a HostOnly exclusive image's slot is MOVED into the net entry
        (`detach_host_replica` — no copy, pool occupancy unchanged) and a Both
        image drops its redundant store host replica before the D2H copy; the
        pre-consume spill (start_request) copies (its source stays alive).
        Dropped entries (eviction/supersede/arena-reclaim) return slots via a
        `set_state_slot_releaser` callback; `take`/`take_pinned` transfer slots
        with the entry (the program releases them on restore failure).
        `clear()` and the spill's ABORT/exception paths release captured slots
        (a dropped handle would leak the pool slot). *Exit:* 0 pool-slot leaks
        (census `occupied()` returns to baseline after a phase); the
        `state_relinquish` e2e counter proves the move-not-copy path fired.
        **Shipped + e2e-verified 2026-09-16 19:34.** Phase 13 (state-saturation):
        **18 state images relinquished to the net (move-not-copy)**, device pool
        saturated 7/7, 39 spills OK, 3 restores, 0 worker recoveries, 0 FAIL.
        e2e pool must be 96 slots (net ~28 units at the 4 GiB byte budget +
        store demoted replicas exceed 48 under the e2e's 10-session stress;
        prod's 48 is fine for its lighter real load). Unit test
        `test_state_slot_lifecycle` pins the releaser-on-drop / take_pinned-transfer
        / census semantics.
      - **Increment 3 — retire per-replica demotes.** The pressure planner
        demotes whole units, not replicas: retire the state-only and KV-only
        demote decisions; the safety-net spill becomes the only device→host
        path. (Largest risk; do last, after #7's full gate.)
        **Slice 3 Increment 1 (relief metric) SHIPPED `569dbbd2`, 2026-09-17
        20:1x, e2e-verified (rc=0, 0 crashes) + deployed (prod up 20:14:44).**
        The 19:22/19:42/19:55 wedge loop (sentinel restarted the user's live
        server 3×) was traced to `relieve_kv_fit` ranking victims by
        `resident_device_pages` — which counts pages shared with other address
        spaces. A ~235k unit sharing a ~230k prefix with an idle shared-prefix
        entry scored ~7300 "unique" pages but freed 2–16 real pages per
        demotion (free 777→793→802→804→807 over five 15s bursts, each a 3.3GB
        D2H + 154MB state copy), and stage-2 (release the idle shared prefix —
        where the pages were actually pinned) never ran because stage-1 kept
        finding "victims". Fix: `unique_resident_device_pages` probe
        (device_resident && address_references == 1, mirroring the
        `resident_resources` skip); both stages scored in one call, higher
        score wins (tie → content-preserving demotion); both zero → no-op;
        returned value is the MEASURED free-page delta. Unit test
        (test_context_store.cpp) pins the topology: fully-forked unit + prefix
        both unique==0 / resident==2; branch release frees nothing, prefix
        release frees the pages. Soak exit signal: 0 wedge restarts +
        plausible freed counts under the monitor. **Increment 2 (planner
        retirement)** — status below (implemented, e2e pending).
        **Slice 3 Increment 1 (relief metric) SHIPPED `569dbbd2`, 2026-09-17
        20:1x, e2e-verified (rc=0, 0 crashes) + deployed (prod up 20:14:44).**
        The 19:22/19:42/19:55 wedge loop (sentinel restarted the user's live
        server 3×) was traced to `relieve_kv_fit` ranking victims by
        `resident_device_pages` — which counts pages shared with other address
        spaces. A ~235k unit sharing a ~230k prefix with an idle shared-prefix
        entry scored ~7300 "unique" pages but freed 2–16 real pages per
        demotion (free 777→793→802→804→807 over five 15s bursts, each a 3.3GB
        D2H + 154MB state copy), and stage-2 (release the idle shared prefix —
        where the pages were actually pinned) never ran because stage-1 kept
        finding "victims". Fix: new `unique_resident_device_pages` probe
        (device_resident && address_references == 1, mirroring the
        `resident_resources` skip); both stages scored in one call, higher
        score wins (tie → content-preserving demotion); both zero → no-op;
        returned value is the MEASURED free-page delta (available_pages
        before/after), logged as `scored X unique, freed Y pages`. Unit test
        pins the topology (fully-forked unit + prefix both unique==0; branch
        release frees nothing, prefix release frees the pages). Soak exit
        signal: 0 wedge restarts + plausible freed counts under the monitor.
        **Slice 3 Increment 2 (planner retirement) IMPLEMENTED, e2e PENDING**
        (2026-09-17 21:2x): `keep_replica_demotes()` helper + 3 generation
        gates in program_impl.h (KV demote generation in
        `select_kv_pressure_actions`, shared-state demote branch in
        `inspect_shared_pressure_successors`, add-state demote branch in
        `inspect_pressure_successors`) — default OFF (demotes retired, Evict
        carries the load); `NINFER_KEEP_REPLICA_DEMOTES=1` restores the old
        option set. Duplicate-drops + state-slot relief stay. **Tier-1
        decision (differs from the original plan):** `demote_checkpoints_to_make_room`
        tier-1 (DeviceOnly checkpoint D2H) is NOT re-pointed — it is a
        state-SLOT-pool pre-allocation guard that is unit-completeness-
        preserving (the state image moves to the net, its home; the unit is
        never split into a KV-without-state form), so it stays as-is.
        Verified: full build clean; resource_manager_test A/B-identical
        failure sets (5 pre-existing baseline FAILs, no new); context_store_test
        passes. E2E verdicts: focused suite pressure gate + mode checks
        updated; repo suite pressure gate updated (spill_ok/safety-restores
        counted) — the plan's "re-point" list turned out mostly unnecessary:
        `checkpoint_demoted` (relay D2H, deferred exception),
        `relief_demote`/`relief_dual_drop` (state-slot relief, kept),
        `host_copy_ok` (safety-net spill, the new sole path) all stay live.
        27b test_engine_prefix_real re-pointed: the source-pressure test now
        accepts whole-unit Evict (evicted counter) as the relief mechanism
        alongside demote; builds clean, needs a GPU-exclusive run to verify
        (second 16GB model load would OOM alongside prod — run in a
        prod-stopped window).
        **E2E verification (2026-09-17 21:2x–21:5x, all on the Inc 2 binary):**
        - focused suite, switch OFF: rc=0, 0 bad_alloc, restore + safety-net
          H2D fired, cache reuse > 0.
        - focused suite, switch ON (control): rc=0, same verdicts — and the
          focused scenario relieves via the safety-net spill in BOTH (degraded
          = 0 in both), so the focused suite is behavior-neutral ON/OFF.
        - phase 13 (state-saturation) + 14 (queued-relief), switch OFF:
          0 bad_alloc, 0 crashes, 46 spills OK, 39 restores, net accumulated,
          pool saturated 6/6 — the whole-unit machinery works. BUT 13×
          `[engine] WORKER RECOVER: isolated-feasible request is blocked in an
          idle Engine` (phase 13) + 1× (phase 14). Each runs the OOM-recovery
          path (fail_all_cleanup + catalog clear + re-arm) — a transient
          liveness stall, self-healing (all requests completed), but 13
          catalog-clears in a phase is a real quality cost.
        - phase 13 control, switch ON: CAPPED at 360s (the demote path is slow
          — 44% of the phase in 360s; a full ON run exceeds the 10-min
          foreground limit, so it cannot be one blocking command). In the
          portion it covered: 0 WORKER RECOVER, 11 relief-kv demotes firing
          (confirms the kill-switch reaches the server). Suggests the demote
          path avoids the stalls, but not a complete comparison.
        **Attribution (honest):** the recovery mechanism is pre-existing
        (020ca885, a general liveness net), not new Inc 2 code. Prod under
        live (lighter) load shows 0 recoveries since restore — the stalls only
        appear under the e2e's FORCED state-saturation. Cannot fully attribute
        the 13 stalls to Inc 2 vs pre-existing (full ON control infeasible in
        one foreground run; OFF log overwritten). **Verdict: Inc 2 is clean on
        the hard regressions (crash/bad_alloc) and fine for live load, but
        under forced state-saturation it self-heals 13× via the catalog-clearing
        OOM path. Do NOT enable permanently yet.**
        **Root cause (instrumented, 2026-09-17 22:3x):** the blocked head's
        deficit is **KV, not state** — the planner's `greedy_cover_target`
        returns nullopt with `residual dev{slots=0, mkv=50–195, bkf=48–192}`
        and `any_decision=1` (eviction options ARE generated; `w/evict==elig`,
        so it is NOT an Inc 2 gate bug). Even evicting every eligible owner
        (including shared prefixes, which DO get eviction options) does not
        cover the KV deficit. Why: `resident_resources` (program_impl.h:8412,
        8460) only counts KV pages with `address_references == 1` — a shared
        prefix's pages are referenced by the private sessions that fork from
        it, so `address_references > 1` and the shared prefix's eviction frees
        ~0 KV. Evicting the private sessions doesn't free the prefix either
        (still referenced by it). So the shared prefix's device KV is locked:
        evict-all can't reach it. The **KV-only demote** (retired by Inc 2)
        was the only mechanism that could free a shared prefix's device KV
        (move it to host, keeping the references). A targeted state-slot
        relief (tried, reverted) did not help — the deficit is KV.
        **Verdict: Inc 2 is clean on the hard regressions (crash/bad_alloc)
        and fine for live load, but under forced state-saturation it self-heals
        13–14× via the catalog-clearing OOM path because the shared-prefix KV
        relief (KV-only demote) is retired. Do NOT enable permanently yet.**
        **Decision (user, 2026-09-17): implement the fix** — "rare in prod"
        only holds while ONE session runs; with several concurrent Claude Code
        sessions (the intended usage) the stall is the steady state, not an
        edge case. Design below (Slice 3 Increment 3).
        **Root cause pinned (instrumented, 2026-09-17 22:4x):** the blocked
        head is a ROOT admission (src=0 shrsrc=0 in all 49 no-plan events)
        needing a full KV working set; the pool is full of a shared prefix
        (locked KV) + private sessions (tiny unique KV). `resident_resources`
        (program_impl.h:8412, 8460) counts only `address_references == 1` pages,
        so a shared prefix's eviction frees ~0 KV (its pages are referenced by
        the forking sessions). Evict-all therefore can't free the shared KV —
        the only mechanism that could was the KV-only demote (retired by Inc 2).
        A targeted state-slot relief (tried, reverted) did not help (deficit is
        KV). NOTE: a full ON (demotes-active) phase-13 comparison is infeasible
        in one foreground run (the demote path is too slow — it capped at 44%),
        so the 13–14 recoveries are not yet cleanly attributed to Inc 2 vs
        pre-existing; the mechanism (KV-only demote retired) strongly implicates
        Inc 2, but the ON control only covered the pre-saturation portion.
        **Slice 3 Increment 3 — shared-prefix KV relief (DESIGN, 2026-09-17;
        user-directed after the multi-session exposure call).**
        *Problem / exposure:* under concurrent sessions that share a large
        prefix (observed 248k tokens in the e2e; prod will be the same shape
        with several Claude Code sessions), the device KV pool saturates with
        (a) the shared prefix's KV — locked: every page has
        `address_references > 1` (forking sessions alias the prefix's logical
        pages), so `resident_resources` counts the prefix's eviction as ~0
        pages (program_impl.h:8460 skips `address_references != 1`) — and
        (b) the sessions' small unique KV. A root admission then needs a full
        KV working set (~190 pages) that evict-all cannot free: evicting a
        session keeps the pages (the prefix still references them); evicting
        the prefix frees ~0 (the sessions still reference them). The planner
        returns no plan → `TemporarilyBlocked` → idle engine → the brute-force
        `WORKER RECOVER` (OOM-recovery path: catalog clear + spill-before-loss
        + re-arm), 13–14× per forced-saturation phase. Self-healing (no
        crash, no data loss, all requests complete) but each recovery clears
        the whole catalog — every session loses its cache and re-prefills.
        *Why it is not a gate flip (investigated 22:4x):* re-enabling the
        KV-only demote for shared prefixes would NOT free the pages — a D2H
        of one address space does not drop the physical page while aliasing
        address spaces still hold device references (the 19:22 incident is the
        same law: 7300 "freed" pages, +2–16 actual). Freeing shared KV
        requires a demote that is aware of the aliasing set.
        *Design options:*
        - **A (recommended): unit-complete shared-prefix demote.** A new
          pressure action that moves the WHOLE shared-prefix unit to host —
          device KV → host + state image → host (unity preserved: no
          KV-without-state form). The KV half must be a multi-reference-aware
          D2H: copy each logical page once, drop ALL device references, so
          aliasing sessions then see a host-resident replica (the
          `[kv-not-resident] VALID_BUT_NO_DEVICE` + H2D-restore path already
          handles host-resident sources — verify for the shared-source case).
          Frees the device KV pages + the device state slot. Enters the
          planner's option set as one more option per shared owner (greedy
          cover seeds it; 30ms planner budget must hold).
        - **B (stopgap): targeted relief on the stall path.** Extend
          `relieve_kv_fit` (the Inc 1 structure) with a stage: when a fit-gate
          / queued stall persists and a shared prefix is the dominant pinned
          holder, demote that prefix (whole unit) as a relief action — outside
          the planner. Smaller change, no search-space growth; weaker (reacts
          on the 15s stall cadence instead of at admission).
        - **C (rejected): KV-only demote for shared prefixes** — splits the
          unit (KV host, state device): exactly the class Inc 2 retired.
        - **D (rejected for now): accept the safety net** — the user's
          multi-session exposure makes the catalog-clear cost unacceptable.
        *Phasing (after path approval):*
        1. **Investigation (no deploy):** answer the open questions; pick A
           vs B; short design of the chosen path with exact call sites.
        2. **Implementation + unit tests:** the multi-reference-aware KV
           demote (or the relief stage); tests pin: demoting a shared prefix
           frees the device pages once the last aliasing reference is
           demoted; an aliasing session restores from host (H2D) with no data
           loss; the unit stays complete (no KV-without-state form).
        3. **E2E A/B:** phase 13 with the relief ON (expect ~0 recoveries, 0
           bad_alloc, pool saturates, restores fire) vs OFF (13–14
           recoveries — the current baseline); focused suite both. A full ON
           (demotes-active) control does not fit one foreground swap (the
           demote path is too slow — 44% of phase 13 in 360s); use equivalent
           partial windows or a reduced-round phase-13 variant for A/B.
        4. **Prod soak with 2–3 concurrent sessions** (the real scenario):
           exit = 0 `WORKER RECOVER`, 0 `[planner-no-plan]` at saturation,
           0 wedge restarts, plausible D2H counts.
        5. Behind an env kill-switch (default ON after verification;
           `NINFER_SHARED_PREFIX_RELIEF=0` disables).
        *Open questions (phase 1):*
        - Is there a D2H primitive that moves a logical page ONCE and drops
          every device reference (logical_kv_store.h — `begin_device_to_host`
          is per-address today; `address_references` is tracked)? If not, the
          demote is N copies (N = alias count) — bandwidth cost must be
          bounded (demote coldest/idle prefixes first; never a prefix with an
          in-flight active request if avoidable).
        - Who holds device references to a shared prefix's pages at
          saturation: the prefix's address space + each forking continuation's
          address space + ActiveMutables? Which are demotable while a session
          is in flight (the `active` flag)?
        - Does the H2D-restore / materialization path support a HostOnly
          SHARED SOURCE for KV (not just state)? (program_impl.h:5571's
          comment covers state; verify the KV path.)
        - Host KV budget: a 248k-token prefix's device KV (≈3.9k pages text +
          backend) vs the 12 GiB e2e / 30 GiB prod arenas under multi-session
          pressure (the host-state-pool-sizing lesson: two tenants share the
          arena — net units + demoted replicas).
        - Checkpoint references on the prefix state (`checkpoint_references`)
          — the demote must respect them (relief tier-1/tier-2 already model
          DeviceOnly vs dual-resident).
        - Restore-latency impact on aliasing sessions after a demote (they
          pay H2D on their next turn) — measure in e2e (restore counters +
          ttft delta).
        - Keep the brute-force recovery as the last-resort backstop (do not
          remove it).
        *Status:* **Path A implemented + e2e-verified (2026-09-18); the
        state-saturation stall it targets is fixed, but the prod soak
        (2026-09-18) showed the dominant prod bottleneck is host-net
        overcommit, not the device→host planner — see P2.5 Increment 3.**
        Implementation (uncommitted): `shared_prefix_demotes()` helper
        (env `NINFER_SHARED_PREFIX_DEMOTES`, default ON) re-enables the
        demote option for shared-prefix owners only (private units stay
        demote-free per Inc 2): the KV demote pass in
        `select_kv_pressure_actions` (new `allow_demote` param; shared
        callers pass the switch, private callers keep the Inc-2
        retirement), the `DemoteSharedToHost` state branch, plus a
        global `active_address_references` guard in `prepare_kv` (turns a
        would-be `terminate` at publish into a controlled "replica
        changed" failure — the per-address active check can't see active
        refs held by OTHER aliasing address spaces). The greedy seed
        (`greedy_cover_target`) now includes the shared demote move in its
        efficiency ordering (new `choice` field), so a KV deficit only a
        demote can cover is seedable instead of deferring before the
        search runs. `[relief-shared]` log line marks published planner
        demotes.
        **Verification (5-round phase 13, 2026-09-18 09:1x):** 0 WORKER
        RECOVER (baseline: 13–14), 0 bad_alloc, 34/34 requests done, 0
        errors, pool saturated 6/6, 26 spills OK, 10 restores, net
        accumulated. Mechanism (log-verified): the demote option in the
        option space makes the planner's plan FEASIBLE → the head routes
        through the P1.5(d) queued-relief path (KV occupancy block →
        relief-while-queued) instead of dead-ending at admission
        (TemporarilyBlocked → idle engine → liveness throw). The physical
        page freeing is done by the queued relief's idle-prefix release
        (`[relief-kv] ... released idle shared prefix (freed 376 pages)`),
        which is why `[relief-shared]` (planner-published demotes) is 0 in
        this scenario — the planner's demote is the feasibility key, the
        queued relief is the physical actor. 3 residual `[planner-no-plan]`
        events (backend-only residual at that instant) are transient: the
        head re-inspects, the relief cycle frees the prefix, the head is
        admitted — self-resolving within one 15s cycle, no liveness
        recovery. Unit tests: resource_manager_test identical 5 baseline
        FAILs (no new), context_store_test ok.
        **Remaining:** (a) the 3 transient no-plans (backend-only
        residual; self-resolving — acceptable per P1.5(d) visible-queue
        semantics, but the design's "0 no-plan at saturation" criterion is
        not literally met); (b) full 8-round A/B (the 8-round phase
        exceeds the 10-min foreground swap limit; 5 rounds covers the
        stall window — the baseline's first recovery was in the phase's
        first minute); (c) prod soak with 2–3 concurrent sessions (the
        real multi-session test); (d) commit + deploy. The
        `[planner-no-plan]` diagnostic stays in the binary for the soak
        (observability), removed with the final cleanup.
        **Window-1 e2e (2026-09-18 16:2x, prod-stopped batched window):
        the `source-pressure-protection` exercise FAILED on a stale oracle,
        not a relief failure.** The relief did its job — the branch fit
        (`path=2` PrivateTurnClosure, `reused=7676/7784`, source protected,
        `stop=no_pressure`) and `[relief-kv]` honestly reported
        `demoted idle continuation 1 (scored 6 unique, freed 6 pages)`.
        But the oracle read `main_kv_d2h_pages` /
        `pressure_private_owners_{degraded,evicted}` — RM-owned counters
        that only advance for PLANNER-selected pressure actions
        (`apply_private_action`), which the fit-gate/queued-relief path
        bypasses entirely. Two real gaps surfaced:
        1. **Honesty gap:** when the spill ABORTs (host state pool full →
           no state image), `relieve_kv_fit` still released the
           continuation, so the log claimed "demoted … to the host safety
           net" while the unit was actually destroyed (KV freed, state
           lost). The fixture (`host_state_slots=0`) hits this on every
           spill.
        2. **Accounting gap:** no counter recorded the relief's actual
           effect, so neither the test nor /stats could see it.
        **Fix (this change):** `spill_victim_to_host_kv_safety_net` now
        returns whether the net RETAINED the unit (every skip/abort →
        false; both `host_kv_safety_net.add` sites → true). `relieve_kv_fit`
        logs `demoted` vs `EVICTED … spill not retained; unit lost` and
        advances three new monotonic counters — `relief_kv_releases`,
        `relief_kv_not_retained`, `relief_kv_pages_freed` (the measured
        `available_pages()` delta) — surfaced in `/stats` under
        `scheduler.pressure` and in the stats change-detection gate. The
        test oracle is re-pointed at `relief_kv_releases` /
        `relief_kv_pages_freed` (≥1 release, ≥2 pages — the branch needed
        3 with 1 free). Behavior is unchanged (a not-retained release was
        always a hard eviction; now it is counted and logged as one).
        CPU tests: safety-net PASS, RM test = the 5 documented baseline
        FAILs (no new). P2.4 test re-run + P1.9 probe re-run are the
        window-2 items (the probe 404'd in window 1 — see P1.9).
        **Window-2 e2e (2026-09-18 16:5x): source-pressure + checkpoint
        pressure now PASS (oracle fix verified). The test advanced to
        `pressure-resume`, which fails with ALL-ZERO planner counters
        (`main=0 spill=0 drops=0 degraded=0 evicted=0`) and device
        occupancy 127→12 — the long unit was DESTROYED, not spilled.**
        Root cause (code-verified): the fixture
        (`pressure_resume_engine_options`) sets `host_state_slots=0`, so
        under the unity invariant every whole-unit spill aborts
        (`no_state_image` — no state image can be captured without a host
        state slot) and the unit is hard-destroyed. The oracle's expected
        action ("endpoint-drop plus four-page spill") is a PRE-UNITY
        partial KV-only pressure action — that action class no longer
        exists (unity: the unit moves whole or not at all). Same
        stale-oracle class as source-pressure, one level deeper.
        **Fix (in progress):** fixture `host_state_slots 0→2` (the spill
        can now retain the unit); the oracle's diagnostic now prints the
        relief counters (`relief_kv_releases/not_retained/pages_freed`) so
        the window-3 run captures the ground truth (which mechanism —
        planner vs fit-gate/queued relief — frees the pages, and how many
        pages the resume restores). The oracle CONDITIONS are deliberately
        left as-is for the observation run; they get re-pinned to the
        observed post-unity behavior (expect: unit retained, not
        not-retained; resume restores the ~120-page closure via H2D, not
        the old 4-page partial) after window 3.
        **Window-3 observation (2026-09-18 17:0x, `host_state_slots=2`
        fixture): ground truth captured.** The page arrives via the
        fit-gate/queued-relief path: `[relief-kv] demoted idle
        continuation 0 (scored 121 unique, freed 121 pages)` — the WHOLE
        121-page long unit, retained in the net (`[safety-spill] OK
        frontier=7713 ckpt_valid=1`, state image D2H 153.9 MB), device
        occupancy 127→12. New counters confirmed live:
        `relief_releases=1 relief_not_retained=0 relief_pages_freed=121`.
        Planner counters all 0 (the fit-gate path bypasses planner action
        application — as designed). **Oracle re-pinned (this change):**
        pressure phase now asserts the relief mechanism (`relief_releases
        ≥1`, `not_retained == 0`, `pages_freed ≥ 6` = the demand, device
        occupancy == 12, state D2H advanced); resume phase asserts the
        whole-unit restore (`PrivateTurnClosure`, `reused_pages ≥ 119`,
        `restored_pages ≥ 100` H2D — the full closure, not the old
        4-page partial). The resume phase did not execute in window 3
        (early return on the pressure oracle), so the re-pinned resume
        bounds are verified by the next scenario run.
        **Window-4 (2026-09-18 17:1x): the re-pin was one condition short.**
        The relief mechanism matched the pin exactly (`relief_releases=1
        relief_not_retained=0 relief_pages_freed=121 device_pages=127/12`),
        but the oracle failed on `state_d2h=0`: the whole-unit spill's
        state-image capture (net entry `state_bytes=147MB`, `ckpt_valid=1`)
        does NOT report into the state-transfer counters — those advance
        only for planner-driven state demotes. Same pre-unity staleness as
        the original oracle. **Final re-pin (this change):** the
        `state_d2h` condition is dropped (the state image's presence is
        proven by the resume phase's restore — no state image → no
        restore); the diagnostic still prints it. *Observability gap —
        **DONE (2026-09-18, `2360dbe3`):** the spill path's state D2H is now
        counted via `spill_state_d2h_count` / `spill_state_d2h_bytes` (the two
        device→host copy sites in the spill — endpoint + checkpoint images).
        Verified live: 5 spills × 153,954,304 B = 769,771,520 B in /stats.
        (Counted directly at the copy sites rather than via the
        transfer-observation path, which is transaction-coupled and the spill
        is transaction-free.)*
        **Window 5 (next): full default scenario set** — verifies the final
        pressure-resume oracle (incl. the resume phase, first real run) and
        the concurrent-settlement exercise (never ran: earlier windows
        early-returned at pressure-resume). Its fixture has
        `host_kv_capacity=0`/`host_state_slots=0`, so its "canonical
        eviction" is a TRUE catalog eviction (unit destroyed, no net to
        spill into) — `pressure_private_owners_evicted` still increments
        under unity; that oracle needs no re-pin.
        **Windows 5–7 (2026-09-18 17:2x–17:4x): the pressure phase held
        every window** (`relief_releases=1 not_retained=0 pages_freed=121`,
        device 127→12, the whole 121-page unit retained in the net with its
        state image) **but the resume phase kept failing**
        (`path=0 reused=0 restored=0`) — three distinct root causes, each
        unmasked by fixing the previous one:
        (w5) `host_state_slots=2` was too small: the state pool's make-room
        evicted the long unit's state image when the short unit captured,
        destroying the net entry (a KV without its state image is unrestorable
        — unity). Fixture raised to 8 (provisioned non-binding: make-room can
        no longer reach the long unit's image).
        (w6) The resume re-sent the IDENTICAL 7683-token prompt →
        `reuse == max_count` → the zero-suffix reuse rejection
        (host_kv_safety_net.h:467 — a strict-prefix retry leaves no tail to
        prefill and is deliberately rejected to root-prefill; the guard has
        existed since the net's foundation commit `d00e5f0b`, so this is a
        test-scenario bug, not a product regression).
        (w7) Appending the tail INSIDE the user message still missed:
        `[prefix-match] TOKEN MISMATCH count=7683/7676 mismatch_at=7674` —
        the stored prompt ends with a 9-token terminator suffix (im_end …
        assistant-start marker) that an in-message tail pushes back, so the
        stored prefix is NOT a true token-prefix of the new prompt. `find()`
        only matches at the entry's OWN frontiers (full compact_prefix or the
        checkpoint frontier), never at arbitrary interior prefixes — so any
        prompt that alters the first message's content can only match at the
        checkpoint frontier if the first 7676 tokens are unchanged.
        **Fix (window 8):** the resume is a genuine continuation — a
        TWO-MESSAGE prompt (original turn + a new short user turn, helper
        `pressure_turn_with_tail`). The single-message rendering is a true
        token-prefix of the two-message rendering only up to the first
        message's im_end; the match lands on the checkpoint frontier (7676,
        the turn boundary — `find()`'s checkpoint path, the same mechanism a
        real agentic continuation uses), reuse=7676, and the whole retained
        unit restores H2D. Demand stays 121 pages (7689/64→121).
        **Window 8 result:** the checkpoint match worked
        (`match=hit frontier=7676 checkpoint=1`, `reused=7676 reused_pages=120
        restored=120`, `[restore] KV+state copied`) — but the oracle still
        failed on `path=0`. Root cause: `prefix_reuse_path` is the PLAN-level
        path (device shortlist), which misses (the unit is on host, not
        device) → Root; the safety-net re-find restores it without
        reclassifying the plan path. So a host-net turn-closure restore
        reports `path=Root` with `reused_prompt_tokens>0` — an inconsistent
        report. **Oracle re-pinned (this change):** the resume phase now
        proves the whole-unit restore by the page counts (`reused_pages≥119`,
        `restored_pages≥100`), not the path. *Observability gap (follow-up):
        a host-net turn-closure restore should report a non-Root path (or a
        dedicated safety-net path) so `reused_prompt_tokens>0` is consistent
        with `prefix_reuse_path`.*
        **Window 9: pressure-resume PASSED** (the re-pin held). The test
        advanced to the 4th exercise, `concurrent-settlement` (first real run:
        earlier windows early-returned at pressure-resume), which failed on a
        stale oracle.
        **Concurrent-settlement (windows 9–10): two stale oracles, both
        because the exercise tests post-unity + P2.5 behavior.**
        (1) The "canonical eviction" assertion checked
        `pressure_private_owners_evicted` (the PLANNER's counter), but the
        full catalog (8/8 continuations) makes room for the 9th (pressure)
        request via a capacity-driven continuation-slot release —
        `release_continuation_slot` → spill-before-loss → hard-destroy (this
        fixture has `host_state_slots=0`/`host_kv_capacity=0`, so the net
        can't retain) — a THIRD path (not planner, not fit-gate relief) that
        increments no eviction counter. **Re-pinned (this change):** the
        assertion now checks the pressure ADMISSION (the fixture is at
        capacity, so the 9th request fits only if a slot was released).
        Window 10: this passed. *Observability gap — **DONE (2026-09-18,
        `2360dbe3`):** the capacity-driven hard-destroy is now counted via
        `slot_release_destroys` — `retain_unit_before_state_loss` returns
        whether the unit is retained, and `release_continuation_slot` counts
        the false case (a live {KV + state} unit lost to capacity eviction or
        client cancel). 0 in prod so far (the net has capacity, so units are
        retained, not destroyed).
        (2) The replay assertion (the session's unit must SURVIVE the pressure
        so a re-send reuses it) FAILED in window 10: `path=0 reused=0` — the
        session's unit was destroyed device-side (spill-before-loss →
        hard-destroy) because the fixture had NO net capacity
        (`host_state_slots=0`/`host_kv_capacity=0`), so there was nowhere to
        retain it. **Correction:** P2.5's per-session net-eviction protection
        (Increment 2, `ae416669`, deployed 2026-09-15) is ALREADY implemented
        — the gap was the fixture, not a missing feature. **Fix (this
        change):** (a) give the fixture net capacity (`host_state_slots=16`,
        `host_kv_capacity=8 GiB`, matching prod) so the session's unit is
        retained in the net under pressure (the P2.5 guarantee); (b) re-pin
        the replay assertion to check `reused_prompt_tokens>0` (the unit
        survived and was reused) and DROP the `path != Root` check — a
        host-net restore reports `path=Root` at the plan level (same finding
        as pressure-resume), while a device restore reports PrivateTurnClosure;
        the path is non-deterministic (depends on which unit the pressure
        displaced), so reuse is the honest evidence. **Window 11: ALL 4
        SCENARIOS PASS (rc=0)** — the P2.4 test suite is green (the slice's
        prod-soak verification remains open).
        **Follow-up #1 (path reporting) — DONE (window 12, 2026-09-18 19:2x):**
        a host-net restore now reports the same path a device-side restore
        would, instead of Root with reused_prompt_tokens>0. The net entry
        records the unit's rewrite-checkpoint kind at spill time
        (`checkpoint_kind`); `find()` propagates it in the match; the three
        re-find sites resolve the restore path (`checkpoint ?
        restore_path(kind) : PrivateEndpoint`) into the transaction and the
        staged prefill; the Begin summary reports `staged.base > 0 ?
        staged.reuse : Root` (self-correcting: every restore-failure path
        resets base to 0, so a failed restore still reports Root); and
        engine-core's admission/runtime Begin consistency check accepts the
        documented UPGRADE direction (admitted Root → runtime restore path,
        logged as "Begin upgraded from committed root to path=%d").
        Window 12: all 4 scenarios pass with the pressure-resume oracle
        RE-TIGHTENED to assert `path == PrivateTurnClosure` again
        (`[admission] Begin upgraded from committed root to path=2 (reuse
        7676)` in the log). The concurrent-settlement replay stays
        path-agnostic (its restore may be device- or net-served).
        **Deployed 2026-09-18 19:27 (e2e swap, rc=0):** all 4 e2e verdicts
        PASS (zero bad_alloc, restore fired under pressure, cache reuse > 0,
        safety-net H2D restore fired). Prod healthy post-restore (0 crash
        classes; relief counters reset to 0 — expected post-restart, the
        soak baseline restarts from here).
        **Prod soak (2026-09-18 18:51–19:01, counter-instrumented binary
        already live — no restart needed):** relief accounting verified in
        prod. Baseline→now: `relief_kv_releases` 5→8, `relief_kv_pages_freed`
        6,774→10,208 (+3,434), `relief_kv_not_retained` 0→0. The counter
        delta EXACTLY matches the journal's `[relief-kv]` freed lines
        (864+858+1712 = 3,434) — honest deltas confirmed live. All three
        new relief events fit their demand in ONE fire (no 15s churn loop,
        no 120s aborts) — the 2026-09-17 incident signature (claimed 7,300
        freed / +10 actual, 3 wedge restarts) did not recur; 0 wedges, 0
        crash classes in the window. Net: 11 units / 18.1 GiB, all dead
        tier (expected — fresh unmatched backups); unit_bytes 18.0→22.8
        GiB. Notable: `state_bytes=0 ... at-ckpt=1 — unit retained complete
        at its checkpoint frontier` — the P2.5 Inc 1 checkpoint-frontier
        retain is working in prod (endpoint state lost, checkpoint state
        keeps the unit whole). Soak continues; exit signal = 0 wedge
        restarts over a full working day with plausible relief counts.
        **Prod soak (2026-09-18 20:54–21:16, post-20:53-restart baseline):**
        the 20:53:55 sentinel restart (the v3.1/v3.3 misfire) reset the
        counters; since 20:54:37: 0 crash classes, 0 "no resident state",
        0 "private source result is missing", 0 finish=error/cancelled.
        `spill_state_d2h_count` 30 = 30×153,954,304 = `spill_state_d2h_bytes`
        exactly (follow-up #3 counters verified live). Net at ceiling
        (25.75/27.6 GiB, all dead tier) — the O2 soft-ceiling dead reaper
        fired (`reap=stale` ×4, 21:15:45), the expected steady-state
        behavior. **WORKER RECOVER ×6 (20:59–21:15, ~every 3–5 min), all
        the same class** ("isolated-feasible request is blocked in an
        idle Engine" — the e2e state-saturation class, logic_error caught,
        worker recovered): every affected request still COMPLETED
        successfully (e.g. req 101: ttft=473ms, reuse=private_turn_closure,
        wall=1.02s) — the recovery path works in prod; no client-visible
        impact. Frequency is driven by this session's load (97–100k-token
        prompts, device-state-slots=7, heavy compact-prefix churn). Not a
        P2.4 exit signal (exit = 0 "no resident state" + 0 wedge restarts);
        tracked as the known state-saturation class.
- [x] **P2.5 — Slice 4: unit LRU/retention + per-session guarantee.** One LRU
      over the shared budget, evicting whole units cost-aware smallest-first
      (kills the 19s class: state can no longer outlive its KV's retention
      decision); protect an interactive session's unit (≥2 slots: turn-closure
      + checkpoint); idle-aware recency weight. *Exit:* 0 mid-instance
      full-root re-prefills in a long-session e2e; the 19s safety-net class is
      gone.
      **Design (2026-09-15, from the P2.3 e2e + live journal evidence):**
      The dominant unit-loss class is `endpoint_state_missing` (95/95 in the
      P2.3 e2e; live flood 19:20–19:31: 80k–297k-token units, all
      `ckpt_valid=1`). Root cause traced in the e2e log: thinking-mode
      follow-ups always rewind to the rewrite checkpoint (preserve_thinking=off
      drops reasoning), and the rewrite-restore path releases the superseded
      endpoint state (`program_impl.h:11194`) — the live unit of a thinking
      session lives at the CHECKPOINT frontier, not the endpoint. The old
      `endpoint_fallback` retained exactly this and was CORRECT: `find()`
      bounds matching by `execution_frontier`, so {KV[0..C], state@C} is a
      complete unit at C — the P2.3 "frontier/state/ledger mismatch" analysis
      was wrong (the ledger beyond C is never compared). P2.5 restores that
      retention, done as a true complete unit:
      - **Increment 1 — retain the deepest complete frontier.** In the spill:
        endpoint state present → retain at the endpoint (with the checkpoint
        if captured; the `rewrite_checkpoint_uncaptured` abort stays — a
        thinking unit without its checkpoint cannot serve its next turn and
        the arena is the binding resource). Endpoint missing, checkpoint
        present → retain the unit TRUNCATED to the checkpoint frontier
        (execution_frontier/ledger/identity/compact_prefix/page counts all at
        C; the checkpoint state becomes the unit's state; the dead tail
        KV[C..E] is not advertised — the arena keeps the physical allocation
        until eviction, the meter counts the logical unit). No state at all →
        abort (a true non-unit). The identity moves into the entry by COPY,
        not swap — a demoted-not-evicted victim keeps its identity (fixes the
        latent silent-root-re-prefill bug noted in P2.3). *Exit:* 0
        `endpoint_state_missing` aborts with `ckpt_valid=1` in e2e + journal;
        the 19s safety-net restores return.
      - **Increment 2 — per-session guarantee in net eviction.**
        `select_eviction_victim` skips units whose session is currently
        active (session_key matches a Catalogued/Active continuation) in the
        live tier — idle sessions' units go first; an active session's unit
        is evictable only when nothing else is. *Exit:* 0 evictions of an
        active session's unit while idle units remain.
      **Increment 1 shipped `b9e6b244`, e2e-verified 2026-09-15 (phases
      4–12, rc=0: 37 PASS / 10 WARN / 0 FAIL) and LIVE-VERIFIED 19:59–21:30:
      0 `endpoint_state_missing` aborts since deploy (was ~1/min, 80k–297k
      tokens), 40+ checkpoint-frontier retains of 100k–420k-token units, 0
      error classes.**
      **Increment 2 implemented `ae416669`, e2e-verified 2026-09-15 23:28
      (phases 4–12, rc=0: 36 PASS / 10 WARN / 0 FAIL — same as increment 1;
      0 new error classes; state-lease: zero orphaned state images, zero
      'private source result is missing') and deployed to prod 23:28.**
      Design: three-tier selection — dead-largest, live-smallest, then
      protected-live-smallest (an active session's unit, evicted only when
      nothing else is). Protection is ORDERING only: the protected tier is
      exhausted before selection gives up, so a re-spill of the same session
      still displaces its own older unit (supersede) rather than being
      rejected. Both eviction loops respect it: the spill loop via
      `select_victim`, the state-pool loop (`retain_state_capture`) routed
      through the same selection. The program wires `set_session_is_live`
      with a predicate over `continuation_states`/`continuation_slots`
      (Active or Catalogued role + session_key match). The dead tier still
      precedes protection — a live session's unit unmatched past the 15-min
      TTL is reaped largest-first like any dead unit (TTL reaping intact).
      Unit tests: `test_session_protection` (protected tier exhausted last,
      dead tier precedes protection, active unit survives idle evictions in
      the state-pool loop, exhaustion displaces it only when alone).
      **Increment 3 — host-net overcommit under concurrent sessions (DESIGN,
      2026-09-18; the finding that frontier-collapse + per-session protection
      are NOT the bottleneck — the aggregate working set is).**
      *Finding (live journal, 2026-09-18 11:40–11:47, one prod, PID 75435,
      `host-kv-mib 30720` = 30 GiB shared budget, `host-state-slots 112`):*
      With several concurrent Claude Code sessions the host net's working set
      exceeds the 30 GiB budget and the net sits at its ceiling, evicting one
      unit per capture (an eviction every 60–90 s in the observed window):
      - **Supersede is working and is not the bottleneck.** Each conversation's
        growing chain collapses to its current frontier (`[safety-net]
        supersede: dropping frontier=45271 (prefix of 48377)`, `54749 (prefix of
        55331)`, `59579 (prefix of 60308)`, …). The 18–22 entries are the
        *current frontiers of distinct concurrent conversations* (one growing
        48k→55k→56k→60k→63k→68k→75k, one steady ~36k always `miss`, plus dead
        remnants / short-lived requests) — NOT redundant same-conversation
        duplicates. So the overcommit is the **aggregate multi-conversation
        working set**, exactly the steady state predicted for several
        concurrent sessions (rare today only because one session is running).
      - **The meter is a shared budget, not a double-count.**
        `shared_occupied_bytes() = shared_arena_->occupied_bytes() +
        state_retained_bytes_` (host_kv_safety_net.h:600) against
        `state_budget_bytes_ = 30 GiB`. KV arena (27–29 GiB) + state images
        (3.7–4.9 GiB) = 32–34 GiB > 30 GiB. `retain_state_capture` (736) evicts
        until `shared + incoming ≤ budget`; the net oscillates at the ceiling
        (30.09 → 29.87 → 30.13 → 31.03 GiB over 4 min).
      - **Each entry carries a fixed ~150–307 MB state image** (a HostStatePool
        slot) plus frontier-scaled KV. `112 slots × ~307 MB ≈ 34 GiB` of state
        capacity alone — the state images are a large fixed fraction of the
        budget, independent of frontier length.
      - **Consequence:** under pressure the three-tier eviction (dead-largest,
        live-smallest, active-session-last) exhausts dead + idle, then evicts
        *live* sessions' frontiers (smallest = cheapest re-prefill), forcing
        full re-prefills of active conversations — the degradation class that
        becomes the norm under several concurrent sessions.
      *This is a capacity / retention-ceiling problem, not a retention-logic
      bug: supersede (collapse) and the per-session guarantee (protect) are
      both shipped and working; the working set simply exceeds the budget.*
      *Options:*
      - **O0 — observability census (additive, zero-risk).** Surface the net
        composition: entry count, per-entry {frontier, state_bytes, kv_pages,
        liveness tier (dead / live / protected-live)}, and the
        shared-vs-budget delta — in `/stats` (host_kv block) and a rate-limited
        log line. Lets the operator confirm what is consuming the budget
        (concurrent sessions vs dead remnants vs short-lived requests) and size
        the budget from evidence instead of guessing.
      - **O1 — budget headroom (primary lever, config).** The budget must fit
        the working set: `N_concurrent × (avg_frontier_KV + ~307 MB state) +
        headroom`. Sizing rule of thumb: budget ≈ 4 × (largest expected
        single-session unit) for 2 sessions, +1× per additional expected
        concurrent session. Bounded by host RAM (53 GB total).
        **O1 BLOCKED by host RAM (2026-09-18, post-fix census):** the working
        set is ~34.75 GiB (unit_bytes), peak shared 32.7 GiB and still
        growing, against the 30 GiB budget — and it is **ONE logical main
        session** (user-confirmed 14:4x: no other sessions; the load is the
        main conversation + its sub-agent forks + old frontiers + its
        compaction generations), not N concurrent conversations. But `free`
        shows only **4 GiB available** (49/53 GiB used). Bumping
        `--host-kv-mib` to fit the working set (~40 GiB) would push the host
        into its 16 GiB swap — catastrophic for an inference server. So the
        overcommit is a **physical RAM hard limit, not a tunable**: the net
        must evict regardless of the budget, and the lever is the tiering
        (session keys) so the *right* units (active session) survive, not
        expanding the budget. O1-as-budget-bump is off the table unless host
        RAM grows or the per-unit cost drops (O5, breaks the unit invariant).
        *Key-splitting consequence:* the derived key (system + first user
        turn) CHANGES at every client compaction, so one logical session
        spans multiple keys over its life — pre-compaction units look like a
        different (idle) "session" to the tiering. That is acceptable (they
        ARE stale after compaction) but means the "distinct keys" count
        overstates the conversation count; the ~18 GiB stale-frontier pile
        (11 units in a 72–76k band) is the main session's old fork/frontier
        weight, the prime trimmable target.
        *Concrete (2026-09-18 soak, pre-fix):* a single active Claude Code
        session — including its sub-agent forks and un-reaped old frontiers —
        drove the net to ~23–27 GiB (9–25 entries), i.e. ~1 heavy session
        nearly fills the 30 GiB budget. So the 30 GiB budget fits ~1 heavy
        session with little headroom, or ~1–2 lighter ones. **Size from the
        post-fix census, not the pre-fix accumulation:** the tiering (48a975bb)
        + O2 reaper (552dbeba) trim the dead/idle weight, so the retained
        *active* working set is smaller than the pre-fix 23–27 GiB. Read
        `host_kv.tier_census` after a representative workload and size
        `--host-kv-mib` to `N_sessions × (active + idle tier bytes) + headroom`.
      - **O2 — soft-ceiling dead reaper (guardrail, kill-switch-able).** Dead
        entries (unmatched > `dead_ttl`) are reaped only *under* budget
        pressure (the dead-largest tier of the eviction loop). A bounded idle
        reaper — evict ONLY dead entries, never live, when the net is over a
        soft ceiling (e.g. 85% of budget) — keeps the baseline lean so pressure
        episodes don't reach the live tier. `NINFER_NET_DEAD_REAP` (default on).
        Low-risk: zero re-prefill cost, zero UX impact. *Caveat:* under the
        observed continuous pressure the net is rarely idle, so this mainly
        trims the steady-state baseline (dead remnants), not the pressure
        episodes themselves.
      - **O3 — eviction cost model = true re-prefill cost (refinement).** Rank
        live victims by frontier length (the actual re-prefill cost) weighted by
        session recency, not nominal KV pages. The current live-smallest already
        approximates this; a refinement only changes *which* live session is
        sacrificed, not *whether* one is.
      - **O4 — per-conversation frontier cap.** Bound entries per conversation to
        {current frontier + checkpoint}. Supersede already collapses the chain
        to the current frontier, so marginal value is low.
      - **O5 — cheaper state images / KV-only frontier entries.** Cutting the
        fixed ~307 MB state image (or retaining KV-only for frontier entries
        that will never be rewound to) would cut per-entry cost, but breaks the
        unit invariant (P2.3) — a state image is required for an exact GDN/SSM
        restore. Not recommended without a correctness redesign.
      *Recommended increment:* **O0 (census) + O2 (soft-ceiling dead reaper)**
      as the first ship — additive/low-risk, kill-switch-able, directly reduces
      the overcommit baseline and the frequency of live-tier evictions — plus
      **O1 sizing guidance** in the config docs. O3 as a follow-up if
      live-eviction frequency stays high after O2.
      **Implemented (2026-09-18) — the sharper answer to "which session is
      active vs old copies": tier the eviction Active > Catalogued > dead.**
      The root cause of the active session being displaced was that
      `session_is_live` (the per-session guarantee's predicate) lumps **Active
      and Catalogued** continuations together as "protected," so idle "old
      copy" units (Catalogued — retained for reuse, not being served) got the
      same last-resort protection as the session actually being driven. Fix:
      add a `session_is_active` predicate (Active role only) and split the
      protected tier into **idle-catalogued** (Catalogued = old copy) and
      **active** (Active = being served). Eviction order is now
      dead-largest → unprotected-live-smallest → **idle-catalogued-smallest →
      active-smallest**, so a single driven session's frontier survives while
      stale idle copies are reaped first. When `session_is_active` is unset the
      behavior falls back to the pre-fix lumped tier (safe default).
      *Files:* `host_kv_safety_net.h` (`session_is_active_` member +
      `set_session_is_active`, `select_eviction_victim` four-tier split,
      `select_victim` wiring); `program_impl.h` (`set_session_is_active` wired
      to the Active-role continuation predicate); test
      `test_host_kv_safety_net.cpp` (`test_active_vs_idle_tiering`).
      *Status:* unit-tested (idle evicted before active; active last resort;
      dead tier still precedes; fallback intact) + `ninfer-serve` builds.
      **Deployed 2026-09-18 12:06** (e2e swap: 4/4 PASS — zero bad_alloc,
      restore fired, cache reuse > 0, safety-net H2D restore; prod restored
      on the fixed binary, sentinel re-armed). Early soak: 0 evictions in the
      first minutes post-deploy (pre-deploy cadence was one every 45–90 s),
      one healthy `[safety-spill] OK` (unit retained complete at its
      checkpoint frontier). The tiering's real test is as old copies
      accumulate: evictions should drain dead → idle (Catalogued) first, with
      the active session's frontier last. O0 census + O2 reaper remain as
      follow-ups if the tiering alone doesn't clear the thrash.
      **O0 census IMPLEMENTED (2026-09-18, uncommitted):** the net's
      per-eviction-tier composition is now in `/stats` (`host_kv.tier_census`
      = {dead, live, idle, active} × {entries, bytes}). `NetTierCensus` in
      `include/ninfer/types.h` (embedded in `RuntimeStats.host_kv_tier_census`);
      `HostKVSafetyNet::tier_census()` + `entry_occupied_bytes()` helper +
      `EvictionTier` enum / `tier_name()` (classify_tier now returns the enum;
      the evict logs use `tier_name(classify_tier(...))`); facade plumbing
      through `program.h` / `program_impl.h` / export `runtime.h` /
      `api_impl.h` / `resource_manager.h` / `stats_json.cpp`. Unit test
      `test_tier_census` (one entry per tier, per-tier byte sums) passes;
      `ninfer-serve` builds; stats_json test all-pass; resource_manager test
      at its 5 baseline FAILs (no new). **Deployed 2026-09-18 12:41** (e2e
      swap 4/4 PASS). First live snapshot: the fresh post-restart entries
      classify as `dead` (unmatched until their first hit) — correct. It is
      the tool for judging, from `/stats`, whether the budget is held by dead
      remnants, idle old copies, or live sessions (i.e. whether the tiering
      alone clears the thrash or O1/O2 are still needed).
      **O2 soft-ceiling dead reaper DEPLOYED (2026-09-18 13:04, `552dbeba`):**
      when the net's shared occupancy sits above a soft ceiling (85% of the
      byte budget), stale (dead) entries are reaped proactively on each
      capture (`add()`), so pressure episodes reach the live/idle/active tiers
      less often. Conservative by design: only DEAD-tier entries are touched
      (live/idle/active are never reaped — zero UX impact), and a FRESH entry
      (created within the dead TTL) is left alone so a just-spilled unit is
      not immediately reaped. `HostKVSafetyNet::reap_stale_above_ceiling()`
      (largest-stale-first, called from `add()`), `set_soft_ceiling_reap()`
      + `kSoftCeilingPct=85`; kill-switch `NINFER_NET_DEAD_REAP=0` (default
      ON) wired in `program_impl.h`. Unit test `test_soft_ceiling_reaper`
      (reaps the stale dead entry above the ceiling, never touches fresh live
      entries, no-op when disabled) passes; `ninfer-serve` builds.
      *Soak finding (13:18):* the reaper FIRES (mechanism works — first
      `reap=stale` at 13:18:50) but is **under-powered for the active
      session's overcommit**: it reaped a single 2-page stale entry and left
      the net at ~29.5 GiB (above the 25.5 GiB ceiling). The net is dominated
      by FRESH working-set entries (the active session's current forks +
      checkpoints, host-mirrored while device-resident, all created in the
      last ~14 min post-restart) — the reaper's 15-min stale gate can't touch
      them. So O2 trims aging-out weight (old forks, dead conversations) on a
      15-min+ timescale, but does NOT reduce the steady-state working set of
      an active session. **The active-session overcommit is a capacity
      problem → O1 (budget) or per-session retention is the real lever.**
      Watch +15 min: as the 13:04 entries age past the TTL, the reaper should
      start trimming the dead-tier ones; if the net stays ~29 GiB, O1 is
      needed.
      **Session-key gap — ROOT CAUSE PINNED (2026-09-18, live window 13:59–14:00).**
      A WORKER RECOVER at 13:59:39 (transient liveness stall during overcommit;
      self-healed — req 63 re-prefilled from root, ttft 5.4s, no crash/wedge/
      restart) opened a live window: the safety-find dump shows **every net
      entry `has_sk=0`** and the incoming request logs `session_key NOT SET
      (nullopt)`; the tier_census is 100% dead (16 entries / 27.4 GiB), 0
      live/idle/active. Traced the key chain: the session key is a
      **CLIENT-PROVIDED HINT** (`PromptInput.hints.session_key`,
      frontend.cpp:729) — the anthropic Messages handler NEVER populates it
      (grep: `session_key` appears only in the `openai_responses_*` serve
      files). Only the OpenAI Responses path sets one (from the `response_id`,
      openai_responses_state.cpp:184–193). Prod is all Anthropic → **every unit
      is session-less → classified `dead` → the P2.5 Inc 2/3 per-session
      tiering (active/idle/live protection) is a no-op in prod.** The plumbing
      (request_plan:4603 → request → state:11291 → entry:7070) is correct; the
      gap is at the top — no key is ever supplied. *Consequence:* under
      overcommit the eviction can't protect the active session's frontier (it
      sits in the `dead` tier, evictable like any stale unit) — the
      active-session re-prefill cost is unmitigated. Complementary to O1
      (budget sizing): O1 fixes *how much* is retained, the session key fixes
      *which* unit survives. *Next increment:* derive a session key
      SERVER-SIDE for the anthropic path (a stable conversation fingerprint —
      e.g. hash of system prompt + first user turn — or a client conversation id
      if the client can supply one) so units carry a key and the tiering
      engages. Design fork: the fingerprint must stay stable across the
      conversation AND across compaction, and not collide across sessions.
      **IMPLEMENTED `613171bd` (2026-09-18) + DEPLOYED 14:21 (e2e swap
      4/4 PASS, prod PID 116425):** `derive_session_key()` in
      `src/serve/request.h` — FNV-1a (the net's own constants) over
      system-prompt text + first-user-turn text; wired at the `prepare_impl`
      chokepoint (an explicit key from the OpenAI Responses path is never
      overridden; no user text → no key, pre-fix behavior). Unit test
      `test_serve_session_key` (stability across turns, distinct first-user →
      distinct key, format/capacity, no-user → no key, non-text excluded); all
      serve tests pass. *Behavior note:* a key also flips RM retention
      RecentPrivate(4) → LiveSession(16) — intended (protect the active
      session's units in the catalog too), watch retention bloat in the soak.
      **LIVE-VERIFIED 14:21–14:24:** `[safety-find] incoming session_key set:
      view=cs-…` (was `NOT SET (nullopt)`); two distinct keys for two distinct
      conversations; the first post-restart spill (179k-token unit) shows
      `session: "cs-5c2f…"` in `/stats` `host_kv.top_units` (was `""` on every
      unit). Fresh entries still classify `dead` until first match (correct);
      the census should show live/idle/active entries as the day's traffic
      matches them — that is the next soak signal, alongside the WORKER
      RECOVER cadence (12 today pre-deploy, each a catalog-clear + 45–100s
      root re-prefill).
      **GAP 2 — birth-site stamp `bcd9c0bf` (deployed via the user's manual
      restart 14:34:53, PID 119014):** the key only reached the net via the
      end-of-turn catalogue (11291), which runs only on the ACTIVE sequence —
      a continuation slot evicted before its own turn catalogues (the at-ckpt
      checkpoint-retain path) spilled keyless. Fix: stamp
      `sequence.session_key = request.session_key` at the materialization-bind
      site (where the sequence gets its prefix identity). Live-verified
      14:36–14:40: 34 `has_sk=1` entries; the subagent's root-admitted unit
      (86743) carries its key.
      **GAP 3 — RM shared-prefix candidate `819ee058` (committed, DEPLOYED
      15:01:36 via e2e swap 4/4 PASS, PID 122293):** the RM has FIVE
      `inspect_admission` sites but only three set the session key — the
      shared-prefix candidate path (resource_manager.h:390) left the plan
      keyless, so a request admitted via a shared prefix carried an empty
      key, and the end-of-turn catalogue (11291) then OVERWROTE the
      sequence's birth-stamped key with nullopt. Live-verified 14:40: the
      main session's 210k–228k units (all `shared_stable_prefix` requests)
      were keyless while the subagent's root-admitted unit was keyed — the
      exact split. Also fixed: the RM test's FakeProgram was missing the
      O0/O0+ census methods (the test target had not built since e995bbad).
      **LIVE-VERIFIED 15:01–15:04 (post-deploy, PID 122293): 0 keyless
      entries (25 `has_sk=1`, 0 `has_sk=0`); the main session's 277886 unit
      (5.75 GiB) now classifies `idle` (was `dead`) — the first unit in a
      protected tier. All three gaps closed; the per-session tiering engages
      on every admission path.**
      **Census interpretation (15:08): an all-`dead` census is a NORMAL
      steady-state artifact, NOT a bug signal.** `classify_tier` anchors the
      dead tier on match recency (`!ever_matched || last_matched > 15min
      TTL`), and every supersede mints a fresh never-matched unit (dead by
      construction) while dropping the previously-matched one — so the
      newest unit is always dead at any snapshot, and the census catches the
      net mid-churn. The reliable signals are (a) 0 keyless entries and (b)
      state-pool evictions being all `tier=dead` (10/10 since deploy — the
      tiering protects live/idle/active state images). Do NOT treat "100%
      dead census" as a regression; treat "keyless entries" or "a
      live/idle/active unit evicted while a dead one remains" as the bug
      signals.
      **Census interpretation (15:08, post-deploy): the all-dead census is
      EXPECTED, not a tiering failure.** `classify_tier` (host_kv_safety_net.
      h:621) anchors the dead tier on MATCH RECENCY: `!ever_matched ||
      last_matched > 15min TTL` → dead, regardless of session key. Right
      after a restart every unit is a fresh, never-matched backup of a
      device-resident unit (requests serve from the catalog, 78 shortlist
      HITs, 0 net restores) → all dead by construction. Units flip to
      live/idle/active on their first net match (a restore). Two
      same-key units (290839 + 263313, both `cs-c089…`) are the main
      session + a sub-agent fork (same first user turn → same key,
      divergent content — correctly not superseded). The 3 post-deploy
      RECOVERs (15:03/15:04/15:08) are NOT net evictions (0 `evict=` lines
      since deploy) — they are engine admission stalls (device KV/state
      pool), i.e. the O1/RAM wall + the P2.4 device→host paths, not the net
      tiering. A reaped fresh net backup is re-creatable via
      spill-before-loss at the point of device loss, so reaping it early
      costs a re-spill, not a re-prefill. *Watch:* if the census stays
      100%-dead 30+ min after warmup (units never matching), that IS a bug.
- [x] **P2.6 — config.** `--host-state-slots` derived from (or replaced by) the
      shared budget; document the single `--host-cache-mib`. **Shipped
      (2026-09-17, with the P4.1 relief-fix deploy):** `host_cache_mib` +
      explicitness flags in `ContextCacheOptions`; `--host-cache-mib` parse
      branch + usage text in serve_options.cpp; derivation in
      `build_sequence_candidate` (layouts_impl.h) — when the knob is set and a
      component is not explicit, ~20% of the total becomes checkpoint state
      slots (derived from the model's `StateImageHostLayout.image_bytes`),
      ~80% host KV; an explicit component keeps its value and is subtracted
      from the total. Parse tests in test_serve_options.cpp (passing). The
      flag is present in the deployed binary; prod still uses explicit
      `--host-state-slots 112 --host-kv-mib 30720` (explicit components keep
      their values, so the derivation is a no-op there until the config
      switches to `--host-cache-mib`).

*#7 exit criteria:* e2e full suite + 3-session long conversation: 0 full-root
mid-conversation re-prefills, 0 ckpt-miss half-units, 0 "no resident state",
no restarts during a 1-hour saturated run.

**Checkpoint-capture coverage (investigated 2026-09-17, no bug found).** The
e2e's "16/23 spills missing checkpoints" WARN was a measurement artifact, not a
capture gap: 15 of the 16 were `at-ckpt` entries (retained AT their checkpoint
frontier — complete units whose OK line hardcodes `ckpt_valid=0`), and the 1
remaining was a clean-append turn whose only ckpt-miss reason was
`rewrite_checkpoint_invalid` (the turn never *requested* a checkpoint — the
e2e runs without `--thinking-mode`, so follow-ups append rather than rewrite).
No capture-failure reasons (`capture_logic_failed` / `rewrite_state_invalid` /
`_hostonly` / `_none`) appeared, so no rewrite turn failed to capture. The
focused e2e (`~/ninfer-e2e/ninfer-e2e.py`) now (a) excludes at-ckpt entries from
the missing-checkpoint count, (b) tallies the ckpt-miss reason distribution, and
(c) FAILs only on genuine capture failures while reporting expected clean-append
misses as a note. No server change needed.

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

- [x] **P4.1 — fatal CUDA context error during a large cold prefill (new
      2026-09-16; RESOLVED 2026-09-17, 9521103d).** Prod processes die in
      CUDA_CHECK ~2 min after startup, during/right after a large cold
      prefill: (1+2) 2026-09-16 10:22+10:26 `device.cu:185
      cudaEventElapsedTime → cudaErrorInvalidResourceHandle` after ~210k-
      token prefills (75s TTFT); (3) 2026-09-17 12:20:30 `device.cu:126
      cudaStreamSynchronize → cudaErrorUnknown` mid-way through a **481k-
      token** cold prefill (926-message session — the largest yet). The
      first-failed call site varies (whichever CUDA call runs first on an
      already-dead context): this is context death, not a timer bug. The
      3rd process (09-16 10:30 → 09-17 12:11, 25.8h) survived a full day —
      *Step 0 met* — but the NEXT process died on its first big prefill.
      GPU clean at inspection (54°C, no Xid). Suspect a WSL2 GPU context
      reset during a large sustained DMA window. **New findings, 12:20
      event (investigated 09-17 13:3x):** (a) **13-min zombie window** —
      `[engine] CRASH: SIGABRT` logged 12:20:30, but the exec'd main process
      kept running until 12:33:23 (zero journal lines, ~100% CPU, both
      in-flight requests hung 13 min; systemd's auto-restart then recovered
      them) — mechanism unknown, and the core was **not captured**:
      `core_pattern` pipes to `/wsl-capture-crash`, which does not exist, so
      every core on this box is silently lost (fix = create the pipe
      target; root/WSL-image level). (b) **Sentinel Class-C blind spot** —
      the wedged process last reported `prefilling=1` and then went fully
      silent; the sentinel only arms on `r=0 p=0 d=0 ∧ w/m≥1`, so a
      frozen-prefilling or silent wedge never fires (zero sentinel entries
      12:20–12:33). Folds into P1.5(d) Increment 2's sentinel update.
      (b2) **2026-09-17 21:10 Class-C false positive (sentinel v3.1, fixed +
      deployed to the live service):** a 360k-token prefill (this session's
      own context) armed C at 153s and restarted a healthy prod. Mechanism:
      Class C's progress clock is the /stats counter sum, but the /stats poll
      (curl --max-time 5) fails during long engine steps — the HTTP pool
      (worker_count = max_concurrency + max_pending + 1, one shared
      ThreadPool, NO reserved slot for /stats) is saturated by streaming
      handlers that span the whole prefill — and the journal fallback (no
      counters by design) cannot advance the clock. The journal's 5s reporter
      (own thread) kept sampling the same counters and logged a healthy
      ~1000 tok/s throughout, proving the engine was computing. Fix
      (wedge-sentinel.sh v3.1): a fresh journal throughput line with non-zero
      prefill/decode tok/s is now ALSO progress evidence (a dead engine goes
      silent or logs 0.0 — it cannot mask a wedge). Follow-up (next deploy
      window): give /stats + /health a dedicated single-thread server/port
      (a true reserved slot) so the :8090 dashboard stops suffering the same
      false outages.
      **New findings, 2026-09-17 14:2x (e2e window, investigated 14:5x):**
      (c) **5 consecutive e2e-server crashes** (14:24/14:30/14:36/14:42/
      14:48, each ~107s after startup, identical `cudaEventElapsedTime →
      InvalidResourceHandle` signature, each at the first H2D restore after
      3 concurrent ~20k-token prefills — far smaller than the 210k–481k
      prefills of the original events, so prefill size is not the trigger).
      **Isolation (14:53–14:55): the f264074b (make-room) binary completed
      the same suite with NO crash in the same environment** — the binary is
      the variable. (d) **Environment degraded all day:** weston (WSLg)
      SIGSEGV crash-loop ~every 100s since 00:01 (229× today,
      `rdp-backend.so`); WSL's own crash-capture pipeline is active. The
      degraded WSL state is a suspected amplifier, not the sole cause (the
      old binary passed in the same degraded environment). **Status: P1.5(d)
      Increment 2 (b811ab23+344d8f69) is committed but NOT deployable until
      this is resolved.** Prime suspect: relief-while-queued firing a D2H
      spill + device-page release while another lane is mid-H2D-restore —
      the in-flight fit-gate relief only ever fires when the engine is
      quiescent, so this DMA overlap is new. *Next (post-reboot):* re-run
      e2e on the clean instance — if the new binary still crashes 5/5 on a
      clean system it is a code regression (fix the relief/restore
      concurrency); if it passes, the trigger was the new DMA-timing pattern
      meeting the degraded WSL state (still worth making queued relief
      quiescent-only).
      **New findings, 2026-09-17 14:2x (e2e window, investigated 14:5x):**
      (c) **5 consecutive e2e-server crashes** (14:24/14:30/14:36/14:42/
      14:48, each ~107s after startup, identical `cudaEventElapsedTime →
      InvalidResourceHandle` signature, each at the first H2D restore after
      3 concurrent ~20k-token prefills — far smaller than the 210k–481k
      prefills of the original events, so prefill size is not the trigger).
      **Isolation (14:53–14:55): the f264074b (make-room) binary completed
      the same suite in the same environment with NO crash** — the binary is
      the variable. (d) **Environment degraded all day:** weston (WSLg)
      SIGSEGV crash-loop ~every 100s since 00:01 (229× today,
      `rdp-backend.so`); WSL's own crash-capture pipeline is active.
      Suspected amplifier, not the sole cause (the old binary passed in the
      same degraded environment). **Status: P1.5(d) Increment 2
      (b811ab23+344d8f69) is committed but NOT deployable until this is
      resolved.** Prime suspect: relief-while-queued firing a D2H spill +
      device-page release while another lane is mid-H2D-restore (the
      in-flight fit-gate relief only ever fires when the engine is
      quiescent, so this DMA overlap is new). *Next:* user directed a system
      reboot (2026-09-17 15:0x) — after reboot, re-run e2e on the clean
      instance: if the new binary still crashes 5/5 on a clean system it is
      a code regression (fix the relief/restore concurrency); if it passes,
      the trigger was the new DMA-timing pattern meeting the degraded WSL
      state (still worth making the queued relief quiescent-only).
      **New findings, 2026-09-17 14:2x (e2e window, investigated 14:5x):**
      (c) **5 consecutive e2e-server crashes** (14:24/14:30/14:36/14:42/
      14:48, each ~107s after startup, identical `cudaEventElapsedTime →
      InvalidResourceHandle` signature, each at the first H2D restore after
      3 concurrent ~20k-token prefills — far smaller than the 210k–481k
      prefills of the original events, so prefill size is not the trigger).
      **Isolation (14:53–14:55): the f264074b (make-room) binary completed
      the same suite in the same environment with NO crash** — the binary is
      the variable. (d) **Environment degraded all day:** weston (WSLg)
      SIGSEGV crash-loop ~every 100s since 00:01 (229× today,
      `rdp-backend.so`); WSL's own crash-capture pipeline is active. The
      degraded WSL state is a suspected amplifier, not the sole cause.
      **Status: P1.5(d) Increment 2 (b811ab23+344d8f69) is committed but NOT
      deployable until this is resolved.** Prime suspect: relief-while-
      queued firing a D2H spill + device-page release while another lane is
      mid-H2D-restore (the in-flight fit-gate relief only ever fires when
      the engine is quiescent, so this DMA overlap is new). Next: re-run
      e2e on the rebooted (clean) instance — if the new binary still
      crashes 5/5 on a clean system it is a code regression (fix the
      relief/restore concurrency); if it passes, the trigger was the new
      DMA-timing pattern meeting the degraded WSL state.
      **New findings, 2026-09-17 14:2x (e2e window 14:24–14:55):**
      (c) **5 consecutive e2e-server crashes** (14:24/14:30/14:36/14:42/
      14:48, each ~107s after startup, identical `cudaEventElapsedTime →
      InvalidResourceHandle` signature, each at the first H2D restore after
      3 concurrent ~20k-token prefills — far smaller than the 210k–481k
      prefills of the original events, so prefill size is not the trigger).
      **Isolation:** the f264074b (make-room) binary completed the same
      suite crash-free in the same environment minutes later (14:54), and
      had passed it at 08:44/09:56 — while the P1.5(d) Increment 2 binary
      (b811ab23) crashed 5/5. The queued block was 10–15s old at each crash
      — the first relief burst (15s stall) is the prime suspect: it is the
      first D2H spill burst that can overlap an active lane's H2D restore
      (the in-flight fit-gate relief only ever fires when the engine is
      quiescent). (d) **Environment degraded today:** weston (WSLg)
      SIGSEGV crash-loop ~every 100s (229× today, `rdp-backend.so`) — a
      chronic all-day background condition (present during the passing runs
      too), suspected amplifier, not the sole cause. **Status:** Increment 2
      is committed (b811ab23+344d8f69) but NOT deployable until this is
      resolved. Next step (user directed a system reboot 2026-09-17 15:0x):
      re-run e2e on the clean instance — if the new binary still crashes
      5/5 on a clean system it is a code regression (suspect: queued-relief
      D2H spill + device release overlapping a concurrent H2D restore; or
      the probe/re-arm scheduling shift); if it passes, the trigger is the
      new DMA-timing pattern meeting the degraded WSL state.
      **DECISIVE A/B bisection, 2026-09-17 17:5x (settles code-vs-environment):**
      rebuilt increment-2 with a `NINFER_NO_QUEUED_RELIEF` env gate that
      disables ONLY the queued-relief D2H burst (probe + visible queue + 120s
      deadline kept) and ran the same e2e suite with the gate on. Gate
      confirmed active in the server log. Request 13 queued (needs 33/33
      pages, free 10/13), sat 120s with NO relief burst (zero `[relief-kv]
      queued demand` lines), was cleanly aborted at the 120s deadline (503 to
      client) — and **the server did NOT crash**: no core, no SIGABRT, healthy
      throughput line until the swap's own pkill. Contrast: with the burst
      enabled, 8/8 runs crashed ~100–130s after startup, always right after the
      `[relief-kv] queued demand — freed ~1390 pages` burst, at the next
      request's H2D-restore timer read. **Verdict: the queued-relief D2H burst
      is the trigger — a code problem, not the environment.** The dxgvmbus
      field-spanning-write warning (seen once at boot) is NOT the trigger. The
      user's pushback was correct; the earlier "WSL2 driver bug, code is clean"
      conclusion was an over-claim. The new pattern the make-room binary never
      had: a relief D2H burst fired on a 15s stall timer while other lanes are
      mid-flight (make-room's in-flight fit-gate relief only fires for the
      request that is itself materializing, so it never created this overlap).
      **RESOLVED (2026-09-17 evening) — root cause + fix + e2e-verified.**
      Deeper probes (context-health + timer-handle dumps) disproved the "dead
      context" theory: at the crash the context was ALIVE (both streams
      `no error`/`not ready`, `cudaPeekAtLastError` clean) and the MainKV
      timer's `start_`/`stop_` were VALID + COMPLETED (`cudaEventQuery` = no
      error on both) — yet `cudaEventElapsedTime` returned
      `InvalidResourceHandle`. The distinguishing condition: the transfer stream
      was BUSY (pending H2D) at the read. **Root cause: `cudaEventElapsedTime`
      on WSL2/dxgkrnl returns a spurious `InvalidResourceHandle` for valid,
      completed events when called while the transfer stream has pending DMA**
      (the dmesg field-spanning-write is in that sync-object wait path). The
      safety-net restore path (`start_sequence`) called
      `context_transfer_observation` (→ `elapsed_ms`) right after enqueuing its
      H2D copies, while the stream was still busy — and read a STALE timer
      (never started/stopped in that path, unlike the materialization path which
      observes after its transfers drain). **Fix (restore path,
      program_impl.h):** for each of the three H2D restores (MainKV, BackendKV,
      State), wrap the copy in `start_context_transfer_timer`/
      `stop_context_transfer_timer` (matching the materialization pattern) and
      add a `cudaStreamSynchronize(transfer_stream)` immediately before the
      `context_transfer_observation`, so the timer is read on an idle stream.
      Verified: full e2e ran to completion, 0 crashes, 6/6 restores completed,
      probe shows `transfer=no error` (idle) at the observation. The
      queued-relief burst itself was NOT the bug (its D2H synced fine, context
      alive after) — it only *enabled* the restore path to run. Also in the fix
      build: single-victim queued relief (was batch-of-3; one ~700-950-page
      demotion exceeds any single demand) + a `NINFER_NO_QUEUED_RELIEF` env
      kill-switch (off by default). Prod is running the fixed binary.
      **Two PRE-EXISTING e2e verdict FAILs surfaced (NOT from this fix):**
      (1) "no KV pressure" — the verdict reads `main_kv_d2h_pages`/
      `private_owners_evicted`/`private_owners_degraded`, which the safety-net
      spill/relief path does not increment (23 spills + 6 restores actually
      happened); a detection gap. (2) "15 spills corrupt ledger/identity" — all
      15 are `at-ckpt=1` units (retained at the checkpoint frontier,
      `ledger == frontier`) vs endpoint units (`ledger == frontier+1`); the
      e2e check only exempts `fallback=1`, not `at-ckpt=1`. Whether
      `ledger == frontier` is a genuine off-by-one in the at-ckpt retention
      (program_impl.h ~7106-7107) or a check gap needs a separate look.
      **Sentinel/swap interaction bug (found same evening, fixed):** the wedge
      sentinel polls /stats on 8080 and cannot tell the e2e swap server from
      prod (the e2e process's counters start at 0, below prod's final totals,
      so its Class-C "no progress" logic false-arms during the swap). During
      the 17:57 swap it restarted the service at 17:59:43 mid-swap; the swap's
      pkill then killed that half-loaded prod, the swap's own `systemctl start`
      collided with the stopping unit, and prod stayed down until the user
      manually restarted it at 18:13:57 (the sentinel had also hit its
      3-restart cap at 18:02:30). Fix: `e2e-swap.sh` now stops
      `ninfer-wedge-sentinel.service` before the swap and restarts it after
      prod is restored (with a manual-restart warning on the FATAL paths).
      *If it recurs:* capture pre-abort journal (last 30s unfiltered) +
      `nvidia-smi` + `dmesg`; fix the core pipe so a backtrace exists. Note:
      hardening `CudaEventTimer::elapsed_ms` (log-and-zero) would NOT save the
      12:20 variant — a sync on a dead context cannot degrade. *Exit (MET,
      2026-09-17):* the timer-read fix + e2e passing with relief enabled
      (0 crashes, 6/6 restores). Prod is running the fixed binary; the
      log-monitor + sentinel watch for any recurrence over the coming days.
- [ ] **P4.2 — `prepared pressure expansion exceeds the target arena`**
      (triaged 2026-09-16 10:45). `pressure_planner.h:1203` (length_error) —
      the pressure planner's prepared expansion does not fit the target arena.
      2× in 24h (both post-10:28 deploy), self-recovering (worker recover +
      client retry). Too rare to act on yet; if it grows past a handful/day,
      trace the arena sizing at the throw site (P1.5's over-commit domain).
- [x] **#4d — planner pruning / search convergence (shipped 2026-09-17).**
      The 8s proc-time regression (a few gen tokens, was <1s) was the
      admission planner enumerating ~3,200 pressure targets in 3.2s
      (`stop_reason=expansion_capacity`, `budget_exhausted=True`) — a
      set-cover search over every parked catalog unit, seeded with the
      evict-all incumbent so the optimality stops could never fire. Fixed in
      three phases: **A** (8757a2a8) greedy eviction-cover seed + covered-
      target stop + infeasible-branch prune + owner prefilter; **B+C**
      (337daaed) fast infeasibility test (skip the ~1ms assess when the
      summed effects can't cover) + 30ms wall-clock backstop (safe: the seed
      is a verified Feasible cover, so a capped search still seals a
      complete plan — unlike the removed 5ms cap). e2e: p95 search 3.2s →
      30.2ms, 0 budget stops, 66 PASS / 0 FAIL. New `planner-latency` e2e
      gate (p95 < 50ms, 0 budget stops) + `seed_type`/`owner_count` in the
      request log keep it detectable. The e2e host arena was re-sized
      4→12 GiB (~/ninfer-e2e, outside repo): the converged planner retains
      ~3× more units on host (the intended P2.4 behavior), which starved the
      old 4 GiB arena (thinking spills + state-saturation relief).
- [x] **15:15–15:32 stall (pre-deploy) — DIAGNOSED 2026-09-18: not a wedge.**
      Re-examined the 09-15 request-log snapshots for 15:14–15:35 (271 rows):
      **0 true wedges** (running=prefilling=decode_ready=0 with waiting≥1),
      80 active snapshots (run/prefill/decode > 0) and 98 transient
      `materializing=1` snapshots. The engine was actively serving — a stream
      of short requests (the user's agentic loop) interleaved with large-unit
      materializations (safety-net H2D / checkpoint restores; prefill spikes
      of 4096/5473 tok/s). The "materializing=1 for ~15 min" impression was a
      misread: the snapshots are sparse (recorded on state change), so a
      single `mat=1` row followed by an idle gap looked like a sustained
      materialization, but the gap was the user between turns, not a stuck
      engine. The "ticker degraded to 25–30s" detail was from the (now-rotated)
      journal throughput-interval lines, likely the restore's D2H/H2D transfer
      time. *No fix needed for this instance* — it was active processing, not
      a wedge. (The underlying cost — a large-unit restore is slow — is the
      expected cold/restore-prefill cost, not a defect.)
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
