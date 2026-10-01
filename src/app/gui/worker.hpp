#pragma once
#include "app/common.hpp"
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

}
