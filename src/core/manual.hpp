#pragma once
#include "core/journal.hpp"
#include "core/search.hpp"
#include "core/types.hpp"
#include <optional>
#include <string>

namespace gao {

// Manual tuning: the same three knobs the optimizer tests and changes
// (power limit, core offset, memory offset), driven by hand from the Manual
// page. Voltage points, per-P-state clocks and multipliers are deliberately
// not here: the public NVML/NVAPI path exposes no safe writable handle for
// them, and locking a voltage point hard-froze the reference card during
// development. The UI says so instead of hiding it.
//
// Bounds reuse the search bounds on the way up so a hand-applied value can
// never exceed what --optimize could have produced; below stock a modest
// downclock is allowed. That is downclocking, not undervolting: the voltage
// curve is untouched either way. apply_manual() refuses anything outside
// them before touching hardware or the journal.
struct ManualTune {
    bool use_core = true;
    bool use_mem = true;
    bool use_power = true;
    int core_mhz = 0;
    int mem_mhz = 0;
    int power_pct = 100;
};

inline constexpr int kManualCoreMin = -200;
inline constexpr int kManualCoreMax = kCoreMaxMhz;   // 300
inline constexpr int kManualCoreStep = 15;
inline constexpr int kManualMemMin = -500;
inline constexpr int kManualMemMax = kMemMaxMhz;   // 1500
inline constexpr int kManualMemStep = 50;
inline constexpr int kManualPowerHardMin = 50;
inline constexpr int kManualPowerHardMax = 150;
inline constexpr int kManualPowerStep = 5;

// How far into the range a value sits, for the color marking on the Manual
// page. Thresholds are fractions of the search range: the top 10 % is
// extreme, 70-90 % is warm. Anything at or below stock is safe: a downclock
// is the thermally safe direction. Power is asymmetric too: low power is
// always safe, high power turns warm above stock and extreme near the
// driver maximum.
enum class ManualZone { Safe, Warm, Extreme };

ManualZone core_zone(int mhz);
ManualZone mem_zone(int mhz);
ManualZone power_zone(int pct, int range_min, int range_max);
bool manual_is_extreme(const ManualTune& tune, int range_min, int range_max);

// Effective values: a disabled knob counts as stock.
int manual_effective_core(const ManualTune& tune);
int manual_effective_mem(const ManualTune& tune);
int manual_effective_power(const ManualTune& tune);

// Snap to the hardware steps (core 15, mem 50, power 5).
ManualTune manual_snap(ManualTune tune);

// Range check on the effective values. power_min/max is the driver range
// (power_limit_range_pct); when unknown pass the hard bounds. Refuses before
// anything is written, including the journal.
bool manual_in_range(const ManualTune& tune, int power_min, int power_max, std::string* why);

// Journaled apply: a begin line is flushed to disk before the first write,
// completed after the last one, so a freeze between them becomes a ceiling
// for the next optimize run (borderline OC, one step back). Only raised
// (non-negative) clocks are journaled: the journal's ceilings are exclusive
// upper bounds, and a downclock cannot be the crash cause. A journal may be
// null (unit tests); production always passes one. Any failure resets to
// stock and returns false with the reason in *why.
bool apply_manual(const GpuControl& gpu, const ManualTune& tune, Journal* journal, int power_min, int power_max,
                  std::string* why);

}
