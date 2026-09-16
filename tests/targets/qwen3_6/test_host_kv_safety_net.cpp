// Host-KV safety-net lifecycle: supersede-on-add and liveness-based eviction.
//
// Regression context (2026-09-13): a single Claude Code session filled the 30 GiB
// arena to 99.8% because (a) every eviction of a growing conversation added a new
// entry while all its strict-prefix ancestors stayed (30219, 30589, 31251, ...),
// and (b) a client-compacted conversation left 324K-token entries that nothing
// ever matched again — and smallest-first eviction protected them as the largest
// units. These tests pin both fixes:
//   1. a growing lineage leaves exactly ONE entry (supersede-on-add),
//   2. dead entries (never matched / unmatched beyond the TTL) are evicted
//      largest-first, before any live entry (liveness-based eviction).

#include "targets/qwen3_6/impl/runtime/host_kv_safety_net.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

using namespace ninfer::targets::qwen3_6::detail;
using ninfer::TokenId;
using ninfer::targets::qwen3_6::PreparedSessionKey;

namespace {

int failures = 0;

void check(bool ok, const char* label) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++failures;
    }
}

// Build a complete unit (KV pages + state image) with a distinct token ledger
// starting at `token_base`, so entries from different "conversations" never
// prefix-match each other.
HostKVSafetyNetEntry make_entry(std::size_t tokens, std::uint32_t token_base,
                                std::size_t state_bytes) {
    HostKVSafetyNetEntry entry;
    entry.ledger.resize(tokens);
    for (std::size_t i = 0; i < tokens; ++i) {
        entry.ledger[i] = static_cast<TokenId>(token_base + i);
    }
    entry.execution_frontier = static_cast<std::uint32_t>(tokens);
    entry.text_page_count    = static_cast<std::uint32_t>(tokens);
    entry.state_host.assign(state_bytes, std::byte{1});
    entry.state_bytes        = state_bytes;
    return entry;
}

// A growing conversation: the same token stream, longer each turn.
HostKVSafetyNetEntry lineage_entry(std::size_t tokens, std::size_t state_bytes) {
    return make_entry(tokens, /*token_base=*/0, state_bytes);
}

void test_supersede_on_add() {
    HostKVSafetyNet net;
    const std::size_t sb = 1024;

    net.add(lineage_entry(30000, sb));
    check(net.size() == 1, "first entry retained");

    net.add(lineage_entry(31000, sb));
    check(net.size() == 1, "31k superseded 30k");
    check(net.superseded_count() == 1, "one supersede counted");

    net.add(lineage_entry(32000, sb));
    check(net.size() == 1, "32k superseded 31k");
    check(net.superseded_count() == 2, "two supersedes counted");
    check(net.at(0).execution_frontier == 32000, "survivor is the newest entry");

    // A different conversation (disjoint tokens) does NOT supersede.
    net.add(make_entry(5000, /*token_base=*/1000000, sb));
    check(net.size() == 2, "disjoint conversation is a separate entry");
    check(net.superseded_count() == 2, "no spurious supersede");
}

void test_liveness_eviction() {
    HostKVSafetyNet net;
    const std::size_t sb = 1024;
    net.set_dead_ttl(std::chrono::minutes(15));
    // Budget fits exactly three state images: the fourth add must evict one.
    net.set_state_budget_bytes(3 * sb);

    const auto now = std::chrono::steady_clock::now();

    // A: 40k pages, matched an hour ago — DEAD (beyond the 15 min TTL).
    auto a = make_entry(40000, 0, sb);
    a.ever_matched = true;
    a.last_matched = now - std::chrono::hours(1);
    net.add(std::move(a));

    // B: 5k pages, matched just now — LIVE.
    auto b = make_entry(5000, 1000000, sb);
    b.ever_matched = true;
    b.last_matched = now;
    net.add(std::move(b));

    // C: 8k pages, never matched — DEAD (the conversation died before a hit).
    auto c = make_entry(8000, 2000000, sb);
    net.add(std::move(c));

    check(net.size() == 3, "three entries fit the budget");

    // D: 5k pages, live. Its add must evict exactly one entry — the LARGEST
    // dead one (A, 40k), not the smallest live one (B, 5k).
    auto d = make_entry(5000, 3000000, sb);
    d.ever_matched = true;
    d.last_matched = now;
    net.add(std::move(d));

    check(net.size() == 3, "one entry evicted to fit");
    bool saw_a = false, saw_b = false, saw_c = false, saw_d = false;
    for (std::size_t i = 0; i < net.size(); ++i) {
        const std::size_t pages = net.at(i).text_page_count;
        if (pages == 40000) { saw_a = true; }
        if (pages == 5000) {
            saw_b = saw_b || net.at(i).ledger[0] == 1000000;
            saw_d = saw_d || net.at(i).ledger[0] == 3000000;
        }
        if (pages == 8000) { saw_c = true; }
    }
    check(!saw_a, "dead giant evicted first (largest dead)");
    check(saw_b && saw_c && saw_d, "live and never-matched-but-small entries kept");
    check(net.eviction_count() >= 1, "eviction counted");
}

