#pragma once



// Host-KV safety net: when the pressure planner evicts a continuation, the device KV pages

// are copied to host RAM before release. When a matching prefix returns, the host copy is

// found by prefix matching and restored via H2D.



#include "core/host_kv_arena.h"

#include "targets/qwen3_6/impl/runtime/logical_kv_store.h"
#include "targets/qwen3_6/impl/runtime/prefix_identity.h"
#include <ninfer/targets/qwen3_6/state_image.h>



#include <algorithm>

#include <atomic>

#include <chrono>

#include <cstddef>

#include <cstdint>

#include <limits>

#include <optional>

#include <span>

#include <vector>

#include <stdexcept>

#include <cstdio>

#include <utility>
#include <functional>

#include <vector>



namespace ninfer::targets::qwen3_6::detail {



struct HostKVSafetyNetEntry {

    // Identity for prefix matching against incoming prompts.

    ResidentPrefixIdentity prefix_identity;

    std::vector<TokenId> ledger;

    std::uint32_t execution_frontier = 0;



    // Raw host KV allocations from HostKVArena. Each is a single contiguous

    // extent: the spill path requires one extent per component and repairs
    // arena fragmentation by compacting (relocating live extents) instead of
    // splitting the allocation across extents. The vector keeps the entry
    // format stable; it holds exactly one allocation per component.

    std::vector<HostKVAllocation> text_allocations;

    std::vector<HostKVAllocation> backend_allocations;



    // Page layout info (copied from the arena's supported layouts).

    std::uint32_t text_page_count    = 0;

    std::uint32_t backend_page_count = 0;



    // Raw pinned-host buffer for the continuation state image.

    // P2.4 Increment 2 (net as the unit's host home): the state image lives in
    // a HostStatePool slot — the same pool the StateImageStore uses for host
    // replicas — instead of an untracked heap vector. Pool occupancy is the
    // census: host state residency is fully explained by pool slots (store
    // replicas + net entries), and a net entry's slot is the unit's host home
    // (after a relinquishing spill, the net entry is the sole host copy).
    // The bytes fields are the budget accounting (slots x image_bytes).
    std::optional<qwen3_6::HostStateSlotHandle> state_slot;

    std::size_t state_bytes = 0;



    // Checkpoint (turn-closure/rewrite) fallback. Follow-up prompts diverge

    // from the ledger exactly where the previous turn's generation began

    // (reasoning/replay drops), so the full execution_frontier never matches

    // them. The checkpoint frontier is where the prompt still matches; its

    // state image rides along so a checkpoint-level restore is exact.

    bool checkpoint_valid     = false;

    std::uint32_t checkpoint_frontier = 0;

    std::optional<qwen3_6::HostStateSlotHandle> checkpoint_state_slot;

    std::size_t checkpoint_state_bytes = 0;



    // Timestamp for LRU eviction.

    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();



    // Liveness: whether find() has ever matched this entry, and when it last
    // did. Eviction treats entries that never matched, or that have been
    // unmatched for longer than the dead TTL, as dead weight (evicted
    // largest-first — they cost nothing to lose); live entries are evicted
    // smallest-first (cheapest re-prefill). Updated by find() on the serve
    // thread; mutable because find() is const.

    mutable bool ever_matched = false;

    mutable std::chrono::steady_clock::time_point last_matched{};



    // Pin flag: when set, the spill's LRU eviction loop skips this entry.

    // Used by the restore flow to prevent eviction during the reserve→start

    // pipeline without removing the entry from the net (which would hold

    // arena bytes hostage as a taken entry).

    bool pinned = false;



    // Unique ID for stable reference across vector modifications.

    // pin() stores this ID; take_pinned() re-finds the entry by ID.

    std::uint64_t entry_id = 0;



    // Session key from the catalog entry at spill time. Used as a fallback

    // for thinking-mode follow-ups where prefix matching fails (preserve_thinking=off

    // drops reasoning from all turns, diverging the prompt from the ledger at the

    // first reasoning point). The session key lets the safety-find match by

    // session identity instead of prefix content.

    std::optional<qwen3_6::PreparedSessionKey> session_key;



    // Compact prefix (token IDs without reasoning) for matching thinking-mode

    // follow-ups. When non-empty, safety-find uses this instead of the full

    // ledger for prefix matching.

    std::vector<TokenId> compact_prefix;

    // The logical device pages this entry was spilled from, in page order
    // (P1.7(b) identity-based restore). At restore, the longest prefix of
    // these still shareable (device-resident, full, no writer — frozen) is
    // adopted into the restored address space instead of re-materialized +
    // H2D-copied, so an already-resident prefix is not re-allocated.

    std::vector<LogicalKVPageHandle> text_source_pages;

    std::vector<LogicalKVPageHandle> backend_source_pages;

};



// Result of find(): which entry matched, how many tokens are reusable, and

// whether the match is at the checkpoint frontier (state selection differs).

struct HostKVSafetyNetMatch {

    std::size_t index         = 0;

    std::uint32_t reuse_tokens = 0;

