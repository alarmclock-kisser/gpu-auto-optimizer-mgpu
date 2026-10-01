#pragma once
#include "app/common.hpp"
#include "core/stability.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gao::gui {

// Runs one optimize on a background thread so the window stays responsive.
// The UI thread polls snapshot(); `wake` is called (from the worker thread)
// whenever there is something new to draw.
class OptimizeWorker {
public:
    struct Snapshot {
        bool running = false;
        std::vector<std::string> log;
        std::optional<app::OptimizeOutcome> outcome;   // set once the run has ended
    };

    explicit OptimizeWorker(std::function<void()> wake) : wake_(std::move(wake)) {}
    ~OptimizeWorker();

    bool start(Preset preset, std::string gpu_uuid, std::optional<FanCurve> fan_curve);
    // false when a run is already active
    void abort() { abort_ = true; }     // the run restores stock before it ends
    bool running() const { return running_; }
    Snapshot snapshot() const;

private:
    std::function<void()> wake_;
    mutable std::mutex mu_;
    std::vector<std::string> log_;
    std::optional<app::OptimizeOutcome> outcome_;
    std::atomic<bool> running_{false};
    std::atomic<bool> abort_{false};
    std::jthread thread_;
};

// A short read-only stability check for a hand-applied tune: 60 s of the
// same stress load the optimizer uses, at the currently applied settings.
// Changes nothing, so stopping it early is always safe.
class ManualWorker {
public:
    struct Snapshot {
        bool running = false;
        std::optional<StabilityResult> result;
        std::string error;
    };

    explicit ManualWorker(std::function<void()> wake) : wake_(std::move(wake)) {}
    ~ManualWorker();
    ManualWorker(const ManualWorker&) = delete;
    ManualWorker& operator=(const ManualWorker&) = delete;

    bool start(std::string gpu_uuid, int max_temp_c);
    bool running() const { return running_; }
    Snapshot snapshot() const;

private:
    std::function<void()> wake_;
    mutable std::mutex mu_;
    std::optional<StabilityResult> result_;
    std::string error_;
    std::atomic<bool> running_{false};
    std::jthread thread_;
};

}
