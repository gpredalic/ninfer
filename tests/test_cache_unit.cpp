// P2.2 Slice 1: unit identity + cost model.
//
// Pins the #7 unit invariant at the type level: a cache unit costs exactly
// kv_bytes + state_image_bytes, the cost is residency-independent, and the
// occupancy meter accounts whole units (sum of parts, exact add/remove).

#include "targets/qwen3_6/impl/runtime/cache_unit.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

using ninfer::targets::qwen3_6::detail::CacheUnitCost;
using ninfer::targets::qwen3_6::detail::CacheUnitOccupancy;

int failures = 0;

void expect(bool condition, const char* message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

template <class Function>
void expect_throw(Function&& function, const char* message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    expect(false, message);
}

// (a) The unit cost is exactly the sum of its parts.
void test_cost_is_sum_of_parts() {
    const CacheUnitCost unit{.kv_bytes = 1'000'000, .state_image_bytes = 147'000'000};
    expect(unit.total() == 148'000'000, "unit cost is not kv + state");
    const CacheUnitCost kv_only{.kv_bytes = 512, .state_image_bytes = 0};
    expect(kv_only.total() == 512, "state-less unit cost is wrong");
    const CacheUnitCost state_only{.kv_bytes = 0, .state_image_bytes = 64};
    expect(state_only.total() == 64, "kv-less unit cost is wrong");
    const CacheUnitCost empty{};
    expect(empty.total() == 0, "empty unit cost is not zero");
}

// The KV half is (text + backend) pages x stride; a unit without backend KV
// passes backend_pages = 0.
void test_kv_bytes_arithmetic() {
    using namespace ninfer::targets::qwen3_6::detail;
    expect(cache_unit_kv_bytes(100, 0, 1024) == 102'400, "text-only KV bytes wrong");
    expect(cache_unit_kv_bytes(100, 50, 1024) == 153'600, "text+backend KV bytes wrong");
    expect_throw([&] { cache_unit_kv_bytes(1, 0, 0); }, "zero page stride must throw");
    expect_throw(
        [&] { cache_unit_kv_bytes(std::numeric_limits<std::uint64_t>::max(), 1, 2); },
        "page overflow must throw");
}

// (d) Residency independence: the cost takes plain byte counts, so the device
// form and the host form of the same unit report the same cost by
// construction. Assert it explicitly — this is the invariant the shared meter
// relies on.
void test_cost_is_residency_independent() {
    using namespace ninfer::targets::qwen3_6::detail;
    const std::uint64_t text_pages    = 1'234;
    const std::uint64_t backend_pages = 567;
    const std::uint64_t stride        = 2'048;
    const std::uint64_t state_bytes   = 147'000'000;
    const CacheUnitCost device_form{.kv_bytes = cache_unit_kv_bytes(text_pages, backend_pages,
                                                                    stride),
                                    .state_image_bytes = state_bytes};
    const CacheUnitCost host_form{.kv_bytes = cache_unit_kv_bytes(text_pages, backend_pages,
                                                                 stride),
                                  .state_image_bytes = state_bytes};
    expect(device_form == host_form, "device and host forms of a unit differ in cost");
    expect(device_form.total() == host_form.total(), "device and host totals differ");
}

// (b) The meter's occupancy is the sum of the costs of the retained units.
void test_meter_is_sum_of_parts() {
    using namespace ninfer::targets::qwen3_6::detail;
    CacheUnitOccupancy meter;
    expect(meter.empty() && meter.occupied_bytes() == 0 && meter.unit_count() == 0,
           "fresh meter is not empty");
    const std::vector<CacheUnitCost> units{
        {.kv_bytes = 100, .state_image_bytes = 10},
        {.kv_bytes = 200, .state_image_bytes = 20},
        {.kv_bytes = 0, .state_image_bytes = 30},
    };
    for (const CacheUnitCost& unit : units) { meter.add(unit); }
    expect(meter.unit_count() == 3, "meter unit count wrong");
    expect(meter.occupied_bytes() == 100 + 10 + 200 + 20 + 0 + 30,
           "meter occupancy is not the sum of unit costs");
}

// (c) Adding and then removing a unit changes the meter by exactly its cost.
void test_meter_add_remove_delta() {
    using namespace ninfer::targets::qwen3_6::detail;
    CacheUnitOccupancy meter;
    const CacheUnitCost base{.kv_bytes = 1'000, .state_image_bytes = 100};
    const CacheUnitCost unit{.kv_bytes = 4'096, .state_image_bytes = 147'000'000};
    meter.add(base);
    meter.add(unit);
    expect(meter.occupied_bytes() == base.total() + unit.total(), "meter before remove wrong");
    meter.remove(unit);
    expect(meter.occupied_bytes() == base.total(), "remove did not subtract exactly the cost");
    expect(meter.unit_count() == 1, "remove did not decrement the unit count");
    meter.remove(base);
    expect(meter.empty(), "meter is not empty after removing all units");
}

// Removing more than is resident is a genuine accounting bug and must throw.
void test_meter_rejects_underflow() {
    using namespace ninfer::targets::qwen3_6::detail;
    CacheUnitOccupancy meter;
    const CacheUnitCost unit{.kv_bytes = 100, .state_image_bytes = 10};
    expect_throw([&] { meter.remove(unit); }, "removing from an empty meter must throw");
    meter.add(unit);
    expect_throw([&] { meter.remove(unit); meter.remove(unit); },
                 "double-remove must throw");
}

// The cost total is overflow-checked, not wrapped.
void test_cost_overflow() {
    using namespace ninfer::targets::qwen3_6::detail;
    const CacheUnitCost overflow{.kv_bytes = std::numeric_limits<std::uint64_t>::max(),
                                 .state_image_bytes = 1};
    expect_throw([&] { overflow.total(); }, "cost overflow must throw");
    CacheUnitOccupancy meter;
    const CacheUnitCost huge{.kv_bytes = std::numeric_limits<std::uint64_t>::max() / 2,
                             .state_image_bytes = std::numeric_limits<std::uint64_t>::max() / 2};
    meter.add(huge);
    expect_throw([&] { meter.add(huge); }, "occupancy overflow must throw");
}

} // namespace

int main() {
    test_cost_is_sum_of_parts();
    test_kv_bytes_arithmetic();
    test_cost_is_residency_independent();
    test_meter_is_sum_of_parts();
    test_meter_add_remove_delta();
    test_meter_rejects_underflow();
    test_cost_overflow();
    if (failures != 0) { return 1; }
    std::cout << "ok\n";
    return 0;
}
