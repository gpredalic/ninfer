# Pressure planner: from budgeted A* search to a bounded greedy

Status: **proposal** (analysis of the current design + a concrete replacement).
Scope: `src/runtime/engine/materialization_planner.h` and the pressure-target
generation it drives (`program_impl.h` `add_state`/`add_kv`, `state_image_store.h`).

## 1. Observed failure

Under two parallel large sessions (this Claude session + subagents, ~500k-token
prompts), the prod server (fixed jinja build, `9183f6ba`) repeatedly fails
materialization with `std::bad_alloc`:

```
[materialize] materialization source has no resident state — falling back to root prefill
[materialize] safety-net HIT after source eviction: frontier=494437 checkpoint=0
[engine] WORKER OOM: std::bad_alloc - recovering mat=152
```

Live `/stats` at the moment of failure:

| metric | value |
|---|---|
| `host_kv` | 26.7 / 30.0 GiB, 3 free extents, largest 3.9 GiB, frag 0.97 |
| `device_state` | **6 / 6 (full)** |
| `host_state` | **2 / 24 (nearly empty)** |
| `device_main_kv` pages | 9339 |
| `checkpoints_dropped` | 29 (climbing) |
| `searches` / `search_budget_exhaustions` | **104 / 104 (100%)** |

The safety-net restore itself works (the jinja suffix fix is deployed and
capturing checkpoints — 19/21 recent spills log `ckpt_valid=1`). The failure is
the **device state-slot reservation** that follows the restore:
`state_store->reserve_destination()` returns `nullopt` at
`program_impl.h:4759` because the 6-slot device pool is full.

The tell-tale asymmetry: **device_state is 6/6 while host_state is 2/24.** If
demotion-to-host were working, the host pool would be absorbing the pressure.
It is not. State is stuck on device, the pool stays full, and every restore
over-commits.

## 2. What the current planner does

`MaterializationPlanner::plan` (`materialization_planner.h:94`) is a
**budgeted best-first (A*-style) search** over *pressure targets* — sets of
demote / evict / drop actions across the private and shared owners and their
checkpoints:

1. **Identity pass** — for each admission candidate, compute the no-pressure
   ("identity") cost. If one is feasible and dominates all others, seal it and
   return (no pressure needed). This is the fast path and it works.
2. **Incumbent** — if no identity candidate is feasible, seed an incumbent:
   try `guided_closure_target` (a single demote-preferring heuristic), else
   `root_maximal_target` (full eviction).
3. **Search** — expand pressure targets from a priority queue ordered by
   `lower_bound_ns`, tracking the best feasible incumbent. Two auxiliary
   structures: a `pending_` queue (targets to assess) and a `guided_` Pareto
   beam (width 16) that prunes dominated guidance.
4. **Budget** — the search is capped at
   `search_budget_ns = min(5'000'000, incumbent.cost.total_ns / 20)`
   (`:262-263`) and `kTargetBudget = 4096` targets (`:620`). It stops on
   queue-exhausted, model-optimal, **time-budget**, target-budget, or
   expansion-capacity.

The cost model (`fold_assessment`, `:859`) is elaborate: it folds immediate
machine cost + a *portfolio value* of future recovery loss across all owners and
checkpoints, with a 90% discount on eviction-uncertain loss (`:984-988`).

## 3. Drawbacks

1. **Combinatorial space vs. a 5 ms budget.** The target space is the Cartesian
   product of {demote, evict, drop} × {18 private + 6 shared owners} ×
   {their checkpoints}. With ~24 owners and 2–3 checkpoints each, that is
   thousands-to-millions of targets. `min(5ms, cost/20)` is a *fixed* 5 ms cap
   that does not grow with problem size. Result: **100% of searches exhaust the
   time budget** (`searches == search_budget_exhaustions`), so the search
   explores a tiny fraction of the space and returns whatever incumbent it
   happened to find.

2. **The budget is coupled to the incumbent cost, not the work needed.**
   `incumbent.cost.total_ns / 20` means a *larger* prefill (exactly the case
   that needs pressure most) gets the *same* 5 ms cap. The planner is asked to
   solve a harder problem with no more time.