    bool checkpoint           = false;

};



class HostKVSafetyNet {

public:

    HostKVSafetyNet() = default;





    HostKVSafetyNet(const HostKVSafetyNet&)            = delete;

    HostKVSafetyNet& operator=(const HostKVSafetyNet&) = delete;

    HostKVSafetyNet(HostKVSafetyNet&&)                 = default;

    HostKVSafetyNet& operator=(HostKVSafetyNet&&)      = default;



    // Find entry whose prefix_identity and ledger match the given prompt for `count` tokens.

    // Returns the index of the best (longest) match, or std::nullopt if none.

    // Two-level find, the old HostKvCache pattern: try the full execution

    // frontier first (prompt extends the cached turn), then the checkpoint

    // frontier (prompt rewinds: reasoning/replay dropped the generated turn).

    [[nodiscard]] std::optional<HostKVSafetyNetMatch>

    find(const PreparedPromptData& prompt, std::size_t max_count,

         const std::optional<qwen3_6::PreparedSessionKey>& session_key = {}) const {

        std::optional<HostKVSafetyNetMatch> best;

        {

            std::uint64_t prompt_hash = 1469598103934665603ULL;

            for (const TokenId t : prompt.token_ids) {

                prompt_hash ^= static_cast<std::uint64_t>(t);

                prompt_hash *= 1099511628211ULL;

            }

            std::fprintf(stderr,

                         "[safety-find] incoming: prompt_tokens=%zu prompt_hash=%llu max_count=%zu\n",

                         prompt.token_ids.size(),

                         static_cast<unsigned long long>(prompt_hash), max_count);

        }

        if (session_key) {

            std::fprintf(stderr,

                         "[safety-find] incoming session_key set: view=%.*s\n",

                         static_cast<int>(session_key->view().size()),

                         session_key->view().data());

        } else {

            std::fprintf(stderr,

                         "[safety-find] incoming session_key NOT SET (nullopt)\n");

        }

        for (std::size_t index = 0; index < entries_.size(); ++index) {

            const HostKVSafetyNetEntry& entry = entries_[index];


            {

                std::uint64_t cp_hash = 1469598103934665603ULL;

                for (const TokenId t : entry.compact_prefix) {

                    cp_hash ^= static_cast<std::uint64_t>(t);

                    cp_hash *= 1099511628211ULL;

                }

                std::fprintf(stderr,

                             "[safety-find] entry %zu: exec_frontier=%u ckpt_frontier=%u cp_size=%zu cp_hash=%llu ledger_size=%zu has_sk=%d\n",

                             index, entry.execution_frontier, entry.checkpoint_frontier,

                             entry.compact_prefix.size(),

                             static_cast<unsigned long long>(cp_hash),

                             entry.ledger.size(),

                             entry.session_key ? 1 : 0);

            }

            const std::size_t count =

                std::min(max_count, static_cast<std::size_t>(entry.execution_frontier));

            const std::size_t effective_count =
                entry.compact_prefix.empty() ? count
                    : std::min(count, entry.compact_prefix.size());

            std::uint32_t reuse = 0;

            bool checkpoint = false;

            if (effective_count != 0 &&

                prefix_matches(prompt,

                               entry.compact_prefix.empty()

                                   ? std::span<const TokenId>(entry.ledger.data(), effective_count)

                                   : std::span<const TokenId>(entry.compact_prefix.data(), effective_count),

                               entry.prefix_identity, effective_count)) {

                reuse = static_cast<std::uint32_t>(effective_count);

            } else if (entry.checkpoint_valid && entry.checkpoint_frontier != 0 &&

                       entry.checkpoint_frontier <= max_count &&

                       static_cast<std::size_t>(entry.checkpoint_frontier) <= entry.ledger.size() &&

                       prefix_matches(prompt,

                                      entry.compact_prefix.empty()

                                          ? std::span<const TokenId>(entry.ledger.data(), entry.checkpoint_frontier)

                                          : std::span<const TokenId>(entry.compact_prefix.data(), std::min(static_cast<std::size_t>(entry.checkpoint_frontier), entry.compact_prefix.size())),

                                      entry.prefix_identity, entry.checkpoint_frontier)) {

                reuse = entry.checkpoint_frontier;

                checkpoint = true;

            }

            // Session-key fallback: if prefix matching failed (thinking mode

            // drops reasoning from all turns, diverging the prompt from the

            // ledger at the first reasoning point), match by session identity.

            // The session key confirms this is the right continuation; use the

            // checkpoint frontier as the reuse level (the turn boundary where

            // the stored context still matches the prompt).

            if (reuse == 0 && session_key && entry.session_key) {

                const bool keys_match = (*session_key == *entry.session_key);

                std::fprintf(stderr,

                             "[safety-find] session-key compare: match=%d incoming_view=%.*s entry_view=%.*s "

                             "frontier=%u ckpt_valid=%d ckpt_frontier=%u max_count=%zu\n",

                             static_cast<int>(keys_match),

                             static_cast<int>(session_key->view().size()), session_key->view().data(),

                             static_cast<int>(entry.session_key->view().size()), entry.session_key->view().data(),

                             entry.execution_frontier,

                             static_cast<int>(entry.checkpoint_valid),

                             entry.checkpoint_frontier, max_count);

                if (keys_match &&

                    entry.checkpoint_valid && entry.checkpoint_frontier != 0 &&

                    entry.checkpoint_frontier < max_count) {

                    reuse = entry.checkpoint_frontier;

                    checkpoint = true;

                    std::fprintf(stderr,

                                 "[safety-find] session-key HIT: frontier=%u checkpoint=%u\n",

                                 entry.execution_frontier, entry.checkpoint_frontier);

                } else if (keys_match) {

                    std::fprintf(stderr,

                                 "[safety-find] session-key match but conditions fail: "

                                 "ckpt_valid=%d ckpt_frontier=%u max_count=%zu\n",

                                 static_cast<int>(entry.checkpoint_valid),

                                 entry.checkpoint_frontier, max_count);

                }

            } else if (reuse == 0 && !session_key) {

                // Only log once per find() call

            }

            if (reuse == 0 && session_key && !entry.session_key) {

                std::fprintf(stderr,

                             "[safety-find] entry has no session_key (index=%zu frontier=%u)\n",

                             index, entry.execution_frontier);

            }

            if (reuse == 0 && !session_key) {

                // Incoming request has no session_key — can't do session-based fallback

            }

            // reuse == max_count (the entire prompt is the cached prefix,

            // e.g. a strict-prefix retry) leaves no target tail for prefill

            // and throws "zero-suffix reuse" downstream — reject it and let

            // the request prefill from scratch instead of failing.

            if (reuse != 0 && reuse != max_count && (!best || reuse > best->reuse_tokens)) {

                best = HostKVSafetyNetMatch{

                    .index = index, .reuse_tokens = reuse, .checkpoint = checkpoint};

            }

        }

        // One line per find() call, but only when something is in the net —

        // an empty net on a root admission is the common no-op case.

        if (best) {

            // Liveness: a hit proves the conversation is alive. Entries that

            // never get here (client compacted the conversation, session died)

            // become dead weight for the eviction loops. (const ref: the

            // liveness fields are mutable.)

            const HostKVSafetyNetEntry& matched = entries_[best->index];

            matched.ever_matched = true;

            matched.last_matched = std::chrono::steady_clock::now();

        }

        if (!entries_.empty() || best) {

            std::fprintf(stderr,

                         "[safety-find] entries=%zu max_count=%zu match=%s frontier=%u checkpoint=%d\n",

                         entries_.size(), max_count, best ? "hit" : "miss",

                         best ? best->reuse_tokens : 0,

                         best ? static_cast<int>(best->checkpoint) : 0);

        }

        

        return best;

    }



