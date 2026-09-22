// Two-device qualification suite for the tp2 split mechanics in
// src/ops/common/split_launch.h: the per-rank device/stream discipline
// (for_each_rank), the context guard (require_split_context), and the
// current-device save/restore (CurrentDeviceScope).
//
// Every case REQUIRES two CUDA devices driven from ONE process; with fewer
// than two visible devices the suite reports the repository's skip code (77)
// instead of failing, like tests/ops/test_allreduce.cpp.
//
// WHAT IS CHECKED. The split mechanics are the substrate every tensor-parallel
// Op form runs on, so this suite verifies the substrate directly, without
// depending on any model Op family:
//   * require_split_context rejects a tp == 1 context and accepts a tp == 2 one;
//   * for_each_rank runs each rank's body with that rank's device current;
//   * a buffer allocated inside a rank's body is resident on that rank's device;
//   * for_each_rank restores the caller's current device when it returns.
#include "ops/common/split_launch.h"
#include "ops/op_tester.h"

#include "core/device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>

using namespace ninfer;
using namespace ninfer::test;
namespace detail = ninfer::ops::detail;

namespace {

// require_split_context must reject a single-device context and accept a two-device one.
int run_context_guard_case(const ExecutionContext& ec) {
    int failures = 0;

    const ExecutionContext single({0}); // tp == 1
    bool rejected_single = false;
    try {
        detail::require_split_context(single, "test: tp==1 must be rejected");
    } catch (const std::invalid_argument&) {
        rejected_single = true;
    }
    if (!rejected_single) {
        std::cerr << "split_ops: require_split_context accepted a tp==1 context\n";
        ++failures;
    }

    try {
        detail::require_split_context(ec, "test: tp==2 must be accepted");
    } catch (const std::exception& e) {
        std::cerr << "split_ops: require_split_context rejected a valid tp==2 context: "
                  << e.what() << '\n';
        ++failures;
    }
    return failures;
}

// for_each_rank must self-validate: a tp == 1 (or otherwise malformed) context is rejected with
// std::invalid_argument -- the intended diagnostic -- rather than bad_optional_access from
// dereferencing an empty optional. A valid tp == 2 context is accepted.
int run_for_each_rank_validation_case(const ExecutionContext& ec) {
    int failures = 0;

    const ExecutionContext single({0}); // tp == 1
    bool rejected_single = false;
    try {
        detail::for_each_rank(single, [](int) {});
    } catch (const std::invalid_argument&) {
        rejected_single = true;
    } catch (const std::bad_optional_access&) {
        std::cerr << "split_ops: for_each_rank threw bad_optional_access instead of "
                     "invalid_argument for a tp==1 context\n";
        ++failures;
    }
    if (!rejected_single) {
        std::cerr << "split_ops: for_each_rank accepted a tp==1 context\n";
        ++failures;
    }

    try {
        detail::for_each_rank(ec, [](int) {});
    } catch (const std::exception& e) {
        std::cerr << "split_ops: for_each_rank rejected a valid tp==2 context: " << e.what() << '\n';
        ++failures;
    }
    return failures;
}

// for_each_rank must run each rank's body with that rank's device current.
int run_device_selection_case(const ExecutionContext& ec) {
    int failures = 0;
    std::array<int, 2> observed{-1, -1};
    detail::for_each_rank(ec, [&](int rank) {
        int current = -1;
        CUDA_CHECK(cudaGetDevice(&current));
        observed[rank] = current;
    });
    for (int rank = 0; rank < 2; ++rank) {
        if (observed[rank] != ec.dev[rank]->device) {
            std::cerr << "split_ops: for_each_rank rank " << rank << " ran on device "
                      << observed[rank] << ", expected " << ec.dev[rank]->device << '\n';
            ++failures;
        }
    }
    return failures;
}

// A buffer allocated inside a rank's body must be resident on that rank's device: this is the
// whole point of the per-rank device discipline, and the mistake a single-device caller makes
// when it assumes one current device for the whole process.
int run_rank_residency_case(const ExecutionContext& ec) {
    int failures = 0;
    constexpr std::size_t kBytes = 1024;
    std::array<void*, 2> buffers{nullptr, nullptr};
    detail::for_each_rank(ec, [&](int rank) {
        CUDA_CHECK(cudaMalloc(&buffers[rank], kBytes));
    });
    for (int rank = 0; rank < 2; ++rank) {
        cudaPointerAttributes attrs{};
        CUDA_CHECK(cudaPointerGetAttributes(&attrs, buffers[rank]));
        if (attrs.type != cudaMemoryTypeDevice || attrs.device != ec.dev[rank]->device) {
            std::cerr << "split_ops: rank " << rank << " buffer is not resident on its device "
                      << "(type=" << attrs.type << ", device=" << attrs.device << ", expected "
                      << ec.dev[rank]->device << ")\n";
            ++failures;
        }
        CUDA_CHECK(cudaSetDevice(ec.dev[rank]->device));
        CUDA_CHECK(cudaFree(buffers[rank]));
        buffers[rank] = nullptr;
    }
    return failures;
}

// for_each_rank must leave the caller's current device unchanged.
int run_device_restore_case(const ExecutionContext& ec) {
    int failures = 0;
    // Start from rank 0's device (the ExecutionContext postcondition), run the split, and require
    // the same device current afterwards.
    CUDA_CHECK(cudaSetDevice(ec.dev[0]->device));
    int before = -1;
    CUDA_CHECK(cudaGetDevice(&before));
    detail::for_each_rank(ec, [](int) {
        // no-op body; the point is the device save/restore around it
    });
    int after = -1;
    CUDA_CHECK(cudaGetDevice(&after));
    if (before != after) {
        std::cerr << "split_ops: for_each_rank did not restore the current device (" << before
                  << " -> " << after << ")\n";
        ++failures;
    }
    return failures;
}

} // namespace

int main() {
    if (cuda_unavailable()) {
        std::cout << "SKIP: no usable CUDA device\n";
        return 77;
    }
    int device_count = 0;
    cuda_check(cudaGetDeviceCount(&device_count), "cudaGetDeviceCount");
    if (device_count < 2) {
        std::cout << "SKIP: split mechanics require two CUDA devices, found " << device_count
                  << '\n';
        return 77;
    }

    const ExecutionContext ec({0, 1});
    int failures = 0;
    failures += run_context_guard_case(ec);
    failures += run_for_each_rank_validation_case(ec);
    failures += run_device_selection_case(ec);
    failures += run_rank_residency_case(ec);
    failures += run_device_restore_case(ec);
    std::cout << (failures ? "FAIL" : "OK") << " split_ops\n";
    return failures ? 1 : 0;
}