#include "ops/gdn_gating_proj/bf16/bf16_gdn_gating_proj_plan.h"

#include "ninfer/ops/rmsnorm.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

namespace ninfer::ops::detail {
namespace {

inline constexpr std::int32_t kAnyCols = std::numeric_limits<std::int32_t>::max();

struct ColsSet {
    std::int32_t first;
    std::int32_t last;

    constexpr bool contains(std::int32_t cols) const noexcept {
        return cols >= first && cols <= last;
    }
};

struct RouteSpec {
    ColsSet cols;
    Bf16GdnGatingScheduleId schedule;
};

constexpr std::array<RouteSpec, 6> k27Routes{{
    {{1, 1}, Bf16GdnGatingScheduleId::GemvPairedRows},
    {{2, 8}, Bf16GdnGatingScheduleId::SmallTSplit10},
    // As token tiles double, halve SplitK. This keeps the cooperative grid near 192 CTAs instead
    // of making T a launch limit. Once the unsplit grid has enough independent work, it also
    // removes the cooperative-residency constraint.
    {{9, 1024}, Bf16GdnGatingScheduleId::MmaCooperativeSplit8},
    {{1025, 2048}, Bf16GdnGatingScheduleId::MmaCooperativeSplit4},
    {{2049, 4096}, Bf16GdnGatingScheduleId::MmaCooperativeSplit2},
    {{4097, kAnyCols}, Bf16GdnGatingScheduleId::MmaUnsplit},
}};

constexpr std::array<RouteSpec, 5> k35Routes{{
    // The same progression keeps the long-range cooperative routes near 256 CTAs.
    {{1, 127}, Bf16GdnGatingScheduleId::MmaCooperativeSplit16},
    {{128, 1024}, Bf16GdnGatingScheduleId::MmaCooperativeSplit8},
    {{1025, 2048}, Bf16GdnGatingScheduleId::MmaCooperativeSplit4},
    {{2049, 4096}, Bf16GdnGatingScheduleId::MmaCooperativeSplit2},
    {{4097, kAnyCols}, Bf16GdnGatingScheduleId::MmaUnsplit},
}};

template <std::size_t N>
constexpr bool catalog_is_closed(const std::array<RouteSpec, N>& routes,
                                 std::int32_t last) noexcept {
    std::int64_t expected = 1;
    for (const RouteSpec& route : routes) {
        if (route.cols.first != expected || route.cols.last < route.cols.first) { return false; }
        expected = static_cast<std::int64_t>(route.cols.last) + 1;
    }
    return routes.back().cols.last == last && expected == static_cast<std::int64_t>(last) + 1;
}

static_assert(catalog_is_closed(k27Routes, kAnyCols));
static_assert(catalog_is_closed(k35Routes, kAnyCols));

bool is_27(const Bf16GdnGatingProblem& problem) noexcept {
    return problem.heads == 48 && problem.input_rows == 5120;
}

bool is_35(const Bf16GdnGatingProblem& problem) noexcept {
    return problem.heads == 32 && problem.input_rows == 2048;
}

bool schedule_uses_mma(Bf16GdnGatingScheduleId schedule) noexcept {
    switch (schedule) {
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
    case Bf16GdnGatingScheduleId::MmaUnsplit:
        return true;
    case Bf16GdnGatingScheduleId::GemvPairedRows:
    case Bf16GdnGatingScheduleId::SmallTSplit10:
    case Bf16GdnGatingScheduleId::SimtWarpRowC4:
    case Bf16GdnGatingScheduleId::SimtWarpRowC8:
        return false;
    }
    return false;
}

std::int32_t mma_tile_cols(const Bf16GdnGatingProblem& problem) noexcept {
    return is_35(problem) ? 64 : 128;
}

std::int32_t schedule_split_k(Bf16GdnGatingScheduleId schedule) {
    switch (schedule) {
    case Bf16GdnGatingScheduleId::SmallTSplit10:
        return 10;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
        return 32;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
        return 16;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
        return 8;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
        return 4;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
        return 2;
    case Bf16GdnGatingScheduleId::GemvPairedRows:
    case Bf16GdnGatingScheduleId::SimtWarpRowC4:
    case Bf16GdnGatingScheduleId::SimtWarpRowC8:
    case Bf16GdnGatingScheduleId::MmaUnsplit:
        return 1;
    }
    throw std::logic_error("BF16 GDN gating: unknown schedule");
}

bool cooperative_grid_is_resident(Bf16GdnGatingScheduleId schedule, std::int32_t cols,
                                  std::int32_t tile_cols, std::int32_t row_tiles,
                                  std::int32_t resident_ctas) noexcept {
    const std::int64_t column_tiles = (static_cast<std::int64_t>(cols) + tile_cols - 1) / tile_cols;
    const std::int64_t grid_ctas =
        column_tiles * row_tiles * static_cast<std::int64_t>(schedule_split_k(schedule));
    return grid_ctas <= resident_ctas;
}

// The split-K kernel's per-CTA residency (dynamic shared memory plus register budget) is fixed
// by the sm_120a build, so the only device-dependent term of the cooperative grid limit is the
// SM count. The tuned route table was measured on the 170-SM 5090 (340/680 CTAs device-wide);
// querying the running device instead of assuming that count keeps every cooperative launch
// resident on any SM120 part, e.g. the smaller RTX PRO 4000 Blackwell.
std::int32_t device_sm_count() {
    static std::once_flag once;
    static std::int32_t sm_count = 0;
    std::call_once(once, [] {
        int device = 0;
        if (cudaGetDevice(&device) != cudaSuccess) { device = 0; }
        int sm = 0;
        const cudaError_t err = cudaDeviceGetAttribute(&sm, cudaDevAttrMultiProcessorCount, device);
        if (err != cudaSuccess || sm <= 0) {
            throw std::runtime_error(std::string("BF16 GDN gating: cannot query SM count: ") +
                                     cudaGetErrorName(err));
        }
        sm_count = sm;
    });
    return sm_count;
}

// Per-SM CTA limits of the registered sm_120a build. BN128 uses 40 KiB of dynamic shared
// memory; split8 uses 71 registers with 256 threads and split4/2 use 62 registers with 512
// threads, so two CTAs fit per SM. BN64 uses 24 KiB; split32 uses 91/93 registers per thread
// (two CTAs/SM) while split16/8/4/2 use at most 62 registers (four CTAs/SM).
constexpr std::int32_t kCtasPerSm27        = 2;
constexpr std::int32_t kCtasPerSm35Split32 = 2;
constexpr std::int32_t kCtasPerSm35Other   = 4;

bool cooperative_27_grid_is_resident(Bf16GdnGatingScheduleId schedule, std::int32_t cols) {
    // There are three 16-row tiles per 128-token tile.
    return cooperative_grid_is_resident(schedule, cols, 128, 3,
                                        device_sm_count() * kCtasPerSm27);
}

bool cooperative_35_grid_is_resident(Bf16GdnGatingScheduleId schedule, std::int32_t cols) {
    // There are two 16-row tiles per 64-token tile.
    const std::int32_t ctas_per_sm =
        schedule == Bf16GdnGatingScheduleId::MmaCooperativeSplit32 ? kCtasPerSm35Split32
                                                                   : kCtasPerSm35Other;
    return cooperative_grid_is_resident(schedule, cols, 64, 2, device_sm_count() * ctas_per_sm);
}

// Split ladder per geometry, from the most parallel cooperative route to the unsplit fallback.
// MmaUnsplit (SplitK == 1) never issues a cooperative launch, so it is always launchable and
// closes the ladder.
constexpr std::array<Bf16GdnGatingScheduleId, 4> k27SplitLadder{{
    Bf16GdnGatingScheduleId::MmaCooperativeSplit8,
    Bf16GdnGatingScheduleId::MmaCooperativeSplit4,
    Bf16GdnGatingScheduleId::MmaCooperativeSplit2,
    Bf16GdnGatingScheduleId::MmaUnsplit,
}};

constexpr std::array<Bf16GdnGatingScheduleId, 5> k35SplitLadder{{
    Bf16GdnGatingScheduleId::MmaCooperativeSplit16,
    Bf16GdnGatingScheduleId::MmaCooperativeSplit8,
    Bf16GdnGatingScheduleId::MmaCooperativeSplit4,
    Bf16GdnGatingScheduleId::MmaCooperativeSplit2,
    Bf16GdnGatingScheduleId::MmaUnsplit,
}};

template <std::size_t N>
Bf16GdnGatingScheduleId resident_mma_schedule(const std::array<Bf16GdnGatingScheduleId, N>& ladder,
                                              Bf16GdnGatingScheduleId tuned,
                                              const Bf16GdnGatingProblem& problem) {
    std::size_t start = ladder.size();
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        if (ladder[i] == tuned) { start = i; break; }
    }
    if (start == ladder.size()) {
        throw std::logic_error("BF16 GDN gating: tuned split route missing from its ladder");
    }
    for (std::size_t i = start; i < ladder.size(); ++i) {
        const Bf16GdnGatingScheduleId candidate = ladder[i];
        if (candidate == Bf16GdnGatingScheduleId::MmaUnsplit ||
            (is_27(problem) ? cooperative_27_grid_is_resident(candidate, problem.cols)
                            : cooperative_35_grid_is_resident(candidate, problem.cols))) {
            return candidate;
        }
    }
    return Bf16GdnGatingScheduleId::MmaUnsplit; // unreachable: the ladder ends in MmaUnsplit
}

bool candidate_is_legal(Bf16GdnGatingScheduleId schedule, const Bf16GdnGatingProblem& problem) {
    if (!bf16_gdn_gating_admits(problem)) { return false; }
    if (is_27(problem)) {
        switch (schedule) {
        case Bf16GdnGatingScheduleId::GemvPairedRows:
            return problem.cols == 1;
        case Bf16GdnGatingScheduleId::SmallTSplit10:
            return problem.cols >= 2 && problem.cols <= 8;
        case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
        case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
        case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
            return cooperative_27_grid_is_resident(schedule, problem.cols);
        case Bf16GdnGatingScheduleId::MmaUnsplit:
            return true;
        case Bf16GdnGatingScheduleId::SimtWarpRowC4:
        case Bf16GdnGatingScheduleId::SimtWarpRowC8:
        case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
        case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
            return false;
        }
    }

    switch (schedule) {
    case Bf16GdnGatingScheduleId::SimtWarpRowC4:
        return problem.cols <= 4 * 65'535;
    case Bf16GdnGatingScheduleId::SimtWarpRowC8:
        return problem.cols <= 8 * 65'535;
    case Bf16GdnGatingScheduleId::MmaUnsplit:
        return true;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
        return cooperative_35_grid_is_resident(schedule, problem.cols);
    case Bf16GdnGatingScheduleId::GemvPairedRows:
    case Bf16GdnGatingScheduleId::SmallTSplit10:
        return false;
    }
    return false;
}

std::size_t checked_partial_bytes(std::int32_t heads, std::int32_t split_k, std::int32_t cols) {
    const std::size_t logical_rows = static_cast<std::size_t>(2 * heads);
    const std::size_t split        = static_cast<std::size_t>(split_k);
    const std::size_t tokens       = static_cast<std::size_t>(cols);
    if (tokens > std::numeric_limits<std::size_t>::max() / logical_rows ||
        split > std::numeric_limits<std::size_t>::max() / (tokens * logical_rows)) {
        throw std::overflow_error("BF16 GDN gating workspace element count overflows size_t");
    }
    const std::size_t elements = split * tokens * logical_rows;
    if (elements > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw std::overflow_error("BF16 GDN gating workspace byte count overflows size_t");
    }
    return elements * sizeof(float);
}

void execute_resolved(const Bf16GdnGatingPlan& plan, const Bf16GdnGatingProblem& problem,
                      const Tensor& x, const Weight& a_weight, const Weight& b_weight,
                      const Tensor& A_log, const Tensor& dt_bias, WorkspaceArena& ws, Tensor& g,
                      Tensor& beta, cudaStream_t stream) {
    auto scratch_scope = ws.scope();
    DeviceSpan scratch{};
    if (plan.workspace_bytes != 0) { scratch = ws.alloc_bytes(plan.workspace_bytes); }

    switch (plan.schedule) {
    case Bf16GdnGatingScheduleId::GemvPairedRows:
        bf16_gdn_gating_proj_gemv_launch(x, a_weight, b_weight, A_log, dt_bias, g, beta, stream);
        return;
    case Bf16GdnGatingScheduleId::SmallTSplit10:
        bf16_gdn_gating_proj_small_t_split10_launch(x, a_weight, b_weight, A_log, dt_bias,
                                                    scratch.data, scratch.bytes, g, beta, stream);
        return;
    case Bf16GdnGatingScheduleId::SimtWarpRowC4:
        bf16_gdn_gating_proj_35_simt_c4_launch(x, a_weight, b_weight, A_log, dt_bias, g, beta,
                                               stream);
        return;
    case Bf16GdnGatingScheduleId::SimtWarpRowC8:
        bf16_gdn_gating_proj_35_simt_c8_launch(x, a_weight, b_weight, A_log, dt_bias, g, beta,
                                               stream);
        return;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
        bf16_gdn_gating_proj_35_mma_split32_launch(plan.token_variant, x, a_weight, b_weight, A_log,
                                                   dt_bias, scratch.data, g, beta, stream);
        return;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
        bf16_gdn_gating_proj_35_mma_split16_launch(plan.token_variant, x, a_weight, b_weight, A_log,
                                                   dt_bias, scratch.data, g, beta, stream);
        return;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
        if (is_35(problem)) {
            bf16_gdn_gating_proj_35_mma_split8_launch(plan.token_variant, x, a_weight, b_weight,
                                                      A_log, dt_bias, scratch.data, g, beta,
                                                      stream);
        } else {
            bf16_gdn_gating_proj_mma_split8_launch(plan.token_variant, x, a_weight, b_weight, A_log,
                                                   dt_bias, scratch.data, g, beta, stream);
        }
        return;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
        if (is_35(problem)) {
            bf16_gdn_gating_proj_35_mma_split4_launch(plan.token_variant, x, a_weight, b_weight,
                                                      A_log, dt_bias, scratch.data, g, beta,
                                                      stream);
        } else {
            bf16_gdn_gating_proj_mma_split4_launch(plan.token_variant, x, a_weight, b_weight, A_log,
                                                   dt_bias, scratch.data, g, beta, stream);
        }
        return;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
        if (is_35(problem)) {
            bf16_gdn_gating_proj_35_mma_split2_launch(plan.token_variant, x, a_weight, b_weight,
                                                      A_log, dt_bias, scratch.data, g, beta,
                                                      stream);
        } else {
            bf16_gdn_gating_proj_mma_split2_launch(plan.token_variant, x, a_weight, b_weight, A_log,
                                                   dt_bias, scratch.data, g, beta, stream);
        }
        return;
    case Bf16GdnGatingScheduleId::MmaUnsplit:
        if (is_35(problem)) {
            bf16_gdn_gating_proj_35_mma_unsplit_launch(plan.token_variant, x, a_weight, b_weight,
                                                       A_log, dt_bias, g, beta, stream);
        } else {
            bf16_gdn_gating_proj_mma_unsplit_launch(plan.token_variant, x, a_weight, b_weight,
                                                    A_log, dt_bias, g, beta, stream);
        }
        return;
    }
    throw std::logic_error("BF16 GDN gating: unknown schedule");
}

template <std::size_t N>
std::size_t route_capacity(const std::array<RouteSpec, N>& routes, const Bf16GdnGatingProblem& base,
                           std::int32_t min_cols, std::int32_t max_cols) {
    // With device-adaptive split selection the resolved workspace is piecewise linear in cols:
    // the schedule is constant between route endpoints and cooperative-grid steps, and grows
    // linearly inside such a region. The column-tile count ceil(cols / tile_cols) increases at
    // cols = k * tile_cols + 1, which is where a residency boundary can drop the split; the
    // segment maximum therefore sits at min_cols, at max_cols, at a route endpoint, or at a
    // multiple of the tile. Evaluating the resolved plan at exactly those points is exact on
    // every device.
    const std::int32_t tile = mma_tile_cols(base);
    std::size_t maximum     = 0;
    auto observe            = [&](std::int32_t cols) {
        maximum = std::max(
            maximum,
            bf16_gdn_gating_resolve_plan({base.heads, base.input_rows, cols}).workspace_bytes);
    };
    observe(min_cols);
    observe(max_cols);
    for (const RouteSpec& route : routes) {
        if (route.cols.last < min_cols || route.cols.first > max_cols) { continue; }
        const std::int32_t endpoint = std::min(route.cols.last, max_cols);
        if (endpoint != min_cols && endpoint != max_cols) { observe(endpoint); }
    }
    for (std::int64_t step = (static_cast<std::int64_t>(min_cols) / tile + 1) * tile;
         step <= static_cast<std::int64_t>(max_cols); step += tile) {
        if (step > min_cols && step < max_cols) { observe(static_cast<std::int32_t>(step)); }
    }
    return maximum;
}

} // namespace

