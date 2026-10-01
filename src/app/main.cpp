#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/common.hpp"
#include "core/stability.hpp"
#include "core/version.hpp"
#include "hw/app_files.hpp"
#include "hw/boot_task.hpp"
#include "hw/gpu_control.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include "hw/stress.hpp"
#include <atomic>
#include <filesystem>
#include <string>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The whole argument must be an int: std::atoi would read a typo like
// "--set-core +x" as 0 and silently reset the offset.
static bool ParseIntArg(const char* text, int* out) {
    const char* end = text + std::strlen(text);
    const auto [p, ec] = std::from_chars(text, end, *out);
    return ec == std::errc() && p == end && p != text;
}

static std::optional<gao::app::GpuInfo> selected_gpu(gao::Nvml& nvml) {
    std::string why;
    const auto gpus = gao::app::enumerate_gpus(nvml, &why);
    if (!why.empty()) {
        std::printf("GPU enumeration failed: %s\n", why.c_str());
        return std::nullopt;
    }
    const auto selected = gao::app::resolve_gpu(gpus, gao::app::load_config(), &why);
    if (!selected) std::printf("GPU selection failed: %s\n", why.c_str());
    return selected;
}

static std::optional<gao::GpuControl> selected_control(gao::Nvml& nvml, gao::Nvapi& nvapi,
                                                        const gao::app::GpuInfo& gpu) {
    std::string why;
    auto control = gao::make_gpu_control(nvml, nvapi, gpu.index, &why);
    if (!control) std::printf("GPU mapping failed: %s\n", why.c_str());
    return control;
}

static int probe() {
    gao::Nvml nvml;
    if (!nvml.Init()) {
        std::printf("NVML init failed: %s\n", nvml.Error().c_str());
        return 1;
    }
    const int count = nvml.DeviceCount();
    if (count < 0) {
        std::printf("NVML device count failed: %s\n", nvml.Error().c_str());
        return 1;
    }
    std::printf("GPUs: %d (NVML indices)\n", count);
    for (int i = 0; i < count; ++i) {
        const unsigned index = static_cast<unsigned>(i);
        const std::string name = nvml.DeviceName(index);
        const std::string uuid = nvml.GpuUuid(index);
        const std::string pci_bus_id = nvml.GpuPciBusId(index);
        const gao::Telemetry t = nvml.Read(index);
        const char* const gpu_name = name.empty() ? "NVIDIA GPU" : name.c_str();
        const char* const pci = pci_bus_id.empty() ? "unknown" : pci_bus_id.c_str();
        if (!t.ok) {
            std::printf("  [%d] %s (%s, PCI %s) telemetry read failed\n", i, gpu_name, uuid.c_str(), pci);
            continue;
        }
        // Each field is -1 when the driver did not report it, even when t.ok.
        std::printf("  [%d] %s (%s, PCI %s) core=%s MHz  mem=%s MHz  temp=%s C  fan=%s  power=%s/%d W\n", i,
                    gpu_name, uuid.c_str(), pci, gao::reading(t.core_mhz).c_str(), gao::reading(t.mem_mhz).c_str(),
                    gao::reading(t.temp_c).c_str(), gao::reading(t.fan_pct, "%").c_str(),
                    gao::reading(t.power_w).c_str(), t.power_limit_w);
    }
    return 0;
}