    // Add a new entry. The safety net is bounded by host KV arena bytes,

    // not entry count — the spill function evicts smallest unpinned entries

    // to free arena memory before calling add(). A re-added entry (after

    // restore) already owns its arena allocation, so no eviction is needed.

    // --- Host state pool -------------------------------------------------
    // Retained state images are host memory, exactly like host KV pages, so
    // they share one accounted budget instead of living in unaccounted heap.
    // The budget is set by the engine from the host memory budget.
    void set_state_budget_bytes(std::size_t bytes) noexcept { state_budget_bytes_ = bytes; }
    [[nodiscard]] std::size_t state_budget_bytes() const noexcept { return state_budget_bytes_; }
    [[nodiscard]] std::size_t retained_state_bytes() const noexcept { return state_retained_bytes_; }

    // Cumulative whole units evicted (spill-path eviction + budget-driven
    // reclaim). Monotonic; read by /stats from the serve thread.
    [[nodiscard]] std::uint64_t eviction_count() const noexcept {
        return evictions_.load(std::memory_order_relaxed);
    }

    // Cumulative entries dropped by supersede-on-add (a newer entry for the
    // same conversation made them redundant). Monotonic; read by /stats.
    [[nodiscard]] std::uint64_t superseded_count() const noexcept {
        return superseded_.load(std::memory_order_relaxed);
    }

    // Dead-TTL for liveness-based eviction (see select_eviction_victim).
    void set_dead_ttl(std::chrono::seconds ttl) noexcept { dead_ttl_ = ttl; }

    // P2.5: predicate marking a session as currently active (its
    // continuation still exists). Set once by the program. A live
    // session's unit is a last-resort eviction victim: idle sessions'
    // units go first, an active session's unit only when nothing else
    // is evictable.
    void set_session_is_live(std::function<bool(const std::optional<qwen3_6::PreparedSessionKey>&)> pred) {
        session_is_live_ = std::move(pred);
    }
    // P2.5 Increment 3: predicate for the actively-serving session (Active
    // continuation only). When set, eviction tiers idle (Catalogued) units
    // between the unprotected-live and active tiers, so old copies are reaped
    // before the session currently being served. When unset, the active tier
    // falls back to session_is_live_ (the pre-Increment-3 behavior).
    void set_session_is_active(std::function<bool(const std::optional<qwen3_6::PreparedSessionKey>&)> pred) {
        session_is_active_ = std::move(pred);
    }
    [[nodiscard]] std::chrono::seconds dead_ttl() const noexcept { return dead_ttl_; }

