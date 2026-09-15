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
      full day of live use.
- [ ] **P1.7 — planner H2D demand + identity-based restore (Slice 0 of #7).**
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
- [ ] **P2.3 — Slice 2: atomic admission.** Admit only when the whole unit fits
      the shared host budget; `rewrite_checkpoint_invalid` becomes an admission
      *failure*, not a degraded half-spill. *Exit:* 0 `ckpt_frontier=0` units
      in e2e + journal.
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
- [ ] **P2.4 — Slice 3: atomic spill/restore.** The unit moves to host / back
      as one; the safety-net spill (already whole-unit) becomes the only spill
      path; restore reassembles into the SAME unit (no 3× duplication: device
      slot + pool slot + net vector; no bypass of StateImageStore residency at
      `program_impl.h:11285–11298`). Kills the "state without KV" forms.
      *Exit:* 0 "no resident state" lines under pool pressure (e2e phase 12).
- [ ] **P2.5 — Slice 4: unit LRU/retention + per-session guarantee.** One LRU
      over the shared budget, evicting whole units cost-aware smallest-first
      (kills the 19s class: state can no longer outlive its KV's retention
      decision); protect an interactive session's unit (≥2 slots: turn-closure
      + checkpoint); idle-aware recency weight. *Exit:* 0 mid-instance
      full-root re-prefills in a long-session e2e; the 19s safety-net class is
      gone.
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
