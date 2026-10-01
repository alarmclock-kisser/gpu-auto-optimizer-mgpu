#include "core/config.hpp"
#include "core/boot.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <climits>
#include <utility>

namespace gao {

namespace {
nlohmann::json curve_json(const FanCurve& c) {
    nlohmann::json points = nlohmann::json::array();
    for (const FanPoint& p : c.points) points.push_back({p.temp_c, p.pct});
    return {{"stop_below_c", c.stop_below_c ? nlohmann::json(*c.stop_below_c) : nlohmann::json(nullptr)},
            {"points", points}};
}

std::optional<FanCurve> curve_from(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    FanCurve c;
    const auto stop = j.find("stop_below_c");
    if (stop != j.end() && stop->is_number_integer()) c.stop_below_c = stop->get<int>();
    else if (stop != j.end() && !stop->is_null()) return std::nullopt;
    const auto pts = j.find("points");
    if (pts == j.end() || !pts->is_array()) return std::nullopt;
    for (const auto& p : *pts) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number_integer() || !p[1].is_number_integer()) return std::nullopt;
        c.points.push_back({p[0].get<int>(), p[1].get<int>()});
    }
    if (!valid(c)) return std::nullopt;
    return c;
}

nlohmann::json profile_json(const Profile& p) {
    nlohmann::json j = {
        {"preset", preset_name(p.preset)},
        {"power_pct", p.power_pct},
        {"core_mhz", p.core_mhz},
        {"mem_mhz", p.mem_mhz},
        {"driver", p.driver},
        {"gpu", p.gpu},
        {"saved_at", p.saved_at},
        {"manual", p.manual},
    };
    if (p.fan_curve) j["fan_curve"] = curve_json(*p.fan_curve);
    return j;
}

std::optional<Profile> profile_from(const nlohmann::json& pj) {
    if (!pj.is_object()) return std::nullopt;
    auto num = [&](const char* key) -> std::optional<int> {
        const auto it = pj.find(key);
        if (it == pj.end() || !it->is_number_integer()) return std::nullopt;
        const long long value = it->get<long long>();
        if (value < INT_MIN || value > INT_MAX) return std::nullopt;
        return static_cast<int>(value);
    };
    auto str = [&](const char* key) -> std::optional<std::string> {
        const auto it = pj.find(key);
        if (it == pj.end() || !it->is_string()) return std::nullopt;
        return it->get<std::string>();
    };
    const auto name = str("preset");
    const auto preset = name ? preset_from_name(*name) : std::nullopt;
    const auto power = num("power_pct"), core = num("core_mhz"), mem = num("mem_mhz");
    const auto driver = str("driver"), gpu = str("gpu"), saved_at = str("saved_at");
    if (!preset || !power || !core || !mem || !driver || !gpu || gpu->empty() || !saved_at) return std::nullopt;
    Profile p{*preset, *power, *core, *mem, *driver, *gpu, *saved_at};
    // Profiles saved before the manual flag existed are optimizer results.
    if (const auto it = pj.find("manual"); it != pj.end() && it->is_boolean()) p.manual = it->get<bool>();
    if (const auto it = pj.find("fan_curve"); it != pj.end()) p.fan_curve = curve_from(*it);
    return p;
}

int safe_strikes(const nlohmann::json& j) {
    if (!j.is_number_integer()) return kMaxBootStrikes;
    if (j.is_number_unsigned() && j.get<unsigned long long>() > 1000) return kMaxBootStrikes;
    const long long value = j.get<long long>();
    return value >= 0 && value <= 1000 ? static_cast<int>(value) : kMaxBootStrikes;
}

void read_device_values(DeviceSettings& d, const nlohmann::json& j) {
    if (const auto it = j.find("boot_strikes"); it != j.end()) d.boot_strikes = safe_strikes(*it);
    if (const auto it = j.find("fan_control"); it != j.end() && it->is_boolean()) d.fan_control = it->get<bool>();
    if (const auto it = j.find("fan_curve"); it != j.end()) d.fan_curve = curve_from(*it);
    if (const auto it = j.find("fan_min_pct"); it != j.end() && it->is_number_integer()) {
        const long long value = it->get<long long>();
        if (value > 0 && value <= 100) d.fan_min_pct = static_cast<int>(value);
    }
}
}