const char* bf16_gdn_gating_schedule_name(Bf16GdnGatingScheduleId schedule) noexcept {
    switch (schedule) {
    case Bf16GdnGatingScheduleId::GemvPairedRows:
        return "gdn_gating_proj.bf16.gemv.paired_rows";
    case Bf16GdnGatingScheduleId::SmallTSplit10:
        return "gdn_gating_proj.bf16.small_t.split10";
    case Bf16GdnGatingScheduleId::SimtWarpRowC4:
        return "gdn_gating_proj.bf16.simt.warp_row.c4";
    case Bf16GdnGatingScheduleId::SimtWarpRowC8:
        return "gdn_gating_proj.bf16.simt.warp_row.c8";
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
        return "gdn_gating_proj.bf16.mma.cooperative_split32";
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
        return "gdn_gating_proj.bf16.mma.cooperative_split16";
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
        return "gdn_gating_proj.bf16.mma.cooperative_split8";
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
        return "gdn_gating_proj.bf16.mma.cooperative_split4";
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
        return "gdn_gating_proj.bf16.mma.cooperative_split2";
    case Bf16GdnGatingScheduleId::MmaUnsplit:
        return "gdn_gating_proj.bf16.mma.unsplit";
    }
    return "gdn_gating_proj.bf16.unknown";
}

