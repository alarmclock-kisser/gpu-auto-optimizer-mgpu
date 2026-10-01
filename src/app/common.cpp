#include "app/common.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "core/journal.hpp"
#include "core/fan_curve.hpp"
#include "core/stability.hpp"
#include "core/task_xml.hpp"
#include "hw/app_files.hpp"
#include "hw/boot_task.hpp"
#include "hw/gpu_control.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include "hw/stress.hpp"
#include <ctime>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace gao::app {

namespace {
constexpr wchar_t kTuningMutex[] = L"Local\\GpuAutoOptimizer.Tuning";
constexpr wchar_t kTrayWindowClass[] = L"GpuAutoOptimizerWindow";
}

TuningLock::TuningLock() {
    handle_ = CreateMutexW(nullptr, FALSE, kTuningMutex);
    if (!handle_) return;
    // Not 0: tuning_in_progress() holds the mutex for an instant while it looks.
    const DWORD r = WaitForSingleObject(handle_, 100);
    owned_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;   // abandoned: the last owner died mid-run
}

TuningLock::~TuningLock() {
    if (owned_) ReleaseMutex(handle_);
    if (handle_) CloseHandle(handle_);
}

bool tuning_in_progress() {
    const HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kTuningMutex);
    if (!h) return false;
    const DWORD r = WaitForSingleObject(h, 0);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(h);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

unsigned tray_notice_message() {
    static const UINT msg = RegisterWindowMessageW(L"GpuAutoOptimizer.TrayNotice");
    return msg;
}

void tell_tray(TrayNotice notice) {
    if (const HWND tray = FindWindowW(kTrayWindowClass, nullptr))
        PostMessageW(tray, tray_notice_message(), static_cast<WPARAM>(notice), 0);
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

std::string now_text() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string log_timestamp_text() {
    const auto now = std::chrono::system_clock::now();
    const std::chrono::system_clock::time_point second = std::chrono::floor<std::chrono::seconds>(now);
    const std::time_t t = std::chrono::system_clock::to_time_t(second);
    const int milliseconds = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - second).count());
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, milliseconds);
    return buf;
}

Config load_config() {
    const auto text = read_file(config_path());
    return text ? from_json(*text) : Config{};
}

bool save_config(const Config& c) {
    return !config_path().empty() && write_file_atomic(config_path(), to_json(c));
}

std::vector<GpuInfo> enumerate_gpus(Nvml& nvml, std::string* why) {
    std::vector<GpuInfo> gpus;
    const int count = nvml.DeviceCount();
    if (count < 0) {
        if (why) *why = "NVML device enumeration failed: " + nvml.Error();
        return gpus;
    }
    for (int i = 0; i < count; ++i) {
        const unsigned index = static_cast<unsigned>(i);
        const std::string uuid = nvml.GpuUuid(index);
        if (uuid.empty()) {
            if (why) *why = "NVML did not report a UUID for GPU " + std::to_string(i) + ": " + nvml.Error();
            return {};
        }
        std::string name = nvml.DeviceName(index);
        if (name.empty()) name = "NVIDIA GPU " + std::to_string(i);
        gpus.push_back({index, uuid, std::move(name), nvml.GpuPciBusId(index)});
    }
    return gpus;
}

std::string gpu_label(const GpuInfo& gpu) {
    return "GPU " + std::to_string(gpu.index) + " (NVML) | " + gpu.name + " | PCI " +
           (gpu.pci_bus_id.empty() ? std::string("unknown") : gpu.pci_bus_id);
}

std::optional<GpuInfo> resolve_gpu(const std::vector<GpuInfo>& gpus, const Config& config, std::string* why) {
    if (gpus.empty()) {
        if (why) *why = "no NVIDIA GPUs are available";
        return std::nullopt;
    }
    if (config.selected_gpu.empty()) return gpus.front();
    const auto it = std::find_if(gpus.begin(), gpus.end(),
                                 [&](const GpuInfo& gpu) { return gpu.uuid == config.selected_gpu; });
    if (it == gpus.end()) {
        if (why) *why = "the selected GPU is unavailable; select an installed GPU in the dashboard";
        return std::nullopt;
    }
    return *it;
}

