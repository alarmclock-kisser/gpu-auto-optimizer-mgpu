#include "core/manual.hpp"
#include <algorithm>
#include <cmath>

namespace gao {

namespace {
int snap_to(int value, int step) {
    if (step <= 1) return value;
    return static_cast<int>(std::lround(static_cast<double>(value) / step) * step);
}
}

ManualZone core_zone(int mhz) {
    if (mhz > 270) return ManualZone::Extreme;   // top 10 % of 0-300
    if (mhz > 210) return ManualZone::Warm;     // 70-90 %
    return ManualZone::Safe;                    // stock and any downclock
}

ManualZone mem_zone(int mhz) {
    if (mhz > 1350) return ManualZone::Extreme;   // top 10 % of 0-1500
    if (mhz > 1050) return ManualZone::Warm;
    return ManualZone::Safe;                      // stock and any downclock
}

ManualZone power_zone(int pct, int range_min, int range_max) {
    const int lo = std::min(range_min, range_max);
    const int hi = std::max(range_min, range_max);
    if (pct < lo || pct > hi) return ManualZone::Extreme;
    if (pct > kManualPowerHardMax || pct < kManualPowerHardMin) return ManualZone::Extreme;
    if (pct > 110) return ManualZone::Extreme;
    if (hi - lo >= 10 && pct > hi - 5) return ManualZone::Extreme;
    if (pct > 100) return ManualZone::Warm;
    return ManualZone::Safe;
}

bool manual_is_extreme(const ManualTune& tune, int range_min, int range_max) {
    return core_zone(manual_effective_core(tune)) == ManualZone::Extreme ||
           mem_zone(manual_effective_mem(tune)) == ManualZone::Extreme ||
           power_zone(manual_effective_power(tune), range_min, range_max) == ManualZone::Extreme;
}

int manual_effective_core(const ManualTune& tune) { return tune.use_core ? tune.core_mhz : 0; }
int manual_effective_mem(const ManualTune& tune) { return tune.use_mem ? tune.mem_mhz : 0; }
int manual_effective_power(const ManualTune& tune) { return tune.use_power ? tune.power_pct : 100; }

ManualTune manual_snap(ManualTune tune) {
    tune.core_mhz = std::clamp(snap_to(tune.core_mhz, kManualCoreStep), kManualCoreMin, kManualCoreMax);
    tune.mem_mhz = std::clamp(snap_to(tune.mem_mhz, kManualMemStep), kManualMemMin, kManualMemMax);
    tune.power_pct = std::clamp(snap_to(tune.power_pct, kManualPowerStep), kManualPowerHardMin, kManualPowerHardMax);
    return tune;
}

bool manual_in_range(const ManualTune& tune, int power_min, int power_max, std::string* why) {
    const int core = manual_effective_core(tune);
    const int mem = manual_effective_mem(tune);
    const int power = manual_effective_power(tune);
    const int lo = std::min(power_min, power_max);
    const int hi = std::max(power_min, power_max);
    auto fail = [&](const std::string& reason) {
        if (why) *why = reason + "; nothing applied";
        return false;
    };
    if (core < kManualCoreMin || core > kManualCoreMax)
        return fail("core " + std::to_string(core) + " MHz out of range " + std::to_string(kManualCoreMin) + "-" +
                    std::to_string(kManualCoreMax) + " MHz");
    if (mem < kManualMemMin || mem > kManualMemMax)
        return fail("mem " + std::to_string(mem) + " MHz out of range " + std::to_string(kManualMemMin) + "-" +
                    std::to_string(kManualMemMax) + " MHz");
    if (power < kManualPowerHardMin || power > kManualPowerHardMax)
        return fail("power " + std::to_string(power) + " % out of range 50-150 %");
    if (power < lo || power > hi)
        return fail("power " + std::to_string(power) + " % outside the driver range " + std::to_string(lo) + "-" +
                    std::to_string(hi) + " %");
    return true;
}

bool apply_manual(const GpuControl& gpu, const ManualTune& tune, Journal* journal, int power_min, int power_max,
                  std::string* why) {
    if (!manual_in_range(tune, power_min, power_max, why)) return false;
    const int core = manual_effective_core(tune);
    const int mem = manual_effective_mem(tune);
    const int power = manual_effective_power(tune);
    auto fail = [&](const std::string& reason) {
        const bool reset = gpu.reset_to_stock && gpu.reset_to_stock();
        if (why) *why = reason + (reset ? " -- card at stock" : " -- reset to stock FAILED, run `gao --reset`");
        return false;
    };
    // Journal first: flushed to disk before the first write, so a freeze
    // between the writes leaves an unmatched begin behind. Only raised
    // clocks are journaled: ceilings are exclusive upper bounds for the
    // next search, and a downclock cannot be the crash cause. A begin with
    // neither still records the attempt as "settings".
    int journal_id = -1;
    if (journal) {
        const std::optional<int> core_j = tune.use_core && core >= 0 ? std::optional<int>(core) : std::nullopt;
        const std::optional<int> mem_j = tune.use_mem && mem >= 0 ? std::optional<int>(mem) : std::nullopt;
        journal_id = journal->begin(core_j, mem_j);
        if (journal_id < 0) {
            if (why) *why = "could not write the journal; nothing applied";
            return false;
        }
    }
    auto set = [](const std::function<bool(int)>& setter, int value, int stock) {
        return setter ? setter(value) : value == stock;
    };
    if (!set(gpu.set_power_limit, power, 100)) {
        if (journal) journal->complete(journal_id, "SET FAILED");
        return fail("setting power " + std::to_string(power) + " %" + (gpu.set_power_limit ? "" : " (no power control)"));
    }
    if (!set(gpu.set_core_offset, core, 0)) {
        if (journal) journal->complete(journal_id, "SET FAILED");
        return fail("setting core +" + std::to_string(core) + " MHz failed");
    }
    if (!set(gpu.set_mem_offset, mem, 0)) {
        if (journal) journal->complete(journal_id, "SET FAILED");
        return fail("setting mem +" + std::to_string(mem) + " MHz failed");
    }
    if (journal && !journal->complete(journal_id, "MANUAL APPLY")) {
        // Applied, but the proof did not reach the disk: fail closed so the
        // next boot does not trust an unjournaled state.
        const bool reset = gpu.reset_to_stock && gpu.reset_to_stock();
        if (why)
            *why = std::string("could not write the journal") +
                   (reset ? " -- card at stock" : " -- reset to stock FAILED, run `gao --reset`");
        return false;
    }
    return true;
}

}
