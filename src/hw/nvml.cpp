#include "hw/nvml.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace gao {

using nvmlReturn_t = int;
static constexpr nvmlReturn_t NVML_SUCCESS = 0;
using nvmlDevice_t = void*;

typedef nvmlReturn_t (*fn_init)();
typedef nvmlReturn_t (*fn_shutdown)();
typedef nvmlReturn_t (*fn_count)(unsigned*);
typedef nvmlReturn_t (*fn_byIndex)(unsigned, nvmlDevice_t*);
typedef nvmlReturn_t (*fn_clock)(nvmlDevice_t, int type, unsigned*);
typedef nvmlReturn_t (*fn_temp)(nvmlDevice_t, int sensor, unsigned*);
typedef nvmlReturn_t (*fn_power)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_powerlimit)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_fan)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_name)(nvmlDevice_t, char*, unsigned);
struct nvml_pci_info_v3 {
    char bus_id_legacy[16];
    unsigned domain;
    unsigned bus;
    unsigned device;
    unsigned pci_device_id;
    unsigned pci_subsystem_id;
    char bus_id[32];
};
typedef nvmlReturn_t (*fn_pci_info)(nvmlDevice_t, nvml_pci_info_v3*);
typedef nvmlReturn_t (*fn_luid)(nvmlDevice_t, char*, unsigned*);

static fn_init        p_init = nullptr;
static fn_shutdown    p_shutdown = nullptr;
static fn_count       p_count = nullptr;
static fn_byIndex     p_byIndex = nullptr;
static fn_clock       p_clock = nullptr;
static fn_temp        p_temp = nullptr;
static fn_power       p_power = nullptr;
static fn_powerlimit  p_powerlimit = nullptr;
static fn_fan         p_fan = nullptr;
static fn_name        p_name = nullptr;
static fn_pci_info    p_pci_info = nullptr;
static fn_luid        p_luid = nullptr;
typedef nvmlReturn_t (*fn_pl_default)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_pl_constraints)(nvmlDevice_t, unsigned*, unsigned*);
typedef nvmlReturn_t (*fn_pl_set)(nvmlDevice_t, unsigned);
static fn_pl_default     p_pl_default = nullptr;
static fn_pl_constraints p_pl_constraints = nullptr;
static fn_pl_set         p_pl_set = nullptr;
typedef nvmlReturn_t (*fn_driver)(char*, unsigned);
typedef nvmlReturn_t (*fn_uuid)(nvmlDevice_t, char*, unsigned);
static fn_driver p_driver = nullptr;
static fn_uuid p_uuid = nullptr;
typedef nvmlReturn_t (*fn_fan_count)(nvmlDevice_t, unsigned*);
typedef nvmlReturn_t (*fn_fan_range)(nvmlDevice_t, unsigned*, unsigned*);
typedef nvmlReturn_t (*fn_fan_set)(nvmlDevice_t, unsigned, unsigned);
typedef nvmlReturn_t (*fn_fan_default)(nvmlDevice_t, unsigned);
typedef nvmlReturn_t (*fn_fan_get)(nvmlDevice_t, unsigned, unsigned*);
static fn_fan_count   p_fan_count = nullptr;
static fn_fan_range   p_fan_range = nullptr;
static fn_fan_set     p_fan_set = nullptr;
static fn_fan_default p_fan_default = nullptr;
static fn_fan_get     p_fan_target = nullptr;
static fn_fan_get     p_fan_policy = nullptr;
static fn_fan_get     p_fan_speed = nullptr;
constexpr unsigned kFanPolicyAuto = 0;     // NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW
constexpr unsigned kFanPolicyManual = 1;   // NVML_FAN_POLICY_MANUAL