void boot_log(const std::string& msg) {
    append_line_durable(boot_log_path(), log_timestamp_text() + "  " + msg);
}

bool prepare_state(std::string* why) { return ensure_app_dir(why); }

std::string profile_text(const Profile& p) {
    return std::string(preset_name(p.preset)) + ": power " + std::to_string(p.power_pct) + " %, core +" +
           std::to_string(p.core_mhz) + " MHz, mem +" + std::to_string(p.mem_mhz) + " MHz (driver " + p.driver +
           ", saved " + p.saved_at + ")";
}

std::string decision_text(BootDecision d, const Profile* profile, const std::string& driver) {
    switch (d) {
        case BootDecision::NoProfile: return "no saved profile; run an optimize first";
        case BootDecision::TooManyStrikes:
            return "disabled after " + std::to_string(kMaxBootStrikes) + " crashes; turn boot-apply on again to retry";
        case BootDecision::DriverChanged:
            if (driver.empty()) return "driver version unknown (NVML did not report it); not applied";
            return "driver changed (" + (profile ? profile->driver : std::string("unknown")) + " -> " + driver +
                   "); optimize again";
        case BootDecision::GpuChanged: return "this is not the card the profile was tested on; optimize again";
        case BootDecision::Apply: return "apply";
    }
    return "unknown";
}