    // P2.4 Increment 2: entries hold their state images in HostStatePool slots
    // (the same pool the StateImageStore uses for host replicas). The pool is
    // owned by the program, so the net returns slots through this callback
    // when an entry is dropped (eviction, supersede, arena reclaim). take() /
    // take_pinned() do NOT release — the entry's slots transfer with the entry
    // and the program releases them after the restore consumes them.
    void set_state_slot_releaser(std::function<void(const HostKVSafetyNetEntry&)> releaser) {
        state_slot_releaser_ = std::move(releaser);
    }

    // Census: state-image slots currently held by net entries (endpoint +
    // checkpoint). Together with the store's host replicas this fully explains
    // the HostStatePool occupancy (the host state residency census).
    [[nodiscard]] std::uint32_t state_slots_held() const noexcept {
        std::uint32_t slots = 0;
        for (const HostKVSafetyNetEntry& entry : entries_) {
            if (entry.state_slot) { ++slots; }
            if (entry.checkpoint_state_slot) { ++slots; }
        }
        return slots;
    }

    // Pick the next eviction victim under the shared four-tier policy
    // (dead-largest, live-smallest, idle-catalogued-smallest, active-smallest).
    // The spill loop drives the pinned phase through allow_pinned. P2.5: one
    // pass covers every candidate — an actively-serving (Active) session's unit
    // is returned only when nothing dead, unprotected-live, or idle (Catalogued
    // "old copy") remains (the per-session guarantee), so a second pass could
    // never find more.
    [[nodiscard]] std::optional<std::size_t> select_victim(bool allow_pinned) const noexcept {
        return select_eviction_victim(entries_, std::chrono::steady_clock::now(), dead_ttl_,
                                      allow_pinned, session_is_live_,
                                      /*protect_live_sessions=*/true, session_is_active_);
    }

    // P2.5 Increment 3: which eviction tier an entry falls into under the
    // current predicates — mirrors select_eviction_victim's classification
    // (dead > unprotected-live > idle-catalogued > active). For logging: the
    // evict lines report tier= so the journal shows whether a victim was a
    // dead remnant, an unprotected live unit, an idle (Catalogued) old copy,
    // or the actively-serving session's unit.
    [[nodiscard]] const char* classify_tier(const HostKVSafetyNetEntry& entry,
                                            std::chrono::steady_clock::time_point now) const noexcept {
        if (!entry.ever_matched || (now - entry.last_matched) > dead_ttl_) { return "dead"; }
        if (!session_is_live_ || !entry.session_key ||
            !session_is_live_(*entry.session_key)) { return "live"; }
        const bool active = session_is_active_
            ? session_is_active_(*entry.session_key)
            : session_is_live_(*entry.session_key);  // fallback: old lumped tier
        return active ? "active" : "idle";
    }

    // Host KV pages and retained state images share ONE host memory budget. The
    // arena is the other tenant, so it is queried live instead of duplicating the
    // limit: neither pool may consume the other's headroom.
    void set_shared_arena(const HostKVArena* arena) noexcept { shared_arena_ = arena; }

    [[nodiscard]] std::size_t shared_occupied_bytes() const noexcept {
        return (shared_arena_ != nullptr ? shared_arena_->occupied_bytes() : 0) +
               state_retained_bytes_;
    }

    // P2.2 (#7 Slice 1): the shared meter over host unit occupancy — the sum of
    // each retained unit's cost (KV page bytes + state image bytes). One number
    // across the KV and state halves, instead of the two pools accounted
    // separately. Computed on demand so it is always the sum of the parts (no
    // incremental bookkeeping to drift); P2.4/P2.5 adopt the incremental
    // CacheUnitOccupancy when eviction moves whole units. Saturates instead of
    // throwing: this is a gauge, not an admission check.
    [[nodiscard]] std::uint64_t unit_occupied_bytes(std::uint64_t text_stride,
                                                    std::uint64_t backend_stride) const noexcept {
        if (text_stride == 0 && backend_stride == 0) { return 0; }
        if (text_stride == 0) { text_stride = backend_stride; }
        if (backend_stride == 0) { backend_stride = text_stride; }
        std::uint64_t total = 0;
        for (const HostKVSafetyNetEntry& entry : entries_) {
            const std::uint64_t text_pages    = entry.text_page_count;
            const std::uint64_t backend_pages = entry.backend_page_count;
            const std::uint64_t text_bytes =
                text_pages > std::numeric_limits<std::uint64_t>::max() / text_stride
                    ? std::numeric_limits<std::uint64_t>::max()
                    : text_pages * text_stride;
            const std::uint64_t backend_bytes =
                backend_pages > std::numeric_limits<std::uint64_t>::max() / backend_stride
                    ? std::numeric_limits<std::uint64_t>::max()
                    : backend_pages * backend_stride;
            const std::uint64_t kv_bytes =
                text_bytes > std::numeric_limits<std::uint64_t>::max() - backend_bytes
                    ? std::numeric_limits<std::uint64_t>::max() - backend_bytes
                    : text_bytes + backend_bytes;
            const std::uint64_t state_bytes = entry_state_bytes(entry);
            const std::uint64_t unit =
                kv_bytes > std::numeric_limits<std::uint64_t>::max() - state_bytes
                    ? std::numeric_limits<std::uint64_t>::max() - kv_bytes
                    : kv_bytes + state_bytes;
            total = total > std::numeric_limits<std::uint64_t>::max() - unit
                        ? std::numeric_limits<std::uint64_t>::max()
                        : total + unit;
        }
        return total;
    }