const char* bf16_gdn_norm_gating_schedule_name(Bf16GdnNormGatingScheduleId schedule) noexcept {
    switch (schedule) {
    case Bf16GdnNormGatingScheduleId::Composed:
        return "gdn_norm_gating_proj.bf16.composed";
    case Bf16GdnNormGatingScheduleId::MmaCooperativeSplit32:
        return "gdn_norm_gating_proj.bf16.mma.cooperative_split32";
    }
    return "gdn_norm_gating_proj.bf16.unknown";
}

bool bf16_gdn_gating_admits(const Bf16GdnGatingProblem& problem) noexcept {
    if (problem.cols < 1) { return false; }
    return is_27(problem) || is_35(problem);
}

Bf16GdnGatingPlan bf16_gdn_gating_resolve_candidate(Bf16GdnGatingScheduleId schedule,
                                                    const Bf16GdnGatingProblem& problem) {
    if (!candidate_is_legal(schedule, problem)) {
        throw std::invalid_argument("BF16 GDN gating: candidate is not legal for exact problem");
    }
    const bool mma                          = schedule_uses_mma(schedule);
    const Bf16GdnGatingTokenVariant variant = !mma ? Bf16GdnGatingTokenVariant::None
                                                   : ((problem.cols % mma_tile_cols(problem)) == 0
                                                          ? Bf16GdnGatingTokenVariant::Full
                                                          : Bf16GdnGatingTokenVariant::Predicated);
    const std::int32_t split_k              = schedule_split_k(schedule);
    const std::size_t workspace =
        split_k > 1 ? checked_partial_bytes(problem.heads, split_k, problem.cols) : 0;
    return {schedule, variant, workspace};
}