void test_select_victim_directly() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<HostKVSafetyNetEntry> entries;

    auto dead_big = make_entry(40000, 0, 1024);
    dead_big.ever_matched = true;
    dead_big.last_matched = now - std::chrono::hours(2);
    entries.push_back(std::move(dead_big));

    auto live_small = make_entry(5000, 1000000, 1024);
    live_small.ever_matched = true;
    live_small.last_matched = now;
    entries.push_back(std::move(live_small));

    auto live_medium = make_entry(8000, 2000000, 1024);
    live_medium.ever_matched = true;
    live_medium.last_matched = now;
    entries.push_back(std::move(live_medium));

    // Dead tier wins: the largest dead entry (index 0) beats the smallest live.
    auto victim = HostKVSafetyNet::select_eviction_victim(entries, now, std::chrono::minutes(15),
                                                          /*allow_pinned=*/false);
    check(victim.has_value() && *victim == 0, "largest dead entry is the victim");

    // With no dead entries, the smallest live entry goes (index 1, 5k pages).
    entries[0].last_matched = now;  // make it live
    victim = HostKVSafetyNet::select_eviction_victim(entries, now, std::chrono::minutes(15),
                                                     /*allow_pinned=*/false);
    check(victim.has_value() && *victim == 1, "smallest live entry is the victim");

    // Pinned entries are skipped in phase 1.
    entries[1].pinned = true;
    victim = HostKVSafetyNet::select_eviction_victim(entries, now, std::chrono::minutes(15),
                                                     /*allow_pinned=*/false);
    check(victim.has_value() && *victim == 2, "pinned entry skipped in phase 1");
    victim = HostKVSafetyNet::select_eviction_victim(entries, now, std::chrono::minutes(15),
                                                     /*allow_pinned=*/true);
    check(victim.has_value() && *victim == 1, "pinned entry eligible in phase 2");
}