    [[nodiscard]] static std::size_t entry_state_bytes(const HostKVSafetyNetEntry& entry) noexcept {
        return entry.state_bytes + entry.checkpoint_state_bytes;
    }

    // A cache unit is {attention KV + GDN state}. It is retained atomically or not at
    // all: KV without its state cannot resume (the attention K/V is not independently
    // recomputable, because it depends on the GDN recurrence), and state without its KV
    // is only usable while that KV is device-resident. A half unit is therefore refused
    // rather than stored - it would waste host memory and, worse, prefix matching could
    // select it and restore with a missing half, silently producing a wrong continuation.
    [[nodiscard]] static bool is_complete_unit(const HostKVSafetyNetEntry& entry) noexcept {
        const bool has_kv = entry.text_page_count != 0 || entry.backend_page_count != 0 ||
                            !entry.text_allocations.empty() || !entry.backend_allocations.empty();
        return has_kv && entry_state_bytes(entry) != 0;
    }

    // Re-prefill cost proxy for one unit. Cost scales with the retained context, so the
    // retained KV page count is the comparable term; the state image is fixed size and
    // common to every unit, so it cannot affect the ordering.
    [[nodiscard]] static std::size_t unit_context_pages(const HostKVSafetyNetEntry& entry) noexcept {
        return static_cast<std::size_t>(entry.text_page_count) +
               static_cast<std::size_t>(entry.backend_page_count);
    }


    // Victim selection shared by both eviction loops (retain_state_capture and
    // the spill's evict loop). Four tiers:
    //   dead  — never matched, or unmatched for longer than the dead TTL.
    //           The conversation is gone, so re-prefill cost is ZERO: evict
    //           the LARGEST dead entry first (frees the most arena). This is
    //           what reaps stale giants from compacted conversations.
    //   live  — matched within the TTL. Re-prefill cost scales with context
    //           length, so evict the SMALLEST first; oldest last-match breaks
    //           ties. (plan.md's cost model, with liveness as the primary key.)
    //   idle-catalogued — P2.5 Increment 3: live units whose session is
    //           Catalogued (retained for reuse) but NOT actively being served
    //           (session_is_live true, session_is_active false) — "old copies".
    //           Evicted before the active session's unit, so a single driven
    //           session's frontier is not displaced by stale idle copies.
    //   active — P2.5: live units whose session is actively being served
    //           (session_is_active matches an Active continuation) are evicted
    //           only after every dead, unprotected-live, and idle-catalogued
    //           unit is gone: an active session's unit is a last-resort victim
    //           (the per-session guarantee). Protection is ordering only — the
    //           tier is exhausted before selection gives up.
    // Pinned entries are candidates only in the spill loop's phase 2
    // (allow_pinned); they are always live (pinned right after a find hit).
    [[nodiscard]] static std::optional<std::size_t>
    select_eviction_victim(const std::vector<HostKVSafetyNetEntry>& entries,
                           std::chrono::steady_clock::time_point now,
                           std::chrono::seconds dead_ttl, bool allow_pinned,
                           const std::function<bool(const std::optional<qwen3_6::PreparedSessionKey>&)>& session_is_live = nullptr,
                           bool protect_live_sessions = false,
                           const std::function<bool(const std::optional<qwen3_6::PreparedSessionKey>&)>& session_is_active = nullptr) noexcept {
        std::optional<std::size_t> dead;
        std::size_t dead_size = 0;
        std::optional<std::size_t> live;
        std::size_t live_size = 0;
        std::chrono::steady_clock::time_point live_time{};
        // P2.5 Increment 3: idle (Catalogued) "old copy" units — retained for
        // reuse but not currently being served. Evicted before the
        // actively-serving (Active) session's unit.
        std::optional<std::size_t> live_idle;
        std::size_t live_idle_size = 0;
        std::chrono::steady_clock::time_point live_idle_time{};
        std::optional<std::size_t> live_active;
        std::size_t live_active_size = 0;
        std::chrono::steady_clock::time_point live_active_time{};
        const auto is_active = [&](const HostKVSafetyNetEntry& e) -> bool {
            if (!e.session_key) { return false; }
            if (session_is_active) { return session_is_active(*e.session_key); }
            // No active predicate wired: fall back to the live predicate so the
            // pre-Increment-3 behavior (all live units in one protected tier)
            // is preserved.
            return session_is_live ? session_is_live(*e.session_key) : false;
        };
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const HostKVSafetyNetEntry& candidate = entries[i];
            if (candidate.pinned && !allow_pinned) { continue; }
            if (!is_complete_unit(candidate)) { continue; }
            const bool is_dead = !candidate.ever_matched ||
                                 (now - candidate.last_matched) > dead_ttl;
            const std::size_t size = unit_context_pages(candidate);
            if (is_dead) {
                if (!dead || size > dead_size) { dead = i; dead_size = size; }
            } else if (protect_live_sessions && candidate.session_key &&
                       session_is_live && session_is_live(*candidate.session_key)) {
                if (is_active(candidate)) {
                    if (!live_active || size < live_active_size ||
                        (size == live_active_size && candidate.last_matched < live_active_time)) {
                        live_active = i;
                        live_active_size = size;
                        live_active_time = candidate.last_matched;
                    }
                } else {
                    if (!live_idle || size < live_idle_size ||
                        (size == live_idle_size && candidate.last_matched < live_idle_time)) {
                        live_idle = i;
                        live_idle_size = size;
                        live_idle_time = candidate.last_matched;
                    }
                }
            } else if (!live || size < live_size ||
                       (size == live_size && candidate.last_matched < live_time)) {
                live = i;
                live_size = size;
                live_time = candidate.last_matched;
            }
        }
        if (dead) { return dead; }
        if (live) { return live; }
        if (live_idle) { return live_idle; }
        return live_active;
    }

