# Implemented — shipped & verified

Ledger of completed work, newest first. Each entry: what, commit(s), and how it
was verified. The working plan is `plan.md`; design context is `plan-reference.md`.

## 2026-09-14 — the restart cycle is over (state-lease wedge + error loop)

The user-facing problem was constant server restarts. Three stacked bugs caused
it; all three are fixed and live (deployed 20:04:47, PID 155184).

- **Fit-gate livelock → bounded defer** — `cb0f245c`. The unbounded fit-gate
  defer livelocked the engine (540k defer lines, 3 deaths). Now: a 120s deadline
  aborts the deferred request (engine preserved) + log rate-limit.
  *Verified live:* fired 5× on 09-14 (13:44, 14:18, 14:43, 14:48, 15:05) and 1×
  post-deploy — every time the engine kept serving other sessions, no crash.
- **State-lease H2D wedge** — `fc5d0cf3`. The pool-full "no resident state" →
  root-fallback → adopt-error cascade: the H2D restore needed a device slot with
  no relief when the pool was full. Fix: demote the coldest demotable checkpoint
  before each H2D restore attempt (both sites).
  *Verified:* e2e phase 12 (new regression phase: 4 thinking sessions × 5 rounds,
  rewrite-recycle pressure) 5/5 PASS; prod 0 errors 14:50–15:03.
- **Adopt contract** — `f6a5bec3` (reader side) + `b60c37eb` (writer side).
  Root-fallback terminals (source evicted → `has_source=false`) now acknowledge
  the source as summary-less Retained; adopt releases the claim to Catalogued
  instead of throwing "private source result is missing". The request completes
  on the slow path instead of erroring.
  *Verified live:* 0 "private source result is missing" post-deploy (was 38 in
  the prior 6h); 11 "no resident state" fallbacks completed cleanly.
  Note: `f6a5bec3` alone was inert — the fallback catch never set the flag;
  `b60c37eb` added the writer side.
- **Safe LEAK diagnostics + label rename** — `d11bfeff` + `b60c37eb`. The
  state-lease log now carries `ckpt_refs / residency / shared_refs` (only
  valid-guarded non-throwing accessors) and distinguishes
  `retained (shared prefix)` (false alarm, shared_refs≥1) from
  `LEAK (orphan)` (true orphan, shared_refs=0).
  *Lesson recorded:* the first attempt (`1eb4c5d6`) called `physical_slot()`
  inside the `noexcept` `try_release_state_image` → throw in noexcept →
  `std::terminate` → SIGABRT crash loop; reverted in `2d7042f3`.
- **e2e hardening** — `18908226` (`--start-phase` split, per-phase verdicts) +
  phase 12 state-lease regression; verdict counters key on the new labels.

## 2026-09-13 — regression fixes (deployed)

- **Device-KV fit gate** — `cdaae981`: defer instead of `bad_alloc` under 2
  parallel large sessions (the OOM was device-side, not host).
- **Host-KV safety-net lifecycle** — `74427d6c`: supersede-on-add + liveness
  eviction (dead-conversation giants no longer protected by smallest-first).
- **Host-KV observability + saturation** — `1fd9d08c`, `9183f6ba`: stats
  plumbing, overcommit guard, checkpoint residency; arena compaction +
  `/stats` fragmentation metrics (`34223326`).
- **Planner** — `15d6361c`: removed the 5ms search budget, closed the
  model/physical gap.
- **Frontend** — `431d3324`: ported fork-only jinja fixes,
  `--post-thinking-temperature`.
- **Jinja chat-template engine + froggeric v22.5** (PR #4, `cb535944`):
  vendored jinja engine executes `--chat-template`; byte-exact parity vs the
  jinja2 oracle (16/16 froggeric contract); A/B tool-leak battery green.
- **Post-thinking sampler + atomic {KV + state} unit** (PR #6, `ae8eb23e`):
  `post_thinking` sampling phase; the atomic-unit invariant enforced in
  `host_kv_safety_net.h` (half units refused, cost-aware smallest-unit-first
  eviction, one shared host budget). Quality-neutral on BFCL v4 (p = 0.79).
- **Identity-fail incident (resolved)**: stale pre-compaction binary + jinja
  suffix-detection bug (rewrite_checkpoint never set → zero reuse). Both fixed;
  e2e 11-phase suite 59 PASS / 0 FAIL; prod on the fixed binary.

## Ops

- **Monitor dashboard** — `f640f7b8` + sidecar (`~/ninfer-monitor/`, :8090):
  `/stats` endpoint + log rotation; journal-sourced monitor (`033676e2`)
  watches the live journal for leak / source-missing / OOM signatures.

## Open carries (jinja line — not on the serving path)

- The qwen3.8 artifact still embeds the pre-v22.5 template; rebuilding needs the
  BF16 source checkpoint (not on the Mac). `kFroggericV22DeployedTemplateDigest`
  keeps the deployed digest accepted; live serving overrides via `--chat-template`.
- The frontend does not yet expose `tool_call_format`,
  `auto_disable_thinking_with_tools`, `max_tool_arg_chars`,
  `max_tool_response_chars` (covered by the raw engine suite only).
