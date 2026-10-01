#pragma once
#include "core/fan_curve.hpp"
#include "core/objectives.hpp"
#include <optional>
#include <string>

namespace gao {

// What --optimize found, as --apply and --boot-apply re-apply it.
struct Profile {
    Preset preset = Preset::BestOfMyGpu;
    int power_pct = 100;
    int core_mhz = 0;
    int mem_mhz = 0;
    std::string driver;     // driver version the profile was tested on
    std::string gpu;        // NVML UUID of the card it was tested on
    std::string saved_at;   // local time, "YYYY-MM-DD HH:MM"
    std::optional<FanCurve> fan_curve;   // the curve the tune was tested with; nullopt: tuned before fan control
};

// Settings that belong to one physical GPU, keyed by its stable NVML UUID.
struct DeviceSettings {
    std::string gpu;
    std::optional<Profile> profile;
    int boot_strikes = 0;                // logons that applied this GPU's profile and have not yet run 2 minutes
    std::optional<FanCurve> fan_curve;   // edited curve; otherwise use the tested/profile default curve
    bool fan_control = false;
    int fan_min_pct = 0;                 // learned minimum manual fan speed
};

// Everything that has to survive a reboot. Freeze ceilings are not here:
// they live in the journal.
struct Config {
    std::vector<DeviceSettings> devices;
    std::string selected_gpu;            // NVML UUID chosen in the dashboard
};

const std::string& effective_gpu_selection(const Config& config, const std::string& session_selection);
std::string to_json(const Config& c);
// Never throws. Bad input yields defaults; a profile with any field missing
// or mistyped is treated as no profile rather than half a profile.
Config from_json(const std::string& text);

const DeviceSettings* find_device(const Config& c, const std::string& gpu);
DeviceSettings* find_device(Config& c, const std::string& gpu);
DeviceSettings& ensure_device(Config& c, const std::string& gpu);
const Profile* find_profile(const Config& c, const std::string& gpu);

// The curve the fans follow for this GPU: edited curve, tested curve, then
// the profile default. nullopt without a profile or edited curve.
std::optional<FanCurve> active_fan_curve(const Config& c, const std::string& gpu);
int fan_min_for(const Config& c, const std::string& gpu, int nvml_min);

}
