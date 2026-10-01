#pragma once
// Application logic shared by gao.exe (CLI) and GpuAutoOptimizer.exe (window
// and tray): everything between the core/hw layers and a user interface.
// Nothing here prints; callers decide how to show messages.
#include "core/boot.hpp"
#include "core/config.hpp"
#include "core/fan_curve.hpp"
#include "core/manual.hpp"
#include "core/objectives.hpp"
#include "core/search.hpp"
#include "core/types.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gao { class Nvml; }

namespace gao::app {

struct GpuInfo {
    unsigned index = 0;         // current NVML index; never persisted
    std::string uuid;           // stable device identity
    std::string name;
    std::string pci_bus_id;     // full PCI address, for display and diagnostics
};

std::string gpu_label(const GpuInfo& gpu);
bool is_elevated();
std::string now_text();   // local time, "YYYY-MM-DD HH:MM"
std::string log_timestamp_text();   // local time, "YYYY-MM-DD HH:MM:SS.fff"
Config load_config();
bool save_config(const Config& c);
std::vector<GpuInfo> enumerate_gpus(Nvml& nvml, std::string* why = nullptr);
std::optional<GpuInfo> resolve_gpu(const std::vector<GpuInfo>& gpus, const Config& config, std::string* why = nullptr);
void boot_log(const std::string& msg);
// Creates, or verifies and re-secures, the admin-only state folder. Every
// elevated command that reads or writes state calls this first.
bool prepare_state(std::string* why);
std::string profile_text(const Profile& p);
std::string decision_text(BootDecision d, const Profile* profile, const std::string& driver);

// Held (process-wide, across gao.exe and the tray app) for the whole of an
// optimize: the watchdog must not re-apply the saved tune under a running
// search, and two searches must not run at once.
class TuningLock {
public:
    TuningLock();
    ~TuningLock();
    TuningLock(const TuningLock&) = delete;
    TuningLock& operator=(const TuningLock&) = delete;
    bool owned() const { return owned_; }
private:
    void* handle_ = nullptr;
    bool owned_ = false;
};
// True while another thread or process holds the TuningLock.
bool tuning_in_progress();

// What the command line tells a running tray app, so its watchdog follows:
// stock on purpose (gao --reset) stops it; the tune applied (gao --apply)
// starts it again.
enum class TrayNotice : unsigned { StockByChoice = 0, TuneApplied = 1 };
void tell_tray(TrayNotice notice);
// The registered window message tell_tray() posts; the notice is its wParam.
unsigned tray_notice_message();

struct OptimizeHooks {
    std::function<bool()> aborted;                        // polled between probes
    std::function<void(const std::string&)> log;          // one line per event
    // The GPU being tuned while a run is active (nullptr before and after), so
    // an emergency handler can reset it.
    std::function<void(const GpuControl*)> active_gpu;
};

struct OptimizeOutcome {
    bool ran = false;          // false: stopped before tuning; `error` says why
    std::string error;
    OptimizeResult result;     // valid when ran
    bool saved = false;        // the profile was saved for --apply / boot-apply
    std::string save_note;     // why it was not saved, when !saved
};

// --optimize from start to end: state folder, drivers, journal checks, the
// search, and saving the profile. Needs elevation.
// fan_curve: the curve to drive the fans with during the run and to save
// as the tested curve; empty means the profile's own (default_curve).
OptimizeOutcome run_optimize(Preset preset, const std::string& gpu_uuid, const OptimizeHooks& hooks,
                             const std::optional<FanCurve>& fan_curve = {});

struct BootApplyOutcome {
    bool applied = false;
    std::vector<std::string> applied_gpus;
    std::vector<std::string> strike_gpus;
    std::string message;       // what happened, as written to boot.log
    bool unclean_reset = false;   // an unfinished journal entry forced a reset to stock first
};

// The logon half of boot-apply, independently for every saved GPU profile.
// When the crash journal holds an unfinished entry (a freeze or a killed
// process between begin and complete), every reachable GPU is reset to stock
// first and nothing is applied this boot: unclean_reset is true and the
// freeze becomes a ceiling for the next run.
// After the grace period: clears only the strikes recorded for this logon.
BootApplyOutcome apply_at_logon();
void clear_boot_strikes(const std::vector<std::string>& gpu_uuids);

// Manual tuning through the same guards as --optimize: elevation, the tuning
// lock, the admin-only state folder, the crash journal and verified writes.
// Any failure ends at stock.
struct ManualApplyOutcome {
    bool ok = false;
    std::string message;
    std::optional<AppliedState> readback;
};

ManualApplyOutcome apply_manual_tune(const std::string& gpu_uuid, const ManualTune& tune);
// Saves a hand-tuned value set as the GPU's profile (one profile per GPU).
// validated=false saves anyway with a warning; the caller shows the badge.
bool save_manual_profile(const std::string& gpu_uuid, Preset preset, const ManualTune& tune, bool validated,
                         const std::string& validation_note, std::string* message);
// Driver power range for the Manual page ({min, max} percent of default);
// {50, 150} when the card reports none.
std::pair<int, int> manual_power_range(const std::string& gpu_uuid);

// Installs the exes into Program Files and registers the logon task (true),
// or removes both. `message` describes the outcome either way.
bool enable_boot(std::string* message);
bool disable_boot(std::string* message);

}