Bf16GdnGatingPlan bf16_gdn_gating_resolve_plan(const Bf16GdnGatingProblem& problem) {
    if (!bf16_gdn_gating_admits(problem)) {
        throw std::invalid_argument(
            "BF16 GDN gating: exact problem or column count is not admitted");
    }
    auto resolve_route = [&](const auto& routes, const auto& ladder) {
        for (const RouteSpec& route : routes) {
            if (!route.cols.contains(problem.cols)) { continue; }
            Bf16GdnGatingScheduleId schedule = route.schedule;
            // The tuned cooperative route is guaranteed resident only on the 170-SM 5090. On a
            // smaller SM120 part walk the ladder down until the cooperative grid is resident;
            // MmaUnsplit closes the ladder and is always launchable.
            if (schedule_uses_mma(schedule) &&
                schedule != Bf16GdnGatingScheduleId::MmaUnsplit) {
                schedule = resident_mma_schedule(ladder, schedule, problem);
            }
            return bf16_gdn_gating_resolve_candidate(schedule, problem);
        }
        throw std::logic_error("BF16 GDN gating: admitted problem has no covering route");
    };
    return is_27(problem) ? resolve_route(k27Routes, k27SplitLadder)
                          : resolve_route(k35Routes, k35SplitLadder);
}