    // Reclaim whole units until `incoming` fits the shared host budget. Eviction
    // is cost-aware (see select_eviction_victim): dead weight goes first
    // (largest), then the smallest live unit, and only as a last resort an
    // active session's smallest unit (the per-session guarantee). Returns false
    // when the capture cannot be retained at all, so the caller degrades by not
    // retaining instead of by growing host memory.
    [[nodiscard]] bool retain_state_capture(std::size_t incoming) noexcept {
        if (state_budget_bytes_ == 0) { return true; }
        if (incoming > state_budget_bytes_) { return false; }
        while (shared_occupied_bytes() + incoming > state_budget_bytes_) {
            const std::optional<std::size_t> victim = select_victim(/*allow_pinned=*/false);
            if (!victim) { return false; }
            std::fprintf(stderr,
                         "[host-state-pool] evict=%zu tier=%s ctx_pages=%zu state_bytes=%zu retained=%zu "
                         "shared=%zu budget=%zu (dead->live->idle->active)\n",
                         *victim, classify_tier(entries_[*victim], std::chrono::steady_clock::now()),
                         unit_context_pages(entries_[*victim]),
                         entry_state_bytes(entries_[*victim]),
                         state_retained_bytes_, shared_occupied_bytes(), state_budget_bytes_);
            remove(*victim);
        }
        return true;
    }

    // P2.5 make-room: the shared HostStatePool can be exhausted by SLOT COUNT
    // even when the byte budget has headroom (the pool is shared with the
    // store's demoted replicas). When a new capture cannot get a slot, evict
    // whole units under the same three-tier policy (dead-largest,
    // live-smallest, active-session-last) until `needed` slots are free.
    // Pinned entries (in-flight restores) are never evicted. Bounded per
    // call so one capture cannot drain the net. Returns the slots freed.
    [[nodiscard]] std::uint32_t make_room_for_state_slots(std::uint32_t needed) noexcept {
        std::uint32_t freed = 0;
        constexpr std::uint32_t kMaxEvictionsPerCall = 4;
        for (std::uint32_t i = 0; i < kMaxEvictionsPerCall && freed < needed; ++i) {
            const std::optional<std::size_t> victim = select_victim(/*allow_pinned=*/false);
            if (!victim) { break; }
            const HostKVSafetyNetEntry& entry = entries_[*victim];
            const std::uint32_t entry_slots =
                (entry.state_slot ? 1U : 0U) + (entry.checkpoint_state_slot ? 1U : 0U);
            std::fprintf(stderr,
                         "[host-state-pool] make-room: evict=%zu tier=%s ctx_pages=%zu state_slots=%u "
                         "(dead->live->idle->active)\n",
                         *victim, classify_tier(entry, std::chrono::steady_clock::now()),
                         unit_context_pages(entry), entry_slots);
            remove(*victim);
            freed += entry_slots;
        }
        return freed;
    }