OptimizeOutcome run_optimize(Preset preset, const std::string& requested_gpu_uuid, const OptimizeHooks& hooks,
                             const std::optional<FanCurve>& fan_curve) {
    OptimizeOutcome out;
    auto log = [&](const std::string& m) { if (hooks.log) hooks.log(m); };
    auto fail = [&](const std::string& why) { out.error = why; return out; };
    if (!is_elevated()) return fail("optimizing changes clocks and power limits and needs administrator rights");
    const TuningLock lock;
    if (!lock.owned()) return fail("another optimize is already running (in the app or on the command line)");
    std::string why;
    if (!prepare_state(&why)) return fail(why);
    Nvml nvml;
    if (!nvml.Init()) return fail("NVML init failed: " + nvml.Error());
    const Config current = load_config();
    std::string gpu_why;
    const auto gpus = enumerate_gpus(nvml, &gpu_why);
    if (!gpu_why.empty()) return fail(gpu_why);
    const std::optional<GpuInfo> selected = requested_gpu_uuid.empty()
                                                ? resolve_gpu(gpus, current, &gpu_why)
                                                : [&]() -> std::optional<GpuInfo> {
                                                      const auto it = std::find_if(
                                                          gpus.begin(), gpus.end(), [&](const GpuInfo& gpu) {
                                                              return gpu.uuid == requested_gpu_uuid;
                                                          });
                                                      return it == gpus.end() ? std::nullopt : std::optional<GpuInfo>(*it);
                                                  }();
    if (!selected) return fail(gpu_why.empty() ? "the selected GPU is unavailable" : gpu_why);
    const std::string& gpu_uuid = selected->uuid;
    const GpuInfo& gpu_info = *selected;
    Nvapi nvapi;
    if (!nvapi.Init()) return fail("NVAPI init failed: " + nvapi.Error());
    const auto luid = nvml.DeviceLuid(gpu_info.index);
    Stress load;
    if (!load.Init(luid, gpu_info.name)) return fail("stress init failed: " + load.Error());
    std::string control_why;
    const auto control = make_gpu_control(nvml, nvapi, gpu_info.index, &control_why);
    if (!control) return fail("could not map the selected GPU between NVML and NVAPI: " + control_why);
    const GpuControl gpu = *control;
    // The profile's curve drives the fans for the whole run, so the clocks it
    // finds hold at the temperatures that curve produces.
    const FanCurve curve = fan_curve.value_or(default_curve(preset));
    FanDriver fans(gpu, curve, objectives_for(preset).max_temp_c, fan_min_for(load_config(), gpu_uuid, gpu.fan_min_pct));
    struct FanRelease {
        FanDriver& f;
        ~FanRelease() { f.release(); }   // every exit: done, aborted, failed or thrown
    } fan_release{fans};
    if (!gpu.set_fan_pct) log("fans: not controllable on this card; the driver keeps them");

    const auto path = journal_path();
    if (path.empty()) return fail("the ProgramData folder could not be resolved; cannot keep the crash journal");
    const auto lines = read_lines(path);
    if (!lines) return fail("the crash journal " + path.string() + " exists but cannot be read; not tuning without it");
    Journal journal(*lines, [&path](const std::string& l) { return append_line_durable(path, l); });
    // Prove the journal is writable before any clock is touched; the parser
    // ignores lines without an id, so this one never becomes a ceiling.
    if (!append_line_durable(path, "{\"session\":\"" + now_text() + "\"}"))
        return fail("cannot write the crash journal " + path.string() + "; not tuning without it");
    for (const auto& f : journal.freezes())
        log("warning: a previous run froze the machine at " + f + "; staying below it from now on");
    if (!gpu.set_power_limit) log("power limit: not adjustable on this card, skipped");

    OptimizeIo io;
    io.probe = [&](double seconds, int max_temp) {
        auto read = [&] {
            const Telemetry t = gpu.read();
            const FanMode before = fans.state().mode;
            const FanState now = fans.tick(t.temp_c, t.power_w, std::chrono::steady_clock::now());
            if (now.mode != before && now.mode == FanMode::Failed) log("fans: a write did not verify; the driver has them again");
            if (now.mode != before && now.mode == FanMode::Foreign) log("fans: another program set them; leaving them alone");
            return t;
        };
        return run_stability([&] { return load.Batch(); }, read, seconds, max_temp);
    };
    io.aborted = hooks.aborted;
    io.log = hooks.log;
    io.bandwidth = [&] {
        const auto gbps = load.MeasureBandwidth();
        if (!gbps) log("bandwidth measurement failed: " + load.Error());
        return gbps;
    };
    if (hooks.active_gpu) hooks.active_gpu(&gpu);
    try {
        out.result = optimize(gpu, objectives_for(preset), journal, io);
    } catch (...) {   // never leave a candidate applied, whatever went wrong
        if (hooks.active_gpu) hooks.active_gpu(nullptr);
        if (gpu.reset_to_stock) gpu.reset_to_stock();
        throw;
    }
    if (hooks.active_gpu) hooks.active_gpu(nullptr);
    out.ran = true;
    if (!out.result.ok) return out;

    const std::string driver = nvml.DriverVersion();
    if (driver.empty() || gpu_uuid.empty()) {
        out.save_note = "NVML did not report the driver version or GPU id, so the profile could never be re-applied";
        return out;
    }
    Config cfg = load_config();
    cfg.selected_gpu = gpu_uuid;
    DeviceSettings& device = ensure_device(cfg, gpu_uuid);
    device.profile = Profile{preset, out.result.power_pct, out.result.core_mhz, out.result.mem_mhz, driver, gpu_uuid, now_text()};
    device.fan_curve.reset();   // a new tune starts from its own curve
    // Only a curve that actually drove the fans counts as tested.
    if (gpu.set_fan_pct && fans.state().mode != FanMode::Failed && fans.state().mode != FanMode::Foreign) {
        device.profile->fan_curve = curve;   // what the run was tested with
        device.fan_control = true;
        if (fans.state().min_pct > fan_min_for(cfg, gpu_uuid, gpu.fan_min_pct)) {
            device.fan_min_pct = fans.state().min_pct;
        }
    }
    device.boot_strikes = 0;
    out.saved = save_config(cfg);
    if (!out.saved) out.save_note = "could not write " + config_path().string();
    return out;
}