// Shared by --set-core and --set-mem. Prints "requested X, read back Y" and
// OK/MISMATCH. Always prints the read-back, even on success -- the number
// being visible is the point, not just the verdict.
static int set_offset(const char* label, int mhz, bool core) {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    const auto gpu = selected_gpu(nvml);
    if (!gpu) return 1;
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    const auto control = selected_control(nvml, nvapi, *gpu);
    if (!control) return 1;
    const bool ok = core ? control->set_core_offset(mhz) : control->set_mem_offset(mhz);
    const auto applied = control->read_applied();
    std::printf("%s: requested %d MHz, read back ", label, mhz);
    if (applied) std::printf("%d MHz\n", core ? applied->core_mhz : applied->mem_mhz);
    else std::printf("unavailable (%s)\n", nvapi.Error().c_str());
    // Print the diagnostic before the verdict: "clamped to
    // 210 MHz" and "the call never reached an unelevated driver" both show
    // MISMATCH here, and without this line they are indistinguishable.
    if (!ok) std::printf("%s\n", nvapi.Error().c_str());
    std::printf("%s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}

static int reset() {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    const auto gpu = selected_gpu(nvml);
    if (!gpu) return 1;
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    const auto control = selected_control(nvml, nvapi, *gpu);
    if (!control) return 1;
    // Stock by choice: tell a running tray app immediately before the writes,
    // or a watchdog tick between the writes could re-apply the tune.
    gao::app::tell_tray(gao::app::TrayNotice::StockByChoice);
    const bool ok = control->reset_to_stock && control->reset_to_stock();
    const auto readback = control->read_applied();
    std::printf("reset: requested core 0 MHz, mem 0 MHz\n");
    if (readback) std::printf("read back core %d MHz, mem %d MHz\n", readback->core_mhz, readback->mem_mhz);
    else std::printf("read back unavailable (%s)\n", nvapi.Error().c_str());
    if (!ok) std::printf("%s\n", nvapi.Error().c_str());
    std::printf("power limit: default %s\n", ok ? "restored where supported" : "not confirmed");
    std::printf("%s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}

// Runs the DX11 stress load for `seconds` of GPU time, printing one line per
// second, then the verdict. Changes no settings, so Ctrl+C is always safe.
static int stress(int seconds, int max_temp_c, gao::StressSelftest selftest) {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    const auto gpu = selected_gpu(nvml);
    if (!gpu) return 1;
    const auto luid = nvml.DeviceLuid(gpu->index);
    gao::Stress load;
    if (!load.Init(luid, gpu->name, selftest)) { std::printf("stress init failed: %s\n", load.Error().c_str()); return 1; }
    std::printf("stress: %s, %d s, abort above %d C\n", load.AdapterName().c_str(), seconds, max_temp_c);

    double t = 0, next_print = 1.0;
    long long window_its = 0;
    double window_s = 0;
    auto batch = [&] {
        const gao::StressBatch b = load.Batch();
        t += b.elapsed_ms / 1000.0;
        window_s += b.elapsed_ms / 1000.0;
        window_its += b.iterations;
        return b;
    };
    auto read = [&] {
        const gao::Telemetry tel = nvml.Read(gpu->index);
        if (t >= next_print) {
            std::printf("  t=%.0fs  score=%.0f it/s  core=%s MHz  mem=%s MHz  temp=%s C  power=%s/%d W\n", t,
                        window_its / window_s, gao::reading(tel.core_mhz).c_str(), gao::reading(tel.mem_mhz).c_str(),
                        gao::reading(tel.temp_c).c_str(), gao::reading(tel.power_w).c_str(), tel.power_limit_w);
            window_its = 0;
            window_s = 0;
            next_print = t + 1.0;
        }
        return tel;
    };
    const gao::StabilityResult r = gao::run_stability(batch, read, seconds, max_temp_c);
    std::printf("VERDICT: %s  score=%.0f it/s  %.1f s  peak=%s C  avg power=%s W  avg core=%s MHz  avg mem=%s MHz\n",
                gao::verdict_name(r.verdict), r.score, r.seconds, gao::reading(r.peak_temp_c).c_str(),
                gao::reading(r.avg_power_w).c_str(), gao::reading(r.avg_core_mhz).c_str(),
                gao::reading(r.avg_mem_mhz).c_str());
    switch (r.verdict) {
        case gao::Verdict::Stable: return 0;
        case gao::Verdict::WrongResult:
        case gao::Verdict::DeviceLost: return 2;
        case gao::Verdict::TooHot:
        case gao::Verdict::NoTelemetry: return 3;
    }
    return 1;
}

static std::atomic<bool> g_abort{false};
static const gao::GpuControl* g_gpu = nullptr;   // set while --optimize runs

// Ctrl+C / Ctrl+Break do not kill the process during --optimize: they ask
// the search to stop, and the search restores stock before returning.
// Closing the window, logoff and shutdown do kill it (Windows allows ~5 s),
// so those reset to stock right here instead of leaving a candidate applied.
static BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_abort = true;
        std::printf("\nabort requested -- finishing the current probe, then restoring stock\n");
        return TRUE;
    }
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_abort = true;
        if (g_gpu && g_gpu->reset_to_stock) g_gpu->reset_to_stock();
        if (g_gpu && g_gpu->set_fan_auto) g_gpu->set_fan_auto();
        return TRUE;
    }
    return FALSE;
}

static int optimize(gao::Preset preset, const std::string& gpu_uuid, const std::optional<gao::FanCurve>& fan_curve) {
    gao::app::OptimizeHooks hooks;
    hooks.aborted = [] { return g_abort.load(); };
    hooks.log = [](const std::string& msg) { std::printf("  %s\n", msg.c_str()); };
    hooks.active_gpu = [](const gao::GpuControl* gpu) {
        g_gpu = gpu;
        SetConsoleCtrlHandler(OnConsoleCtrl, gpu ? TRUE : FALSE);
    };
    const auto out = gao::app::run_optimize(preset, gpu_uuid, hooks, fan_curve);
    if (!out.ran) { std::printf("%s\n", out.error.c_str()); return 1; }
    const gao::OptimizeResult& r = out.result;
    if (!r.ok) {
        std::printf("RESULT: not applied -- %s (%s)\n", r.reason.c_str(),
                    r.stock_restored ? "card at stock" : "reset to stock FAILED, run `gao --reset`");
        return 1;
    }
    std::printf("RESULT: power %d %%, core +%d MHz (confirmed +%d), mem +%d MHz (confirmed +%d)\n",
                r.power_pct, r.core_mhz, r.core_confirmed, r.mem_mhz, r.mem_confirmed);
    std::printf("  before: score=%.0f it/s  core=%d MHz  mem=%d MHz  peak=%d C  power=%d W\n",
                r.baseline.score, r.baseline.avg_core_mhz, r.baseline.avg_mem_mhz, r.baseline.peak_temp_c, r.baseline.avg_power_w);
    std::printf("  after:  score=%.0f it/s  core=%d MHz  mem=%d MHz  peak=%d C  power=%d W\n",
                r.soak.score, r.soak.avg_core_mhz, r.soak.avg_mem_mhz, r.soak.peak_temp_c, r.soak.avg_power_w);
    if (out.saved) std::printf("Saved: `gao --apply` re-applies it, `gao --boot on` applies it at every logon.\n");
    else std::printf("not saved: %s\n", out.save_note.c_str());
    std::printf("Applied until reboot. `gao --reset` returns to stock.\n");
    return out.saved ? 0 : 2;   // 2: tuned and applied, but --apply and boot-apply cannot use it
}

static int apply() {
    if (!gao::app::is_elevated()) { std::printf("--apply needs an elevated (administrator) shell\n"); return 1; }
    std::string why;
    if (!gao::app::prepare_state(&why)) { std::printf("%s\n", why.c_str()); return 1; }
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    const auto gpu = selected_gpu(nvml);
    if (!gpu) return 1;
    gao::Nvapi nvapi;
    if (!nvapi.Init()) { std::printf("NVAPI init failed: %s\n", nvapi.Error().c_str()); return 1; }
    gao::Config cfg = gao::app::load_config();
    gao::DeviceSettings* device = gao::find_device(cfg, gpu->uuid);
    if (!device || !device->profile) {
        std::printf("no saved profile for the selected GPU; optimize it first\n");
        return 1;
    }
    device->boot_strikes = 0;   // strikes only gate boot-apply
    const std::string driver = nvml.DriverVersion();
    const auto d = gao::decide_boot(*device, driver, gpu->uuid);
    if (d != gao::BootDecision::Apply) {
        std::printf("not applied: %s\n",
                    gao::app::decision_text(d, device->profile ? &*device->profile : nullptr, driver).c_str());
        return 1;
    }
    const auto control = selected_control(nvml, nvapi, *gpu);
    if (!control) return 1;
    if (!gao::apply_profile(*control, *device->profile, &why)) { std::printf("not applied: %s\n", why.c_str()); return 1; }
    gao::app::tell_tray(gao::app::TrayNotice::TuneApplied);   // a running tray keeps it applied from now on
    std::printf("applied %s on GPU %s\nOK\n", gao::app::profile_text(*device->profile).c_str(), gpu->name.c_str());
    return 0;
}

static int boot(bool on) {
    std::string message;
    const bool ok = on ? gao::app::enable_boot(&message) : gao::app::disable_boot(&message);
    std::printf("%s\n", message.c_str());
    return ok ? 0 : 1;
}

static int status() {
    const auto text = gao::read_file(gao::config_path());
    const gao::Config cfg = text ? gao::from_json(*text) : gao::Config{};
    std::printf("saved profiles:\n");
    size_t profile_count = 0;
    for (const gao::DeviceSettings& device : cfg.devices) {
        if (!device.profile) continue;
        ++profile_count;
        std::printf("  %s: %s (boot strikes %d/%d)\n", device.gpu.c_str(),
                    gao::app::profile_text(*device.profile).c_str(), device.boot_strikes, gao::kMaxBootStrikes);
    }
    if (profile_count == 0) std::printf("  none (run `gao --optimize`)\n");
    std::printf("selected GPU UUID: %s\n", cfg.selected_gpu.empty() ? "none" : cfg.selected_gpu.c_str());
    gao::Nvml nvml;
    const bool nvml_ok = nvml.Init();
    const std::string driver = nvml_ok ? nvml.DriverVersion() : std::string();
    const auto gpu = nvml_ok ? selected_gpu(nvml) : std::nullopt;
    if (!nvml_ok) std::printf("NVML:       unavailable (%s)\n", nvml.Error().c_str());
    if (gpu) std::printf("selected:   GPU %u, %s\n", gpu->index, gpu->name.c_str());
    const gao::Profile* profile = gpu ? gao::find_profile(cfg, gpu->uuid) : nullptr;
    if (profile) {
        std::printf("driver:     %s (%s)\n", driver.empty() ? "unknown" : driver.c_str(),
                    !driver.empty() && driver == profile->driver ? "matches" : "CHANGED -- run `gao --optimize` again");
        std::printf("profile:    %s\n", gao::app::profile_text(*profile).c_str());
    } else if (gpu) {
        std::printf("profile:    none for selected GPU\n");
    }
    // What the driver reports right now, whoever set it.
    gao::Nvapi nvapi;
    std::optional<gao::AppliedState> applied;
    if (gpu && nvapi.Init()) {
        if (const auto control = selected_control(nvml, nvapi, *gpu)) applied = control->read_applied();
    } else if (gpu) {
        std::printf("NVAPI:      unavailable (%s)\n", nvapi.Error().c_str());
    }
    if (applied)
        std::printf("applied:    power %d %%, core %+d MHz, mem %+d MHz\n", applied->power_pct, applied->core_mhz, applied->mem_mhz);
    else
        std::printf("applied:    unknown (could not read the driver)\n");
    if (gpu) if (const auto fan = nvml.ReadFan(gpu->index))
        std::printf("fans:       %s\n", fan->manual ? ("manual " + std::to_string(fan->target_pct) + " %").c_str() : "driver control");
    const auto curve = gpu ? gao::active_fan_curve(cfg, gpu->uuid) : std::nullopt;
    const gao::DeviceSettings* device = gpu ? gao::find_device(cfg, gpu->uuid) : nullptr;
    std::printf("fan curve:  %s", device && device->fan_control && curve ? "on" : "off");
    if (curve) {
        if (curve->stop_below_c) std::printf(", driver below %d C", *curve->stop_below_c);
        for (const gao::FanPoint& p : curve->points) std::printf(", %d C %d %%", p.temp_c, p.pct);
    }
    std::printf("\n");
    if (device && device->fan_min_pct > 0)
        std::printf("fan minimum: %d %% (learned: the fans stall below it)\n", device->fan_min_pct);
    std::printf("boot-apply: %s\n", gao::boot_task_exists() ? "on (logon task registered)" : "off");
    const auto installed = gao::installed_exe_path();
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::error_code ec;
    const auto build_dir = std::filesystem::path(self).parent_path();
    if (!std::filesystem::exists(installed, ec)) std::printf("boot copy:  not installed\n");
    else if (n && n < MAX_PATH && gao::files_equal(self, installed) &&
             gao::files_equal(build_dir / L"GpuAutoOptimizer.exe", gao::installed_tray_path()))
        std::printf("boot copy:  up to date\n");
    else std::printf("boot copy:  OUTDATED -- run `gao --boot on` to install this build\n");
    if (device) std::printf("strikes:    %d of %d for selected GPU\n", device->boot_strikes, gao::kMaxBootStrikes);
    const auto log = gao::read_lines(gao::boot_log_path());
    if (log && !log->empty()) std::printf("last boot:  %s\n", log->back().c_str());
    return 0;
}

// Emergency exit: every fan back to the driver, whatever set it.
static int fan_auto() {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    const auto gpu = selected_gpu(nvml);
    if (!gpu) return 1;
    const bool ok = nvml.SetFanAuto(gpu->index);
    if (!ok) std::printf("%s\n", nvml.Error().c_str());
    std::printf("fans: %s\n", ok ? "driver control -- OK" : "MISMATCH");
    return ok ? 0 : 1;
}

static int bandwidth() {
    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("NVML init failed: %s\n", nvml.Error().c_str()); return 1; }
    const auto gpu = selected_gpu(nvml);
    if (!gpu) return 1;
    const auto luid = nvml.DeviceLuid(gpu->index);
    gao::Stress load;
    if (!load.Init(luid, gpu->name)) { std::printf("stress init failed: %s\n", load.Error().c_str()); return 1; }
    const auto gbps = load.MeasureBandwidth();
    if (!gbps) { std::printf("bandwidth measurement failed: %s\n", load.Error().c_str()); return 1; }
    std::printf("memory bandwidth: %.1f GB/s (%s)\n", *gbps, load.AdapterName().c_str());
    return 0;
}