    // P2.4 Increment 1 (spill-before-loss): is a unit with this identity still
    // retained in the net — i.e. can a future request for this unit be
    // restored from it? A frontier F is covered by an entry with the same
    // ledger prefix and identity at its execution frontier (a non-rewound
    // follow-up extends it) or its checkpoint frontier (a rewound follow-up
    // lands there); the session key covers the thinking-mode fallback (find()
    // matches by session identity when the prefix diverges). Mirrors
    // prefix_matches(): the ledger tokens AND the identity must agree — the
    // identity alone (token types/positions) cannot distinguish units with
    // the same shape and different content. Used to decide whether the store
    // images of a unit being released are its LAST restorable copies.
    [[nodiscard]] bool retains(const std::span<const TokenId>& unit_tokens,
                               const ResidentPrefixIdentity& identity,
                               const std::optional<qwen3_6::PreparedSessionKey>& session_key,
                               std::uint32_t execution_frontier,
                               std::uint32_t checkpoint_frontier) const noexcept {
        const auto covers = [&](const HostKVSafetyNetEntry& entry, std::uint32_t frontier) {
            if (frontier == 0 || entry.ledger.size() < frontier ||
                unit_tokens.size() < frontier) {
                return false;
            }
            if (!std::equal(unit_tokens.data(), unit_tokens.data() + static_cast<std::ptrdiff_t>(frontier),
                            entry.ledger.begin())) {
                return false;
            }
            return identity.prefix_equals(entry.prefix_identity, frontier);
        };
        for (const auto& entry : entries_) {
            if (session_key && entry.session_key && *session_key == *entry.session_key) {
                return true;
            }
            // find() matches an entry only at its OWN frontiers — a longer
            // entry's ledger prefix does not cover a shorter frontier (the
            // state image rides at the entry's frontier, not at every prefix).
            if (entry.execution_frontier == execution_frontier &&
                covers(entry, execution_frontier)) {
                return true;
            }
            if (entry.checkpoint_valid && entry.checkpoint_frontier == checkpoint_frontier &&
                covers(entry, checkpoint_frontier)) {
                return true;
            }
        }
        return false;
    }

    // Add a new entry. The safety net is bounded by host KV arena bytes,
    // not entry count — the spill function evicts smallest unpinned entries
    // to free arena memory before calling add(). A re-added entry (after
    // restore) already owns its arena allocation, so no eviction is needed.
    // Returns false when the entry was rejected (the entry's state-image
    // pool slots are returned to the pool via the releaser).
    [[nodiscard]] bool add(HostKVSafetyNetEntry entry) {

        // Atomicity first: KV and state are one unit, so a half unit is never stored.
        if (!is_complete_unit(entry)) {
            std::fprintf(stderr,
                         "[safety-net] REJECT-PARTIAL state_bytes=%zu kv_pages=%u/%u - a cache unit "
                         "is {KV + state}\n",
                         entry_state_bytes(entry), entry.text_page_count, entry.backend_page_count);
            if (state_slot_releaser_) { state_slot_releaser_(entry); }
            return false;
        }

        // Retention is bounded by the shared host memory budget. Completed continuations
        // stay catalogued for prefix reuse, so units would otherwise accumulate once per
        // conversation, each carrying a full state image in host memory.
        const std::size_t incoming_state_bytes = entry_state_bytes(entry);
        if (!retain_state_capture(incoming_state_bytes)) {
            std::fprintf(stderr,
                         "[host-state-pool] REJECT state_bytes=%zu retained=%zu budget=%zu\n",
                         incoming_state_bytes, state_retained_bytes_, state_budget_bytes_);
            if (state_slot_releaser_) { state_slot_releaser_(entry); }
            return false;
        }

        // Supersede: an existing entry whose effective prefix is a strict token
        // prefix of the incoming entry's is redundant — every future match for
        // it is also a match for the incoming (longer) entry, which find()
        // prefers. Without this, a growing conversation accumulates one entry
        // per eviction (30219, 30589, 31251, ...), each carrying a full state
        // image, until the arena is full of strict-prefix duplicates.
        const std::span<const TokenId> incoming_prefix =
            entry.compact_prefix.empty()
                ? std::span<const TokenId>(entry.ledger.data(), entry.ledger.size())
                : std::span<const TokenId>(entry.compact_prefix.data(), entry.compact_prefix.size());
        if (!incoming_prefix.empty()) {
            for (std::size_t i = entries_.size(); i-- > 0;) {
                HostKVSafetyNetEntry& old = entries_[i];
                if (old.pinned) { continue; }  // a restore is in flight on it
                const std::span<const TokenId> old_prefix =
                    old.compact_prefix.empty()
                        ? std::span<const TokenId>(old.ledger.data(), old.ledger.size())
                        : std::span<const TokenId>(old.compact_prefix.data(),
                                                  old.compact_prefix.size());
                if (old_prefix.size() >= incoming_prefix.size()) { continue; }
                bool is_prefix = true;
                for (std::size_t t = 0; t < old_prefix.size(); ++t) {
                    if (old_prefix[t] != incoming_prefix[t]) { is_prefix = false; break; }
                }
                if (!is_prefix) { continue; }
                std::fprintf(stderr,
                             "[safety-net] supersede: dropping frontier=%u (prefix of %zu) "
                             "state_bytes=%zu\n",
                             old.execution_frontier, incoming_prefix.size(),
                             entry_state_bytes(old));
                superseded_.fetch_add(1, std::memory_order_relaxed);
                remove(i);
            }
        }

        state_retained_bytes_ += incoming_state_bytes;
        entry.entry_id = ++next_entry_id_;

        entry.pinned = false;  // re-added entries are unpinned

        entries_.push_back(std::move(entry));
        return true;
    }