BootApplyOutcome apply_at_logon() {
    BootApplyOutcome out;
    auto record = [&](const std::string& msg) {
        if (!out.message.empty()) out.message += "\n";
        out.message += msg;
        boot_log(msg);
    };
    Nvml nvml;
    if (!nvml.Init()) {
        record("NVML init failed: " + nvml.Error());
        return out;
    }
    Config cfg = load_config();
    const std::string driver = nvml.DriverVersion();
    std::string list_why;
    const std::vector<GpuInfo> gpus = enumerate_gpus(nvml, &list_why);
    if (!list_why.empty()) {
        record(list_why);
        return out;
    }
    // Protection fallback: an unfinished journal entry means the last run --
    // optimize or manual -- froze or was killed between begin and complete.
    // Reset every reachable GPU to stock as early as possible and apply
    // nothing this boot. The freeze stays a ceiling, so the next run tests
    // one step below it instead of crashing again.
    const auto jpath = journal_path();
    const auto jlines = jpath.empty() ? std::optional<std::vector<std::string>>{}
                                      : read_lines(jpath);
    if (!jlines) {
        record("crash journal unreadable; nothing applied without it");
        return out;
    }
    const Journal journal(*jlines, [](const std::string&) { return true; });
    if (!journal.freezes().empty()) {
        out.unclean_reset = true;
        std::string frozen;
        for (const auto& f : journal.freezes()) frozen += (frozen.empty() ? "" : ", ") + f;
        Nvapi reset_api;
        if (!reset_api.Init()) {
            record("unclean shutdown at " + frozen + "; NVAPI init failed (" + reset_api.Error() +
                   "), nothing applied");
            return out;
        }
        for (const GpuInfo& info : gpus) {
            std::string why;
            const auto control = make_gpu_control(nvml, reset_api, info.index, &why);
            if (!control) {
                record("GPU " + info.uuid + " could not be reset to stock: " + why);
                continue;
            }
            const bool ok = control->reset_to_stock && control->reset_to_stock();
            record("GPU " + info.uuid + (ok ? " reset to stock after an unclean shutdown at " + frozen
                                            : " reset to stock FAILED after an unclean shutdown; run `gao --reset`"));
        }
        record("nothing applied this boot; the next run stays below " + frozen);
        return out;
    }
    Nvapi nvapi;
    bool nvapi_ready = false;
    for (DeviceSettings& device : cfg.devices) {
        if (!device.profile) continue;
        const auto gpu = std::find_if(gpus.begin(), gpus.end(),
                                      [&](const GpuInfo& item) { return item.uuid == device.gpu; });
        const std::string actual_gpu = gpu == gpus.end() ? std::string() : gpu->uuid;
        const BootDecision decision = decide_boot(device, driver, actual_gpu);
        if (decision != BootDecision::Apply) {
            record("GPU " + device.gpu + " not applied: " + decision_text(decision, &*device.profile, driver));
            continue;
        }
        if (!nvapi_ready) {
            if (!nvapi.Init()) {
                record("NVAPI init failed: " + nvapi.Error());
                break;
            }
            nvapi_ready = true;
        }
        std::string why;
        const auto control = make_gpu_control(nvml, nvapi, gpu->index, &why);
        if (!control) {
            record("GPU " + device.gpu + " could not be mapped between NVML and NVAPI: " + why);
            continue;
        }
        ++device.boot_strikes;
        if (!save_config(cfg)) {
            record("GPU " + device.gpu + " could not record the crash strike; not applied");
            continue;
        }
        out.strike_gpus.push_back(device.gpu);
        if (!apply_profile(*control, *device.profile, &why)) {
            record("GPU " + device.gpu + " apply failed: " + why);
            continue;
        }
        out.applied = true;
        out.applied_gpus.push_back(device.gpu);
        record("GPU " + device.gpu + " applied " + profile_text(*device.profile) +
               ", strike " + std::to_string(device.boot_strikes) + " clears in 2 minutes");
    }
    if (out.message.empty()) record("no saved GPU profiles; run an optimize first");
    return out;
}