bool Nvml::Init() {
    // System32 only: gao runs elevated at logon, and the exe's own folder
    // must never be able to supply this DLL.
    HMODULE h = LoadLibraryExA("nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) { error_ = "could not load nvml.dll"; return false; }
    lib_ = h;
    p_init       = (fn_init)GetProcAddress(h, "nvmlInit_v2");
    p_shutdown   = (fn_shutdown)GetProcAddress(h, "nvmlShutdown");
    p_count      = (fn_count)GetProcAddress(h, "nvmlDeviceGetCount_v2");
    p_byIndex    = (fn_byIndex)GetProcAddress(h, "nvmlDeviceGetHandleByIndex_v2");
    p_clock      = (fn_clock)GetProcAddress(h, "nvmlDeviceGetClockInfo");
    p_temp       = (fn_temp)GetProcAddress(h, "nvmlDeviceGetTemperature");
    p_power      = (fn_power)GetProcAddress(h, "nvmlDeviceGetPowerUsage");
    p_powerlimit = (fn_powerlimit)GetProcAddress(h, "nvmlDeviceGetPowerManagementLimit");
    p_fan        = (fn_fan)GetProcAddress(h, "nvmlDeviceGetFanSpeed");
    p_name       = (fn_name)GetProcAddress(h, "nvmlDeviceGetName");
    p_pci_info   = (fn_pci_info)GetProcAddress(h, "nvmlDeviceGetPciInfo_v3");
    p_luid       = (fn_luid)GetProcAddress(h, "nvmlDeviceGetLuid");
    p_pl_default     = (fn_pl_default)GetProcAddress(h, "nvmlDeviceGetPowerManagementDefaultLimit");
    p_pl_constraints = (fn_pl_constraints)GetProcAddress(h, "nvmlDeviceGetPowerManagementLimitConstraints");
    p_pl_set         = (fn_pl_set)GetProcAddress(h, "nvmlDeviceSetPowerManagementLimit");
    p_driver = (fn_driver)GetProcAddress(h, "nvmlSystemGetDriverVersion");
    p_uuid = (fn_uuid)GetProcAddress(h, "nvmlDeviceGetUUID");
    p_fan_count   = (fn_fan_count)GetProcAddress(h, "nvmlDeviceGetNumFans");
    p_fan_range   = (fn_fan_range)GetProcAddress(h, "nvmlDeviceGetMinMaxFanSpeed");
    p_fan_set     = (fn_fan_set)GetProcAddress(h, "nvmlDeviceSetFanSpeed_v2");
    p_fan_default = (fn_fan_default)GetProcAddress(h, "nvmlDeviceSetDefaultFanSpeed_v2");
    p_fan_target  = (fn_fan_get)GetProcAddress(h, "nvmlDeviceGetTargetFanSpeed");
    p_fan_policy  = (fn_fan_get)GetProcAddress(h, "nvmlDeviceGetFanControlPolicy_v2");
    p_fan_speed   = (fn_fan_get)GetProcAddress(h, "nvmlDeviceGetFanSpeed_v2");
    if (!p_init || !p_byIndex) { error_ = "required NVML entry points not found"; return false; }
    inited_ = (p_init() == NVML_SUCCESS);
    if (!inited_) error_ = "nvmlInit_v2 failed";
    return inited_;
}

int Nvml::DeviceCount() {
    // -1 means "not ready" (not initialized, or the query failed) and is
    // never conflated with a genuine zero-device result, mirroring the
    // fan_pct sentinel convention in Telemetry. Error() carries the reason.
    if (!inited_) { error_ = "NVML not initialized"; return -1; }
    if (!p_count) { error_ = "nvmlDeviceGetCount_v2 not available"; return -1; }
    unsigned n = 0;
    if (p_count(&n) != NVML_SUCCESS) { error_ = "nvmlDeviceGetCount_v2 failed"; return -1; }
    return static_cast<int>(n);
}

Telemetry Nvml::Read(unsigned index) {
    Telemetry t;
    if (!inited_) return t;
    nvmlDevice_t dev = nullptr;
    if (p_byIndex(index, &dev) != NVML_SUCCESS) return t;
    unsigned v = 0;
    // Every field below stays at its -1 default (its "unknown" sentinel)
    // unless the driver actually reports a reading: a missing symbol or a
    // non-zero status must never be read as 0, since 0 is itself a real
    // value for several of these (an idle card's fan, a card at its thermal
    // floor). core_ok/temp_ok track the two readings the search depends on,
    // for t.ok below.
    bool core_ok = false;
    bool temp_ok = false;
    if (p_clock && p_clock(dev, /*GRAPHICS*/0, &v) == NVML_SUCCESS) { t.core_mhz = static_cast<int>(v); core_ok = true; }
    if (p_clock && p_clock(dev, /*MEM*/2, &v) == NVML_SUCCESS)      t.mem_mhz = static_cast<int>(v);
    if (p_temp && p_temp(dev, /*GPU*/0, &v) == NVML_SUCCESS)        { t.temp_c = static_cast<int>(v); temp_ok = true; }
    if (p_power && p_power(dev, &v) == NVML_SUCCESS)                t.power_w = static_cast<int>(v / 1000);
    if (p_powerlimit && p_powerlimit(dev, &v) == NVML_SUCCESS)      t.power_limit_w = static_cast<int>(v / 1000);
    if (p_fan && p_fan(dev, &v) == NVML_SUCCESS) t.fan_pct = static_cast<int>(v);
    // A device handle alone is not proof the readings this app relies on
    // (the thermal abort, the fan-curve search) are real, rather than a
    // failed call masquerading as a plausible zero. mem/power/fan may still
    // be -1 (unknown) with ok == true; callers check those individually.
    t.ok = core_ok && temp_ok;
    return t;
}

std::string Nvml::DeviceName(unsigned index) {
    nvmlDevice_t dev = nullptr;
    char buf[96] = {};
    if (!inited_ || !p_name || p_byIndex(index, &dev) != NVML_SUCCESS || p_name(dev, buf, sizeof(buf)) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetName failed";
        return {};
    }
    return buf;
}

std::optional<std::pair<int, int>> Nvml::PowerLimitRangePct(unsigned index) {
    nvmlDevice_t dev = nullptr;
    unsigned def = 0, lo = 0, hi = 0;
    if (!inited_ || p_byIndex(index, &dev) != NVML_SUCCESS) { error_ = "NVML device not available"; return std::nullopt; }
    if (!p_pl_default || p_pl_default(dev, &def) != NVML_SUCCESS || def == 0) {
        error_ = "nvmlDeviceGetPowerManagementDefaultLimit failed"; return std::nullopt;
    }
    if (!p_pl_constraints || p_pl_constraints(dev, &lo, &hi) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetPowerManagementLimitConstraints failed"; return std::nullopt;
    }
    // Round inward so every percent in the range is actually settable.
    const int min_pct = static_cast<int>((static_cast<unsigned long long>(lo) * 100 + def - 1) / def);
    const int max_pct = static_cast<int>(static_cast<unsigned long long>(hi) * 100 / def);
    return std::make_pair(min_pct, max_pct);
}

bool Nvml::SetPowerLimitPct(unsigned index, int pct) {
    nvmlDevice_t dev = nullptr;
    unsigned def = 0, now = 0;
    if (!inited_ || p_byIndex(index, &dev) != NVML_SUCCESS) { error_ = "NVML device not available"; return false; }
    if (!p_pl_default || p_pl_default(dev, &def) != NVML_SUCCESS || def == 0) {
        error_ = "nvmlDeviceGetPowerManagementDefaultLimit failed"; return false;
    }
    const unsigned target = static_cast<unsigned>(static_cast<unsigned long long>(def) * pct / 100);
    auto close_enough = [&](unsigned mw) {
        const long long diff = static_cast<long long>(mw) - static_cast<long long>(target);
        return (diff < 0 ? -diff : diff) <= def / 100;
    };
    // Already there: no write. Some cards report constraints but refuse the
    // set call; without this, resetting to a default they already have
    // would fail and block every run.
    if (p_powerlimit && p_powerlimit(dev, &now) == NVML_SUCCESS && close_enough(now)) return true;
    if (!p_pl_set || p_pl_set(dev, target) != NVML_SUCCESS) {
        error_ = "nvmlDeviceSetPowerManagementLimit failed (elevated?)"; return false;
    }
    // Never trust the return code: read the limit back.
    if (!p_powerlimit || p_powerlimit(dev, &now) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetPowerManagementLimit failed after set"; return false;
    }
    if (!close_enough(now)) {
        error_ = "power limit read back " + std::to_string(now / 1000) + " W, requested " + std::to_string(target / 1000) + " W";
        return false;
    }
    return true;
}

std::string Nvml::DriverVersion() {
    char buf[96] = {};
    if (!inited_ || !p_driver || p_driver(buf, sizeof(buf)) != NVML_SUCCESS) {
        error_ = "nvmlSystemGetDriverVersion failed";
        return {};
    }
    return buf;
}

std::optional<int> Nvml::PowerLimitPct(unsigned index) {
    nvmlDevice_t dev = nullptr;
    unsigned def = 0, now = 0;
    if (!inited_ || p_byIndex(index, &dev) != NVML_SUCCESS) return std::nullopt;
    if (!p_pl_default || p_pl_default(dev, &def) != NVML_SUCCESS || def == 0) return std::nullopt;
    if (!p_powerlimit || p_powerlimit(dev, &now) != NVML_SUCCESS) return std::nullopt;
    return static_cast<int>((static_cast<unsigned long long>(now) * 100 + def / 2) / def);
}

std::string Nvml::GpuUuid(unsigned index) {
    nvmlDevice_t dev = nullptr;
    char buf[96] = {};
    if (!inited_ || !p_uuid || p_byIndex(index, &dev) != NVML_SUCCESS || p_uuid(dev, buf, sizeof(buf)) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetUUID failed";
        return {};
    }
    return buf;
}

std::string Nvml::GpuPciBusId(unsigned index) {
    nvmlDevice_t dev = nullptr;
    nvml_pci_info_v3 info{};
    if (!inited_ || !p_pci_info || p_byIndex(index, &dev) != NVML_SUCCESS ||
        p_pci_info(dev, &info) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetPciInfo_v3 failed";
        return {};
    }
    const char* const bus_id = info.bus_id[0] ? info.bus_id : info.bus_id_legacy;
    if (!bus_id[0]) {
        error_ = "nvmlDeviceGetPciInfo_v3 returned an empty PCI bus address";
        return {};
    }
    return bus_id;
}

std::optional<unsigned> Nvml::GpuBusId(unsigned index) {
    nvmlDevice_t dev = nullptr;
    nvml_pci_info_v3 info{};
    if (!inited_ || !p_pci_info || p_byIndex(index, &dev) != NVML_SUCCESS ||
        p_pci_info(dev, &info) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetPciInfo_v3 failed";
        return std::nullopt;
    }
    return info.bus;
}

std::optional<GpuLuid> Nvml::DeviceLuid(unsigned index) {
    nvmlDevice_t dev = nullptr;
    char luid[8] = {};
    unsigned node_mask = 0;
    if (!inited_ || !p_luid || p_byIndex(index, &dev) != NVML_SUCCESS ||
        p_luid(dev, luid, &node_mask) != NVML_SUCCESS) {
        error_ = "nvmlDeviceGetLuid failed";
        return std::nullopt;
    }
    GpuLuid result{};
    std::memcpy(result.data(), luid, result.size());
    return result;
}

Nvml::~Nvml() {
    if (inited_ && p_shutdown) p_shutdown();
    if (lib_) FreeLibrary((HMODULE)lib_);
}

int Nvml::FanCount(unsigned index) {
    nvmlDevice_t dev = nullptr;
    unsigned n = 0;
    if (!inited_ || !p_fan_count || p_byIndex(index, &dev) != NVML_SUCCESS || p_fan_count(dev, &n) != NVML_SUCCESS) return 0;
    return static_cast<int>(n);
}

std::optional<std::pair<int, int>> Nvml::FanRangePct(unsigned index) {
    nvmlDevice_t dev = nullptr;
    unsigned lo = 0, hi = 0;
    if (!inited_ || !p_fan_range || p_byIndex(index, &dev) != NVML_SUCCESS || p_fan_range(dev, &lo, &hi) != NVML_SUCCESS)
        return std::nullopt;
    return std::make_pair(static_cast<int>(lo), static_cast<int>(hi));
}

bool Nvml::SetFanPct(unsigned index, int pct) {
    nvmlDevice_t dev = nullptr;
    const int fans = FanCount(index);
    if (fans == 0 || !p_fan_set || !p_fan_target || p_byIndex(index, &dev) != NVML_SUCCESS) {
        error_ = "fan control not available"; return false;
    }
    for (unsigned f = 0; f < static_cast<unsigned>(fans); ++f)
        if (p_fan_set(dev, f, static_cast<unsigned>(pct)) != NVML_SUCCESS) {
            error_ = "nvmlDeviceSetFanSpeed_v2 failed (elevated?)"; return false;
        }
    // Never trust the return code: read every fan's target back.
    for (unsigned f = 0; f < static_cast<unsigned>(fans); ++f) {
        unsigned target = 0;
        if (p_fan_target(dev, f, &target) != NVML_SUCCESS || static_cast<int>(target) != pct) {
            error_ = "fan " + std::to_string(f) + " target read back " + std::to_string(target) + " %, requested " +
                     std::to_string(pct) + " %";
            return false;
        }
    }
    return true;
}

bool Nvml::SetFanAuto(unsigned index) {
    nvmlDevice_t dev = nullptr;
    const int fans = FanCount(index);
    if (fans == 0 || !p_fan_default || !p_fan_policy || p_byIndex(index, &dev) != NVML_SUCCESS) {
        error_ = "fan control not available"; return false;
    }
    bool ok = true;
    for (unsigned f = 0; f < static_cast<unsigned>(fans); ++f) {   // every fan, even after one fails
        unsigned policy = kFanPolicyManual;
        if (p_fan_default(dev, f) != NVML_SUCCESS || p_fan_policy(dev, f, &policy) != NVML_SUCCESS || policy != kFanPolicyAuto) {
            error_ = "fan " + std::to_string(f) + " did not return to driver control";
            ok = false;
        }
    }
    return ok;
}

std::optional<FanReading> Nvml::ReadFan(unsigned index) {
    nvmlDevice_t dev = nullptr;
    unsigned policy = 0, target = 0;
    if (!inited_ || !p_fan_policy || !p_fan_target || p_byIndex(index, &dev) != NVML_SUCCESS) return std::nullopt;
    if (p_fan_policy(dev, 0, &policy) != NVML_SUCCESS || p_fan_target(dev, 0, &target) != NVML_SUCCESS) return std::nullopt;
    // The slowest fan: one stalled fan is enough to call the speed too low.
    int speed = -1;
    const int fans = FanCount(index);
    for (unsigned f = 0; p_fan_speed && f < static_cast<unsigned>(fans); ++f) {
        unsigned s = 0;
        if (p_fan_speed(dev, f, &s) != NVML_SUCCESS) { speed = -1; break; }
        speed = speed < 0 ? static_cast<int>(s) : (std::min)(speed, static_cast<int>(s));
    }
    return FanReading{policy == kFanPolicyManual, static_cast<int>(target), speed};
}

}