    // Take ownership of an entry, removing it from the store.

    [[nodiscard]] HostKVSafetyNetEntry take(std::size_t index) {

        if (index >= entries_.size()) {

            throw std::out_of_range("Host KV safety net index is out of range");

        }

        HostKVSafetyNetEntry out = std::move(entries_[index]);

        state_retained_bytes_ -= (entries_[index].state_bytes +
                                         entries_[index].checkpoint_state_bytes);
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));

        return out;

    }



    // Get a const reference to an entry.

    [[nodiscard]] const HostKVSafetyNetEntry& at(std::size_t index) const {

        if (index >= entries_.size()) {

            throw std::out_of_range("Host KV safety net index is out of range");

        }

        return entries_[index];

    }



    // Remove an entry (frees host KV allocations via their destructors and
    // returns its state-image pool slots through the releaser).

    void remove(std::size_t index) {

        if (index >= entries_.size()) { return; }

        evictions_.fetch_add(1, std::memory_order_relaxed);
        state_retained_bytes_ -= (entries_[index].state_bytes +
                                         entries_[index].checkpoint_state_bytes);
        if (state_slot_releaser_) { state_slot_releaser_(entries_[index]); }
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));

    }



    // Pin an entry to prevent LRU eviction during the restore pipeline.

    // The entry stays in the net (arena bytes remain counted as net entries,

    // not held hostage as a taken entry). Returns the entry's unique ID for

    // later take_pinned/unpin — the index may shift due to vector mutations

    // from spill evictions, but the ID is stable.

    [[nodiscard]] std::uint64_t pin(std::size_t index) {

        if (index >= entries_.size()) { return 0; }

        entries_[index].pinned = true;

        return entries_[index].entry_id;

    }

    void unpin(std::uint64_t id) {

        for (auto& e : entries_) {

            if (e.entry_id == id) { e.pinned = false; return; }

        }

    }



    // Remove and return a pinned entry by its stable ID (used after H2D copy

    // to re-add with refreshed timestamp). The ID survives vector mutations

    // (spill evictions shifting indices) between pin() and take_pinned().

    [[nodiscard]] HostKVSafetyNetEntry take_pinned(std::uint64_t id) {

        for (auto it = entries_.begin(); it != entries_.end(); ++it) {

            if (it->entry_id == id) {

                HostKVSafetyNetEntry out = std::move(*it);

                state_retained_bytes_ -= (it->state_bytes + it->checkpoint_state_bytes);
                entries_.erase(it);

                return out;

            }

        }

        throw std::out_of_range("Host KV safety net pinned entry ID not found");

    }



    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }



    void clear() noexcept {
        for (auto& entry : entries_) {
            if (state_slot_releaser_) { state_slot_releaser_(entry); }
        }
        entries_.clear();
        state_retained_bytes_ = 0;
    }



private:

    std::vector<HostKVSafetyNetEntry> entries_;

    std::uint64_t next_entry_id_ = 0;
    // Retained state-image bytes and their budget (0 = unbounded).
    std::size_t state_budget_bytes_    = 0;
    std::size_t state_retained_bytes_  = 0;
    // Live view of the other tenant of the shared host budget (KV pages).
    const HostKVArena* shared_arena_ = nullptr;
    std::atomic<std::uint64_t> evictions_{0};
    // Entries dropped by supersede-on-add (a newer entry made them redundant).
    std::atomic<std::uint64_t> superseded_{0};
    // An entry unmatched for longer than this is dead weight (evicted
    // largest-first). 15 minutes: a live conversation matches every turn, so
    // silence this long means the conversation is gone (compacted, abandoned).
    std::chrono::seconds dead_ttl_{15 * 60};
    std::function<bool(const std::optional<qwen3_6::PreparedSessionKey>&)> session_is_live_;
    // P2.5 Increment 3: predicate marking a session as ACTIVELY being served
    // (its continuation is in the Active role, not merely Catalogued/retained).
    // session_is_live_ is Active OR Catalogued; session_is_active_ is Active
    // only. The split lets eviction reap idle (Catalogued) "old copy" units
    // before the actively-serving session's unit, which the lumped live
    // predicate could not distinguish.
    std::function<bool(const std::optional<qwen3_6::PreparedSessionKey>&)> session_is_active_;
    // P2.4 Increment 2: returns a dropped entry's state-image pool slots to the
    // HostStatePool the program owns. Invoked by remove() only — take() /
    // take_pinned() transfer the slots with the entry (the program releases
    // them after the restore consumes them).
    std::function<void(const HostKVSafetyNetEntry&)> state_slot_releaser_;

};



} // namespace ninfer::targets::qwen3_6::detail
