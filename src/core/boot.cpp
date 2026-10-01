#include "core/boot.hpp"
#include "core/manual.hpp"
#include "core/search.hpp"

namespace gao {

BootDecision decide_boot(const DeviceSettings& device, const std::string& driver, const std::string& gpu) {
    if (!device.profile) return BootDecision::NoProfile;
    if (device.boot_strikes >= kMaxBootStrikes) return BootDecision::TooManyStrikes;
    if (driver.empty() || driver != device.profile->driver) return BootDecision::DriverChanged;
    if (gpu.empty() || gpu != device.gpu || gpu != device.profile->gpu) return BootDecision::GpuChanged;
    return BootDecision::Apply;
}

bool apply_profile(const GpuControl& gpu, const Profile& p, std::string* why) {
    // Up: never more than the search could have produced. Down: a modest
    // downclock from a hand-tuned profile (downclocking, not undervolting).
    if (p.power_pct < 50 || p.power_pct > 150 || p.core_mhz < kManualCoreMin || p.core_mhz > kCoreMaxMhz ||
        p.mem_mhz < kManualMemMin || p.mem_mhz > kMemMaxMhz) {
        if (why) *why = "profile values out of range; nothing applied";
        return false;
    }
    auto fail = [&](const std::string& reason) {
        const bool reset = gpu.reset_to_stock && gpu.reset_to_stock();
        if (why) *why = reason + (reset ? " -- card at stock" : " -- reset to stock FAILED, run `gao --reset`");
        return false;
    };
    // A missing setter is only acceptable when the profile wants stock there.
    auto set = [](const std::function<bool(int)>& setter, int value, int stock) {
        return setter ? setter(value) : value == stock;
    };
    if (!set(gpu.set_power_limit, p.power_pct, 100))
        return fail("setting power " + std::to_string(p.power_pct) + " % failed" +
                    (gpu.set_power_limit ? "" : " (no power control on this card)"));
    if (!set(gpu.set_core_offset, p.core_mhz, 0)) return fail("setting core +" + std::to_string(p.core_mhz) + " MHz failed");
    if (!set(gpu.set_mem_offset, p.mem_mhz, 0)) return fail("setting mem +" + std::to_string(p.mem_mhz) + " MHz failed");
    return true;
}

}