std::string to_json(const Config& c) {
    nlohmann::json j = {{"selected_gpu", c.selected_gpu}};
    nlohmann::json devices = nlohmann::json::array();
    for (const DeviceSettings& d : c.devices) {
        if (d.gpu.empty()) continue;
        nlohmann::json item = {
            {"gpu", d.gpu},
            {"boot_strikes", d.boot_strikes},
            {"fan_control", d.fan_control},
            {"fan_min_pct", d.fan_min_pct},
        };
        if (d.profile) item["profile"] = profile_json(*d.profile);
        if (d.fan_curve) item["fan_curve"] = curve_json(*d.fan_curve);
        devices.push_back(std::move(item));
    }
    j["devices"] = std::move(devices);
    return j.dump(2);
}

Config from_json(const std::string& text) {
    Config c;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return c;
    if (const auto it = j.find("selected_gpu"); it != j.end() && it->is_string())
        c.selected_gpu = it->get<std::string>();

    const auto devices = j.find("devices");
    if (devices != j.end() && devices->is_array()) {
        for (const auto& item : *devices) {
            if (!item.is_object()) continue;
            const auto gpu = item.find("gpu");
            if (gpu == item.end() || !gpu->is_string() || gpu->get<std::string>().empty()) continue;
            if (find_device(c, gpu->get<std::string>())) continue;
            DeviceSettings d;
            d.gpu = gpu->get<std::string>();
            read_device_values(d, item);
            if (const auto pj = item.find("profile"); pj != item.end()) {
                d.profile = profile_from(*pj);
                if (d.profile && d.profile->gpu != d.gpu) d.profile.reset();
            }
            c.devices.push_back(std::move(d));
        }
    } else {
        // Migrate the previous single-profile format. Its profile UUID is the
        // stable device key; all fan settings and crash strikes follow it.
        DeviceSettings d;
        if (const auto pj = j.find("profile"); pj != j.end()) d.profile = profile_from(*pj);
        if (d.profile) d.gpu = d.profile->gpu;
        if (const auto it = j.find("fan_min_gpu"); it != j.end() && it->is_string()) {
            const std::string fan_gpu = it->get<std::string>();
            if (d.gpu.empty()) d.gpu = fan_gpu;
        }
        if (d.gpu.empty() && !c.selected_gpu.empty()) d.gpu = c.selected_gpu;
        read_device_values(d, j);
        if (!d.gpu.empty()) c.devices.push_back(std::move(d));
    }

    if (c.selected_gpu.empty()) {
        const auto saved = std::find_if(c.devices.begin(), c.devices.end(),
                                        [](const DeviceSettings& d) { return d.profile.has_value(); });
        if (saved != c.devices.end()) c.selected_gpu = saved->gpu;
        else if (!c.devices.empty()) c.selected_gpu = c.devices.front().gpu;
    }
    return c;
}

const std::string& effective_gpu_selection(const Config& config, const std::string& session_selection) {
    return session_selection.empty() ? config.selected_gpu : session_selection;
}

const DeviceSettings* find_device(const Config& c, const std::string& gpu) {
    const auto it = std::find_if(c.devices.begin(), c.devices.end(),
                                 [&](const DeviceSettings& d) { return d.gpu == gpu; });
    return it == c.devices.end() ? nullptr : &*it;
}

DeviceSettings* find_device(Config& c, const std::string& gpu) {
    const auto it = std::find_if(c.devices.begin(), c.devices.end(),
                                 [&](const DeviceSettings& d) { return d.gpu == gpu; });
    return it == c.devices.end() ? nullptr : &*it;
}

DeviceSettings& ensure_device(Config& c, const std::string& gpu) {
    if (DeviceSettings* existing = find_device(c, gpu)) return *existing;
    c.devices.push_back(DeviceSettings{gpu});
    return c.devices.back();
}

const Profile* find_profile(const Config& c, const std::string& gpu) {
    const DeviceSettings* device = find_device(c, gpu);
    return device && device->profile ? &*device->profile : nullptr;
}

std::optional<FanCurve> active_fan_curve(const Config& c, const std::string& gpu) {
    const DeviceSettings* device = find_device(c, gpu);
    if (!device) return std::nullopt;
    if (device->fan_curve) return device->fan_curve;
    if (!device->profile) return std::nullopt;
    if (device->profile->fan_curve) return device->profile->fan_curve;
    return default_curve(device->profile->preset);
}

int fan_min_for(const Config& c, const std::string& gpu, const int nvml_min) {
    const DeviceSettings* device = find_device(c, gpu);
    return device && !gpu.empty() ? std::max(nvml_min, device->fan_min_pct) : nvml_min;
}

}