void clear_boot_strikes(const std::vector<std::string>& gpu_uuids) {
    if (gpu_uuids.empty()) return;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto text = read_file(config_path());
        Config latest = text ? from_json(*text) : Config{};
        bool changed = false;
        for (const std::string& gpu : gpu_uuids) {
            if (DeviceSettings* device = find_device(latest, gpu); device && device->profile) {
                device->boot_strikes = 0;
                changed = true;
            }
        }
        if (!changed) {
            boot_log("could not re-read gao.json to clear the boot strikes; they stay");
            return;
        }
        if (save_config(latest)) { boot_log("ran 2 minutes without a crash; GPU boot strikes cleared"); return; }
        Sleep(1000);
    }
    boot_log("could not save gao.json to clear the boot strikes; they stay");
}

bool enable_boot(std::string* message) {
    auto say = [&](const std::string& m, bool ok) { if (message) *message = m; return ok; };
    if (!is_elevated()) return say("boot-apply needs administrator rights", false);
    std::string why;
    if (!prepare_state(&why)) return say(why, false);
    const std::string sid = current_user_sid();
    if (sid.empty()) return say("could not determine the current user's SID", false);
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return say("could not find this program's own path", false);
    if (!install_app(std::filesystem::path(self).parent_path(), &why))
        return say(why, false);
    // The logon task starts the tray app, which applies the tune and stays
    // resident to keep it applied: no time limit.
    const auto exe = installed_tray_path();
    const auto u8 = exe.u8string();
    const std::string xml = boot_task_xml(std::string(u8.begin(), u8.end()), "--tray", sid, "PT0S");
    const auto xml_path = app_dir() / L"BootApply.xml";
    if (!write_utf16_file(xml_path, xml)) return say("could not write " + xml_path.string(), false);
    const int code = boot_task_create_xml(xml_path);
    std::error_code ec;
    std::filesystem::remove(xml_path, ec);
    if (code != 0) return say("could not create the task (schtasks exit " + std::to_string(code) + ")", false);
    Config cfg = load_config();
    for (DeviceSettings& device : cfg.devices) device.boot_strikes = 0;
    if (!save_config(cfg)) return say("task created, but could not reset the strike counter", false);
    return say("boot-apply on: " + exe.string() + " --tray runs at every logon and keeps the tune applied (strikes reset)" +
               (std::any_of(cfg.devices.begin(), cfg.devices.end(),
                            [](const DeviceSettings& device) { return device.profile.has_value(); })
                    ? ""
                    : "; note: there are no saved GPU profiles yet, optimize first"),
               true);
}

bool disable_boot(std::string* message) {
    auto say = [&](const std::string& m, bool ok) { if (message) *message = m; return ok; };
    if (!is_elevated()) return say("boot-apply needs administrator rights", false);
    std::string why;
    if (!prepare_state(&why)) return say(why, false);
    const int code = boot_task_remove();
    const bool removed_now = uninstall_app();
    return say(std::string("boot-apply off: task ") + (code == 0 ? "removed" : "not removed") + ", installed copy " +
                   (removed_now ? "removed" : "in use by the running tray app; it is removed at the next restart"),
               code == 0);
}

std::pair<int, int> manual_power_range(const std::string& gpu_uuid) {
    Nvml nvml;
    if (!nvml.Init()) return {kManualPowerHardMin, kManualPowerHardMax};
    const auto gpus = enumerate_gpus(nvml, nullptr);
    const auto it = std::find_if(gpus.begin(), gpus.end(),
                                 [&](const GpuInfo& gpu) { return gpu.uuid == gpu_uuid; });
    if (it == gpus.end()) return {kManualPowerHardMin, kManualPowerHardMax};
    const auto range = nvml.PowerLimitRangePct(it->index);
    if (!range || range->first >= range->second) return {kManualPowerHardMin, kManualPowerHardMax};
    return *range;
}