std::size_t bf16_gdn_gating_capacity_workspace_bytes(std::int32_t heads, std::int32_t input_rows,
                                                     std::int32_t min_cols, std::int32_t max_cols) {
    if (min_cols <= 0 || max_cols < min_cols) {
        throw std::invalid_argument("BF16 GDN gating: invalid column interval");
    }
    const Bf16GdnGatingProblem base{heads, input_rows, 1};
    (void)bf16_gdn_gating_resolve_plan({heads, input_rows, min_cols});
    (void)bf16_gdn_gating_resolve_plan({heads, input_rows, max_cols});
    return is_27(base) ? route_capacity(k27Routes, base, min_cols, max_cols)
                       : route_capacity(k35Routes, base, min_cols, max_cols);
}

Bf16GdnNormGatingPlan bf16_gdn_norm_gating_resolve_plan(const Bf16GdnGatingProblem& problem) {
    Bf16GdnGatingPlan control            = bf16_gdn_gating_resolve_plan(problem);
    Bf16GdnNormGatingScheduleId schedule = Bf16GdnNormGatingScheduleId::Composed;
    std::int32_t norm_splits             = 0;
    // The fused route reuses the split32 cooperative grid. When that grid is not resident on
    // the running device, keep the composed path (rmsnorm + the device-adaptive control plan).
    if (is_35(problem) && problem.cols <= 16 &&
        cooperative_35_grid_is_resident(Bf16GdnGatingScheduleId::MmaCooperativeSplit32,
                                        problem.cols)) {
        control     = bf16_gdn_gating_resolve_candidate(
            Bf16GdnGatingScheduleId::MmaCooperativeSplit32, problem);
        schedule    = Bf16GdnNormGatingScheduleId::MmaCooperativeSplit32;
        norm_splits = 32;
    }
    const std::size_t norm_partial_bytes =
        static_cast<std::size_t>(norm_splits) * problem.cols * sizeof(float);
    return {schedule, control, control.workspace_bytes + norm_partial_bytes};
}