// P2.5 per-session guarantee: a unit whose session is still active (its
// continuation exists) is evicted only after every dead and idle-session unit
// is gone. Protection is ordering only — the protected tier is exhausted
// before selection gives up.
void test_session_protection() {
    const auto now = std::chrono::steady_clock::now();
    const std::size_t sb = 1024;

    auto make_key = [](const char* name) {
        PreparedSessionKey key;
        const std::size_t n = std::strlen(name);
        std::memcpy(key.bytes.data(), name, n);
        key.size = static_cast<std::uint16_t>(n);
        return key;
    };
    const PreparedSessionKey key_live = make_key("live-session");
    const PreparedSessionKey key_idle = make_key("idle-session");

    // Only `live-session` is active; every other key (and no key at all) is idle.
    const auto is_live = [&key_live](const std::optional<PreparedSessionKey>& k) {
        return k.has_value() && *k == key_live;
    };

    // Direct selection: the protected tier is exhausted last.
    {
        auto big_unkeyed = make_entry(9000, 0, sb);  // live, no session key
        big_unkeyed.ever_matched = true;
        big_unkeyed.last_matched = now;
        auto small_live = make_entry(1000, 1000000, sb);  // live, active session
        small_live.ever_matched = true;
        small_live.last_matched = now;
        small_live.session_key = key_live;
        std::vector<HostKVSafetyNetEntry> entries;
        entries.push_back(std::move(big_unkeyed));
        entries.push_back(std::move(small_live));

        auto victim = HostKVSafetyNet::select_eviction_victim(entries, now, std::chrono::minutes(15),
                                                             /*allow_pinned=*/false, is_live,
                                                             /*protect_live_sessions=*/true);
        check(victim.has_value() && *victim == 0,
              "unprotected live evicted before a smaller protected unit");

        // Nothing unprotected left: the protected unit is the last resort.
        entries.erase(entries.begin());
        victim = HostKVSafetyNet::select_eviction_victim(entries, now, std::chrono::minutes(15),
                                                         /*allow_pinned=*/false, is_live,
                                                         /*protect_live_sessions=*/true);
        check(victim.has_value() && *victim == 0,
              "protected unit evicted only when nothing else is");
    }

    // The dead tier still precedes protection: an unmatched-past-TTL unit of a
    // live session is reaped largest-first like any dead unit.
    {
        auto dead_live = make_entry(40000, 5000000, sb);
        dead_live.ever_matched = true;
        dead_live.last_matched = now - std::chrono::hours(2);
        dead_live.session_key = key_live;
        auto live_idle = make_entry(5000, 6000000, sb);
        live_idle.ever_matched = true;
        live_idle.last_matched = now;
        std::vector<HostKVSafetyNetEntry> entries;
        entries.push_back(std::move(dead_live));
        entries.push_back(std::move(live_idle));
        auto victim = HostKVSafetyNet::select_eviction_victim(entries, now, std::chrono::minutes(15),
                                                             /*allow_pinned=*/false, is_live,
                                                             /*protect_live_sessions=*/true);
        check(victim.has_value() && *victim == 0,
              "dead tier precedes session protection (TTL reaping intact)");
    }

    // State-pool loop: an active session's unit survives while idle units exist.
    {
        HostKVSafetyNet net;
        net.set_dead_ttl(std::chrono::minutes(15));
        net.set_state_budget_bytes(2 * sb);
        net.set_session_is_live(is_live);

        auto p = make_entry(10000, 0, sb);  // the active session's unit
        p.ever_matched = true;
        p.last_matched = now;
        p.session_key = key_live;
        net.add(std::move(p));

        auto q = make_entry(5000, 1000000, sb);  // an idle session's unit
        q.ever_matched = true;
        q.last_matched = now;
        q.session_key = key_idle;
        net.add(std::move(q));

        // Two idle adds, each evicting one entry. Both must evict the idle
        // unit (or its idle replacement), never the active session's unit.
        for (std::uint32_t round = 0; round < 2; ++round) {
            auto r = make_entry(7000 + round * 1000, 2000000 + round * 1000000, sb);
            r.ever_matched = true;
            r.last_matched = now;
            r.session_key = key_idle;
            net.add(std::move(r));
            check(net.size() == 2, "pool holds two units at 2x budget");
            bool saw_p = false;
            for (std::size_t i = 0; i < net.size(); ++i) {
                const auto& e = net.at(i);
                if (e.session_key && *e.session_key == key_live) { saw_p = true; }
            }
            check(saw_p, "active session's unit survived an idle eviction");
        }
    }

    // Exhaustion: when the pool holds only the active session's unit, an
    // incoming idle capture evicts it (last resort) rather than being dropped.
    {
        HostKVSafetyNet net;
        net.set_dead_ttl(std::chrono::minutes(15));
        net.set_state_budget_bytes(sb);
        net.set_session_is_live(is_live);

        auto p = make_entry(10000, 0, sb);
        p.ever_matched = true;
        p.last_matched = now;
        p.session_key = key_live;
        net.add(std::move(p));
        check(net.size() == 1, "active unit fits a 1x budget");

        auto r = make_entry(4000, 1000000, sb);
        r.ever_matched = true;
        r.last_matched = now;
        r.session_key = key_idle;
        net.add(std::move(r));
        check(net.size() == 1, "incoming idle unit displaced the active unit");
        bool saw_p = false;
        for (std::size_t i = 0; i < net.size(); ++i) {
            const auto& e = net.at(i);
            if (e.session_key && *e.session_key == key_live) { saw_p = true; }
        }
        check(!saw_p, "protected unit evicted only when nothing else was");
    }
}