ManualApplyOutcome apply_manual_tune(const std::string& gpu_uuid, const ManualTune& tune) {
    ManualApplyOutcome out;
    if (!is_elevated()) {
        out.message = "manual apply changes clocks and power limits and needs administrator rights";
        return out;
    }
    const TuningLock lock;
    if (!lock.owned()) {
        out.message = "another optimize is already running (in the app or on the command line)";
        return out;
    }
    std::string why;
    if (!prepare_state(&why)) {
        out.message = why;
        return out;
    }
    Nvml nvml;
    if (!nvml.Init()) {
        out.message = "NVML init failed: " + nvml.Error();
        return out;
    }
    const auto gpus = enumerate_gpus(nvml, &why);
    if (!why.empty()) {
        out.message = why;
        return out;
    }
    const auto it = std::find_if(gpus.begin(), gpus.end(),
                                 [&](const GpuInfo& gpu) { return gpu.uuid == gpu_uuid; });
    if (it == gpus.end()) {
        out.message = "the selected GPU is unavailable";
        return out;
    }
    Nvapi nvapi;
    if (!nvapi.Init()) {
        out.message = "NVAPI init failed: " + nvapi.Error();
        return out;
    }
    std::string control_why;
    const auto control = make_gpu_control(nvml, nvapi, it->index, &control_why);
    if (!control) {
        out.message = "could not map the selected GPU between NVML and NVAPI: " + control_why;
        return out;
    }
    const auto path = journal_path();
    if (path.empty()) {
        out.message = "the ProgramData folder could not be resolved; cannot keep the crash journal";
        return out;
    }
    const auto lines = read_lines(path);
    if (!lines) {
        out.message = "the crash journal exists but cannot be read; not tuning without it";
        return out;
    }
    Journal journal(*lines, [&path](const std::string& l) { return append_line_durable(path, l); });
    if (!append_line_durable(path, "{\"session\":\"manual " + now_text() + "\"}")) {
        out.message = "cannot write the crash journal; not tuning without it";
        return out;
    }
    const auto range = nvml.PowerLimitRangePct(it->index).value_or(std::pair{kManualPowerHardMin, kManualPowerHardMax});
    std::string apply_why;
    if (!apply_manual(*control, tune, &journal, range.first, range.second, &apply_why)) {
        out.message = apply_why;
        boot_log("manual apply failed on GPU " + gpu_uuid + ": " + apply_why);
        return out;
    }
    out.readback = control->read_applied();
    const int core = manual_effective_core(tune);
    const int mem = manual_effective_mem(tune);
    const int power = manual_effective_power(tune);
    out.ok = true;
    out.message = "applied power " + std::to_string(power) + " %, core +" + std::to_string(core) + " MHz, mem +" +
                  std::to_string(mem) + " MHz";
    boot_log("manual apply on GPU " + gpu_uuid + ": " + out.message);
    tell_tray(TrayNotice::TuneApplied);
    return out;
}

bool save_manual_profile(const std::string& gpu_uuid, Preset preset, const ManualTune& tune, bool validated,
                         const std::string& validation_note, std::string* message) {
    auto say = [&](const std::string& m, bool ok) {
        if (message) *message = m;
        return ok;
    };
    const ManualTune snapped = manual_snap(tune);
    std::string range_why;
    if (!manual_in_range(snapped, kManualPowerHardMin, kManualPowerHardMax, &range_why)) return say(range_why, false);
    Nvml nvml;
    const std::string driver = nvml.Init() ? nvml.DriverVersion() : std::string();
    if (driver.empty() || gpu_uuid.empty())
        return say("NVML did not report the driver version or GPU id, so the profile could never be re-applied", false);
    std::string why;
    if (!prepare_state(&why)) return say(why, false);
    Config cfg = load_config();
    cfg.selected_gpu = gpu_uuid;
    DeviceSettings& device = ensure_device(cfg, gpu_uuid);
    device.profile = Profile{preset,
                             manual_effective_power(snapped),
                             manual_effective_core(snapped),
                             manual_effective_mem(snapped),
                             driver,
                             gpu_uuid,
                             now_text() + (validated ? " (validated: " + validation_note + ")" : " (not validated)"),
                             std::nullopt,
                             true};   // hand-tuned: the dashboard selects no preset for it
    device.boot_strikes = 0;
    if (!save_config(cfg)) return say("could not write " + config_path().string(), false);
    return say(std::string("saved ") + profile_text(*device.profile), true);
}

}