3. **It returns a *modeled*-feasible plan, not a *physically*-feasible one.**
   The incumbent is "feasible" per the cost model's projection. But the actual
   relief (demoting a checkpoint's device replica to host) is gated at
   execution time (`state_image_store.h:265` `drop_device_replica` requires
   `role==CheckpointImmutable && host_slot && source_pins==0 && …`; the
   `add_state` gate at `program_impl.h:1645-1652` requires
   `role==CheckpointImmutable && source_pins==0 && state_exclusive_to_sequence
   && residual.device.state_slots!=0`). When the model assumes a demotion frees
   a slot but the physical gate refuses it (pinned, shared, or the search never
   selected that demotion), the plan over-commits → `reserve_destination()`
   returns `nullopt` → `bad_alloc`.

4. **The cheap relief is not being selected.** The only *cheap* device-state
   relief is demoting a **checkpoint** (rewrite) state to host — it frees 1 of
   the 2 device slots a continuation holds, and keeps the continuation
   catalogued (follow-up does an H2D, not a re-prefill). The endpoint state
   (`ActiveMutable`) cannot be demoted by `add_state` (the gate requires
   `CheckpointImmutable`); it is only freed by full eviction. Because the
   search exhausts its budget before finding a demote plan, the demotion is
   never issued — hence `host_state 2/24` while `device_state 6/6`.

5. **Over-engineered for the problem.** Guided Pareto beams, portfolio-value
   folding, 13-dimension dominance, lower-bound bookkeeping — none of it is
   needed to answer the actual question: *"free exactly N device state slots and
   M KV pages, cheapest-first, without killing a live continuation if a demotion
   suffices."*

## 4. Root cause of the `bad_alloc`

The device state pool is `plan.persistent.state_images` = **6 slots**
(`program_impl.h:843-856`), i.e. `2 × max_concurrency(3)`. Each active
continuation that captured a checkpoint holds **2** device slots (endpoint
`ActiveMutable` + rewrite `CheckpointImmutable`). So 3 active continuations fill
the pool exactly — **zero headroom**. Any restore of an evicted session (which
needs 2 slots) while 3 are active *must* first free 2 slots. The correct, cheap
action is to demote the 2 coldest checkpoints to host. The current planner does
not reliably issue that demotion (budget exhaustion + model/physical gap), so
the restore over-commits and throws `bad_alloc`.

This is the regression the user identified: before the atomic `{KV + state}`
work (`ae8eb23e` + host-state-pool `79614e4f`), the safety net held KV only and
state was not pooled, so there was no 6-slot pool to exhaust and eviction to
host KV worked. The state pool + the budgeted planner together broke the
eviction-to-host path.

## 5. Proposed design: bounded greedy

Replace the search with a deterministic, physically-grounded greedy that answers
the exact question and never over-commits.

### 5.1 Inputs

- **Demand** `D`: device state slots and KV pages the materialization needs
  (already computed as `details.demand` / `reservation_added`).
- **Free** `F`: currently free device state slots and KV pages.
- **Deficit** `Δ = D − F` (per resource; clamp at 0).

### 5.2 Relief actions (cheapest → most expensive)

| # | action | frees | keeps continuation | follow-up cost |
|---|--------|-------|--------------------|----------------|
| 1 | **Demote checkpoint** (rewrite state device replica → host) | 1 device state slot | yes (catalogued) | H2D of checkpoint state (cheap) |
| 2 | **Demote endpoint** (continuation → HostOnly) | 1 device state slot | yes (catalogued) | H2D of full endpoint state |
| 3 | **Evict** (spill `{KV+state}` to host KV safety net) | 2 device state slots + its KV pages | no (evicted) | H2D of KV+state, or re-prefill if the net entry was itself evicted |

Action 1 is the workhorse: it is the only relief that frees a slot *without*
evicting, and it is exactly what the current planner fails to issue.

### 5.3 Algorithm

"Coldest" is defined by the **existing cost model** (`fold_assessment` +
portfolio value), not by a recency proxy. An action's *loss* is what the cost
function already computes for that owner/checkpoint: immediate copy cost
(`transferred_bytes`) + future recovery loss — re-prefill cost (`rebuild_ns`),
selected-hit count, retention weight, with the 90% safety-net discount on
eviction-uncertain loss. **Size and re-prefill cost are therefore priced
exactly as the cost function intends.**

- **Demote checkpoint o**: loss ≈ D2H bytes + recovery delta for that
  checkpoint (≈ 0 while the session stays catalogued — the follow-up does an
  H2D, not a re-prefill).
- **Demote endpoint o**: loss ≈ D2H bytes + loss of the endpoint recovery path
  (recovery falls back to the checkpoint, else re-prefill).
- **Evict owner o**: loss = portfolio public/private loss of evicting o,
  including `rebuild_ns` (full re-prefill cost), discounted 90% for the
  safety-net-uncertain portion — so an owner that just spilled to the host KV
  net is *cheap* to evict (its follow-up is an H2D, not a re-prefill).

Actions are ranked by **loss per unit of relief** (loss ÷ state slots freed;
KV-page relief counts toward Δ_kv when it is > 0) and applied in order until
the deficit is met:

```
plan(demand D, free F, owners):
    Δ_state = max(0, D.device_state_slots - F.device_state_slots)
    Δ_kv    = max(0, D.kv_pages          - F.kv_pages)
    if Δ_state == 0 and Δ_kv == 0:
        return NoPressure()                      # identity fast path

    # All feasible relief actions, each priced by the cost model.
    # Feasibility = the same physical gates the executor uses, so a planned
    # action is guaranteed to execute.
    actions = feasible_actions(owners)           # demote-ckpt / demote-ep / evict
    for action in actions sorted by (loss / relief, loss, ordinal):
        if Δ_state == 0 and Δ_kv == 0: break
        apply(action)                            # updates the store
        Δ_state -= action.frees_state_slots
        Δ_kv    -= action.frees_kv_pages
        F = actual_free()                        # re-read, not modeled

    if Δ_state <= 0 and Δ_kv <= 0:
        return Plan(applied_actions)             # physically verified
    else:
        return Defer()                           # cannot admit; wait, don't over-commit
```

The per-unit ranking avoids the classic greedy trap (two cheap 1-slot demotes
beating one 2-slot eviction of lower total loss). The deficit is at most a
few slots, so the approximation is bounded; if exactness is ever wanted, the
same action list feeds a trivial DP over the ≤ 6-slot deficit capacity.

`demotable` / `evictable` call the **same physical predicates** the execution
path uses (`state_image_store.h` gates), so a planned action is guaranteed to
succeed when executed. After applying, the planner re-reads the **actual** free
counts (not a model) before returning.

### 5.4 Why this is better

- **No combinatorial search, no time budget.** O(n log n) sort + one linear
  pass. Microseconds, deterministic, no `search_budget_exhaustions`.
- **Physically feasible by construction.** It only issues actions whose gates
  pass, and verifies the real free counts before admitting. No over-commit, no
  `bad_alloc`.
- **Cheapest-first.** Demotes a checkpoint (keeps the session live) before it
  ever evicts. Eviction is the last resort, applied to the owner with the
  lowest loss-per-relief under the cost model (fewest hits, oldest, smallest
  `rebuild_ns`, just-spilled-to-net).
- **Bounded and debuggable.** At most `n` demotes + `m` evicts; every decision is
  a single inspectable line. Easy to unit-test and to reason about under load.
- **Defer, don't over-commit.** If the deficit can't be met, the request waits
  (the existing defer path) instead of reserving into a full pool.

### 5.5 What stays

- The identity fast path (no pressure needed) is kept verbatim — it is correct
  and cheap.
- The cost *model* is kept and reused as a black box: `fold_assessment` /
  portfolio value prices each action (loss = immediate + future recovery, with
  the safety-net discount). What is removed is only the **search machinery**
  around it — the A* queues, the guided Pareto beam, the 5 ms time budget, the
  4096-target budget, the lower-bound bookkeeping. The cost function we already
  designed is the greedy's ranking key, not a casualty of the refactor.

## 6. Supporting changes

1. **Audit the demotion gate** (`program_impl.h:1645-1652`,
   `state_image_store.h:265`). Confirm a `CheckpointImmutable` state with
   `source_pins==0` and a free host slot is actually demoted under device
   pressure. If `source_pins` is non-zero because the checkpoint is the active
   fork source, the greedy must demote a *different* (older) checkpoint, not
   the pinned one — the coldest-first ordering already does this.
2. **Stats** (the "more stats is always good" ask):
   - live checkpoint count (device vs host residency),
   - device KV bytes + state slots held by checkpoints,
   - materialization failures **by resource type** (state-slot vs KV-page vs
     host-arena) — this directly confirms the 6/6 device-state diagnosis,
   - demotions issued vs refused (and the refusal reason).
3. **E2E repro**: a phase that drives **2 parallel large sessions** to
   device-state/KV pressure and asserts **zero `bad_alloc`** and that the
   device-state deficit is relieved by demotion (host_state rises) rather than
   eviction.
4. **Headroom**: consider sizing the device pool with +1 or +2 slots of
   headroom (`2×max_concurrency + k`) so a restore never needs to demote under
   normal load; the greedy then only acts under genuine pressure.

## 7. Rollout

1. Add the stats (§6.2) to the current build and confirm the 6/6 device-state +
   2/24 host-state asymmetry and the failure-by-resource breakdown.
2. Add the e2e 2-parallel-large-sessions phase; confirm it reproduces
   `bad_alloc` on the current build.
3. Implement the greedy planner behind the existing `plan()` interface (same
   `Result` shape), keep the identity fast path.
4. Audit/relax the demotion gate so the greedy's demotes actually execute.
5. Re-run the e2e phase: expect zero `bad_alloc`, host_state rising under
   pressure, `search_budget_exhaustions` gone.
6. Deploy; watch `/stats` for `device_state` headroom and demotion counts.