std::size_t bf16_gdn_norm_gating_capacity_workspace_bytes(std::int32_t heads,
                                                          std::int32_t input_rows,
                                                          std::int32_t min_cols,
                                                          std::int32_t max_cols) {
    std::size_t maximum =
        bf16_gdn_gating_capacity_workspace_bytes(heads, input_rows, min_cols, max_cols);
    if (heads == 32 && input_rows == 2048 && min_cols <= 16) {
        const std::int32_t fused_cols = std::min<std::int32_t>(max_cols, 16);
        maximum                       = std::max(
            maximum,
            bf16_gdn_norm_gating_resolve_plan({heads, input_rows, fused_cols}).workspace_bytes);
    }
    return maximum;
}

void bf16_gdn_gating_execute_plan(const Bf16GdnGatingPlan& plan, const Tensor& x,
                                  const Weight& a_weight, const Weight& b_weight,
                                  const Tensor& A_log, const Tensor& dt_bias, WorkspaceArena& ws,
                                  Tensor& g, Tensor& beta, cudaStream_t stream) {
    const Bf16GdnGatingProblem problem{g.ne[0], x.ne[0], x.ne[1]};
    const Bf16GdnGatingPlan resolved = bf16_gdn_gating_resolve_plan(problem);
    if (resolved.schedule != plan.schedule || resolved.token_variant != plan.token_variant ||
        resolved.workspace_bytes != plan.workspace_bytes) {
        throw std::invalid_argument("BF16 GDN gating: plan does not match the exact problem");
    }
    execute_resolved(plan, problem, x, a_weight, b_weight, A_log, dt_bias, ws, g, beta, stream);
}