// A ResidentPrefixIdentity over `tokens` distinct token IDs (identity
// metadata as assign() requires: token types + 3 position axes).
ResidentPrefixIdentity make_identity(std::size_t tokens, std::uint32_t token_base) {
    ninfer::targets::qwen3_6::PreparedPromptData prompt;
    prompt.token_ids.resize(tokens);
    prompt.token_types.resize(tokens);
    prompt.positions.resize(3 * tokens);
    for (std::size_t i = 0; i < tokens; ++i) {
        prompt.token_ids[i]   = static_cast<TokenId>(token_base + i);
        prompt.token_types[i] = 0;
        prompt.positions[i]           = static_cast<std::int32_t>(i);
        prompt.positions[tokens + i]   = static_cast<std::int32_t>(i);
        prompt.positions[2 * tokens + i] = static_cast<std::int32_t>(i);
    }
    ResidentPrefixIdentity identity;
    identity.assign(prompt);
    return identity;
}

void test_retains() {
    HostKVSafetyNet net;
    const auto unit_a = make_identity(1000, 0);
    std::vector<TokenId> tokens_a(1000);
    for (std::size_t i = 0; i < tokens_a.size(); ++i) { tokens_a[i] = static_cast<TokenId>(i); }
    const std::span<const TokenId> unit_a_tokens(tokens_a);
    auto entry        = lineage_entry(1000, 1024);
    entry.prefix_identity    = unit_a;
    entry.checkpoint_valid   = true;
    entry.checkpoint_frontier = 800;
    net.add(std::move(entry));

    check(net.retains(unit_a_tokens, unit_a, {}, 1000, 800),
          "unit retained at its execution frontier");
    check(net.retains(unit_a_tokens, unit_a, {}, 0, 800),
          "unit retained at its checkpoint frontier");
    check(!net.retains(unit_a_tokens, unit_a, {}, 1200, 0), "no entry at a longer frontier");
    check(!net.retains(unit_a_tokens, unit_a, {}, 0, 500),
          "no entry at a shorter checkpoint frontier");
    // Same identity shape (token types/positions), different token values:
    // the ledger comparison must distinguish the units.
    std::vector<TokenId> tokens_b(1000);
    for (std::size_t i = 0; i < tokens_b.size(); ++i) {
        tokens_b[i] = static_cast<TokenId>(1000000 + i);
    }
    const std::span<const TokenId> unit_b_tokens(tokens_b);
    check(!net.retains(unit_b_tokens, make_identity(1000, 1000000), {}, 1000, 800),
          "a different unit's tokens are not retained");

    // Session key covers the thinking-mode fallback path (prefix matching
    // fails there; find() matches by session identity instead). A distinct
    // conversation with the key set: retains() must match on the key alone.
    PreparedSessionKey key;
    key.size = 4;
    key.bytes[0] = 'a';
    key.bytes[1] = 'b';
    key.bytes[2] = 'c';
    key.bytes[3] = 'd';
    auto keyed = make_entry(5000, /*token_base=*/2000000, 1024);
    keyed.prefix_identity = make_identity(5000, 2000000);
    keyed.session_key     = key;
    net.add(std::move(keyed));
    check(net.retains(unit_a_tokens, make_identity(1200, 0), key, 1200, 0),
          "session key retains the unit even at a frontier with no entry");
    check(!net.retains(unit_a_tokens, make_identity(1200, 0), {}, 1200, 0),
          "without the session key the same frontier is not retained");

    // Supersede: a longer entry of the same lineage replaces the shorter one —
    // the shorter frontier is no longer covered (a future request for this
    // unit extends the NEW frontier, or rewinds to the new checkpoint).
    std::vector<TokenId> tokens_long(2000);
    for (std::size_t i = 0; i < tokens_long.size(); ++i) {
        tokens_long[i] = static_cast<TokenId>(i);
    }
    const std::span<const TokenId> unit_long_tokens(tokens_long);
    auto longer = lineage_entry(2000, 1024);
    longer.prefix_identity = make_identity(2000, 0);
    net.add(std::move(longer));
    check(net.size() == 2, "longer entry superseded the shorter (keyed entry untouched)");
    check(!net.retains(unit_a_tokens, unit_a, {}, 1000, 800),
          "superseded frontier is no longer covered by the net");
    check(net.retains(unit_long_tokens, make_identity(2000, 0), {}, 2000, 0),
          "the new frontier is covered");
}

}  // namespace

int main() {
    test_supersede_on_add();
    test_liveness_eviction();
    test_select_victim_directly();
    test_session_protection();
    test_retains();
    if (failures == 0) {
        std::fprintf(stderr, "PASS: host_kv_safety_net lifecycle\n");
        return 0;
    }
    std::fprintf(stderr, "FAIL: %d check(s)\n", failures);
    return 1;
}
