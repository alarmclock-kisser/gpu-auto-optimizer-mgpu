#pragma once
#include "app/gui/worker.hpp"
#include "core/fan_curve.hpp"
#include "core/config.hpp"
#include "core/types.hpp"
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gao::gui {

enum class Page { Dashboard, Optimize, Fan, About };
enum class Screen { Choose, Run, Results };   // the Optimize page's state

// Something the app did or was told this session, shown in the log with the
// boot.log lines.
struct Note {
    std::string time;   // Full local timestamp, for sorting with boot.log
    std::string text;
    bool warn = false;
};

// Everything the window shows, refreshed by the main loop about once a second.
struct UiState {
    Page page = Page::Dashboard;
    Screen screen = Screen::Choose;
    Preset preset = Preset::BestOfMyGpu;
    bool elevated = false;

    std::vector<app::GpuInfo> gpus;
    std::string selected_gpu;
    std::string gpu_error;
    std::string driver;
    Telemetry telemetry;
    bool telemetry_ready = false;
    std::deque<float> temp_history;    // last minute, one sample per second
    std::deque<float> power_history;

    std::optional<Profile> profile;
    bool has_profiles = false;
    bool profile_driver_ok = false;
    bool profile_gpu_ok = false;
    std::optional<AppliedState> applied;
    bool boot_on = false;
    int strikes = 0;
    std::vector<std::string> boot_log;   // the last lines of boot.log, oldest first
    std::vector<Note> notes;             // this session, oldest first

    bool fan_available = false;           // elevated and the card has fan control
    bool fan_control = false;
    std::optional<FanCurve> fan_curve;    // the active curve
    std::optional<FanCurve> fan_tested;   // what the tune was tested with
    int fan_min_pct = 0;
    int fan_max_temp_c = 75;
    FanState fan_state;
    std::optional<FanPreset> optimize_fan;   // fan curve for the next run; empty: the profile's own
};

struct UiActions {
    std::function<void(Preset)> optimize;
    std::function<void()> abort;
    std::function<void()> restart_elevated;
    std::function<void()> apply_profile;
    std::function<void()> revert_to_stock;
    std::function<void(bool)> set_boot;
    std::function<void()> detect_gpu;
    std::function<void(const std::string&)> select_gpu;
    std::function<void(const FanCurve&)> set_fan_curve;   // saves it as the active curve
    std::function<void(bool)> set_fan_control;
    std::function<void()> reset_fan_curve;                // back to the tested/default curve
};

// Fonts (Segoe UI with Segoe Fluent Icons merged in) and the colour scheme.
// Call load_fonts() once before the first frame, apply_style() on every DPI change.
void load_fonts(const std::string& fonts_dir);
void apply_style(float dpi_scale);

void push_history(std::deque<float>& h, float v);
void draw_ui(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act);

}
