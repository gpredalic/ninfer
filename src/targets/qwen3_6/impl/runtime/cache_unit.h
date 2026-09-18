#pragma once

// The cache unit: a continuation's {KV replica, frontier checkpoint state
// image, prefix identity}, treated as ONE indivisible object for budgeting,
// eviction, and restoration (the #7 unit invariant).
//
// This header is Slice 1 of #7 (P2.2): unit identity + cost model. It defines
// a single residency-independent cost per unit and an occupancy meter that
// accounts whole units. It owns no storage, no eviction policy, and no
// reference counting — the pools that hold units (P2.4/P2.5) adopt the meter.
//
// Residency independence is the point: a unit's cost is the same whether its
// KV pages and state image live on device or on host, so the two host pools
// (KV arena + state pool) can be metered as one number.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ninfer::targets::qwen3_6::detail {

// The cost of one cache unit: its KV replica bytes plus its state image
// bytes. Both halves are required — a unit is never budgeted as KV alone or
// state alone (that is the half-unit violation this model exists to kill).
struct CacheUnitCost {
    std::uint64_t kv_bytes          = 0;
    std::uint64_t state_image_bytes = 0;

    [[nodiscard]] std::uint64_t total() const {
        if (kv_bytes > std::numeric_limits<std::uint64_t>::max() - state_image_bytes) {
            throw std::overflow_error("cache unit cost overflow");
        }
        return kv_bytes + state_image_bytes;
    }

    [[nodiscard]] friend bool operator==(const CacheUnitCost&, const CacheUnitCost&) noexcept =
        default;
};

// The KV half of a unit's cost: (text pages + backend pages) x page stride.
// A unit with no backend KV (non-speculative) passes backend_pages = 0.
[[nodiscard]] inline std::uint64_t cache_unit_kv_bytes(std::uint64_t text_pages,
                                                       std::uint64_t backend_pages,
                                                       std::uint64_t page_stride) {
    if (page_stride == 0) { throw std::invalid_argument("cache unit page stride is zero"); }
    if (text_pages > std::numeric_limits<std::uint64_t>::max() - backend_pages) {
        throw std::overflow_error("cache unit page count overflow");
    }
    const std::uint64_t pages = text_pages + backend_pages;
    if (pages > std::numeric_limits<std::uint64_t>::max() / page_stride) {
        throw std::overflow_error("cache unit KV byte overflow");
    }
    return pages * page_stride;
}

// One shared meter over host unit occupancy: the sum of the costs of the
// units currently retained on host. The meter is accounting only — it does
// not evict, allocate, or reference anything. Pools adopt it by adding a
// unit's cost when the unit lands on host and removing it when the unit
// leaves (eviction, restore, supersede).
class CacheUnitOccupancy {
public:
    void add(const CacheUnitCost& cost) {
        const std::uint64_t total = cost.total();
        if (occupied_bytes_ > std::numeric_limits<std::uint64_t>::max() - total) {
            throw std::overflow_error("cache unit occupancy overflow");
        }
        occupied_bytes_ += total;
        ++unit_count_;
    }

    // Removes exactly one previously-added unit. Removing more than is
    // resident is a genuine accounting bug (double-remove or a unit that was
    // never added), so it throws rather than silently underflowing.
    void remove(const CacheUnitCost& cost) {
        const std::uint64_t total = cost.total();
        if (total > occupied_bytes_) {
            throw std::logic_error("cache unit occupancy underflow");
        }
        occupied_bytes_ -= total;
        if (unit_count_ == 0) { throw std::logic_error("cache unit occupancy underflow"); }
        --unit_count_;
    }

    [[nodiscard]] std::uint64_t occupied_bytes() const noexcept { return occupied_bytes_; }
    [[nodiscard]] std::uint64_t unit_count() const noexcept { return unit_count_; }
    [[nodiscard]] bool empty() const noexcept { return unit_count_ == 0; }

private:
    std::uint64_t occupied_bytes_ = 0;
    std::uint64_t unit_count_     = 0;
};

} // namespace ninfer::targets::qwen3_6::detail
