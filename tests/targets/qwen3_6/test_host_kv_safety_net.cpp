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
#include <vector>

using namespace ninfer::targets::qwen3_6::detail;
using ninfer::TokenId;

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

}  // namespace

int main() {
    test_supersede_on_add();
    test_liveness_eviction();
    test_select_victim_directly();
    if (failures == 0) {
        std::fprintf(stderr, "PASS: host_kv_safety_net lifecycle\n");
        return 0;
    }
    std::fprintf(stderr, "FAIL: %d check(s)\n", failures);
    return 1;
}