void bf16_gdn_gating_execute_candidate(Bf16GdnGatingScheduleId schedule, const Tensor& x,
                                       const Weight& a_weight, const Weight& b_weight,
                                       const Tensor& A_log, const Tensor& dt_bias,
                                       WorkspaceArena& ws, Tensor& g, Tensor& beta,
                                       cudaStream_t stream) {
    const Bf16GdnGatingProblem problem{g.ne[0], x.ne[0], x.ne[1]};
    const Bf16GdnGatingPlan plan = bf16_gdn_gating_resolve_candidate(schedule, problem);
    execute_resolved(plan, problem, x, a_weight, b_weight, A_log, dt_bias, ws, g, beta, stream);
}

void bf16_gdn_gating_dispatch(const Tensor& x, const Weight& a_weight, const Weight& b_weight,
                              const Tensor& A_log, const Tensor& dt_bias, WorkspaceArena& ws,
                              Tensor& g, Tensor& beta, cudaStream_t stream) {
    const Bf16GdnGatingPlan plan = bf16_gdn_gating_resolve_plan({g.ne[0], x.ne[0], x.ne[1]});
    bf16_gdn_gating_execute_plan(plan, x, a_weight, b_weight, A_log, dt_bias, ws, g, beta, stream);
}

void bf16_gdn_norm_gating_dispatch(const Tensor& x, const Tensor& norm_weight, float eps, Tensor& h,
                                   const Weight& a_weight, const Weight& b_weight,
                                   const Tensor& A_log, const Tensor& dt_bias, WorkspaceArena& ws,
                                   Tensor& g, Tensor& beta, cudaStream_t stream) {
    const Bf16GdnGatingProblem problem{g.ne[0], x.ne[0], x.ne[1]};
    const Bf16GdnNormGatingPlan plan = bf16_gdn_norm_gating_resolve_plan(problem);
    if (plan.schedule == Bf16GdnNormGatingScheduleId::Composed) {
        rmsnorm(x, norm_weight, eps, true, h, stream);
        execute_resolved(plan.control, problem, h, a_weight, b_weight, A_log, dt_bias, ws, g, beta,
                         stream);
        return;
    }

    auto scratch_scope = ws.scope();
    DeviceSpan scratch{};
    if (plan.workspace_bytes != 0) { scratch = ws.alloc_bytes(plan.workspace_bytes); }
    bf16_gdn_norm_gating_proj_35_mma_split32_launch(plan.control.token_variant, x, norm_weight, eps,
                                                    h, a_weight, b_weight, A_log, dt_bias,
                                                    scratch.data, g, beta, stream);
}

} // namespace ninfer::ops::detail