int main(int argc, char** argv) {
    // Before anything loads a DLL: System32 only (the delay-loaded
    // d3dcompiler_47.dll included), never the exe's own folder.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    // The manifest makes the process code page UTF-8; match the console so
    // paths print correctly.
    SetConsoleOutputCP(CP_UTF8);
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", gao::kProductName.data(), gao::kVersion.data());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--probe") == 0) return probe();
    if (argc > 2 && std::strcmp(argv[1], "--set-core") == 0) {
        int mhz = 0;
        if (!ParseIntArg(argv[2], &mhz)) { std::printf("--set-core expects an integer MHz value, got '%s'\n", argv[2]); return 1; }
        return set_offset("core offset", mhz, true);
    }
    if (argc > 2 && std::strcmp(argv[1], "--set-mem") == 0) {
        int mhz = 0;
        if (!ParseIntArg(argv[2], &mhz)) { std::printf("--set-mem expects an integer MHz value, got '%s'\n", argv[2]); return 1; }
        return set_offset("mem offset", mhz, false);
    }
    if (argc > 1 && std::strcmp(argv[1], "--reset") == 0) return reset();
    if (argc > 2 && std::strcmp(argv[1], "--stress") == 0) {
        int seconds = 0;
        if (!ParseIntArg(argv[2], &seconds) || seconds < 1 || seconds > 3600) {
            std::printf("--stress expects a duration of 1-3600 seconds, got '%s'\n", argv[2]);
            return 1;
        }
        int max_temp = 85;
        auto selftest = gao::StressSelftest::None;
        for (int i = 3; i < argc; i += 2) {
            if (i + 1 >= argc) { std::printf("%s expects a value\n", argv[i]); return 1; }
            if (std::strcmp(argv[i], "--max-temp") == 0) {
                if (!ParseIntArg(argv[i + 1], &max_temp) || max_temp < 40 || max_temp > 95) {
                    std::printf("--max-temp expects 40-95 C, got '%s'\n", argv[i + 1]);
                    return 1;
                }
            } else if (std::strcmp(argv[i], "--stress-selftest") == 0) {
                if (std::strcmp(argv[i + 1], "wrong") == 0) selftest = gao::StressSelftest::WrongResult;
                else { std::printf("--stress-selftest expects 'wrong', got '%s'\n", argv[i + 1]); return 1; }
            } else {
                std::printf("unknown --stress option '%s'\n", argv[i]);
                return 1;
            }
        }
        return stress(seconds, max_temp, selftest);
    }
    if (argc > 1 && std::strcmp(argv[1], "--bandwidth") == 0) return bandwidth();
    if (argc > 1 && std::strcmp(argv[1], "--apply") == 0) return apply();
    if (argc > 2 && std::strcmp(argv[1], "--boot") == 0) {
        if (std::strcmp(argv[2], "on") == 0) return boot(true);
        if (std::strcmp(argv[2], "off") == 0) return boot(false);
        std::printf("--boot expects on or off, got '%s'\n", argv[2]);
        return 1;
    }
    if (argc > 2 && std::strcmp(argv[1], "--fan") == 0) {
        if (std::strcmp(argv[2], "auto") == 0) return fan_auto();
        std::printf("--fan expects auto, got '%s'\n", argv[2]);
        return 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "--status") == 0) return status();
    if (argc > 1 && std::strcmp(argv[1], "--optimize") == 0) {
        gao::Preset preset = gao::Preset::BestOfMyGpu;
        if (argc > 2) {
            const std::string p = argv[2];
            if (p == "best") preset = gao::Preset::BestOfMyGpu;
            else if (p == "quiet") preset = gao::Preset::Quiet;
            else if (p == "cool") preset = gao::Preset::CoolAndEfficient;
            else if (p == "max") preset = gao::Preset::MaxPerformance;
            else { std::printf("--optimize expects best, quiet, cool or max, got '%s'\n", argv[2]); return 1; }
        }
        std::optional<gao::FanCurve> fan_curve;
        if (argc > 3) {
            const auto fp = argc > 4 && std::strcmp(argv[3], "--fan-curve") == 0 ? gao::fan_preset_from_name(argv[4]) : std::nullopt;
            if (!fp) { std::printf("--optimize <profile> accepts --fan-curve silent|normal|cool|aggressive\n"); return 1; }
            fan_curve = gao::fan_preset_curve(*fp);
        }
        return optimize(preset, "", fan_curve);
    }
    std::printf("usage: gao [--version | --probe | --set-core <mhz> | --set-mem <mhz> | --reset\n"
                "            | --stress <sec> [--max-temp <c>] | --bandwidth\n"
                "            | --optimize [best|quiet|cool|max [--fan-curve silent|normal|cool|aggressive]]\n"
                "            | --apply | --boot on|off | --fan auto | --status]\n");
    return argc > 1 ? 1 : 0;
}
