#include "app/gui/worker.hpp"
#include <exception>

namespace gao::gui {

OptimizeWorker::~OptimizeWorker() {
    abort_ = true;   // a closing window never leaves a candidate applied: the run resets to stock
}

bool OptimizeWorker::start(Preset preset, std::string gpu_uuid, std::optional<FanCurve> fan_curve) {
    if (running_.exchange(true)) return false;
    if (thread_.joinable()) thread_.join();   // the previous run's thread has already finished
    {
        std::lock_guard lock(mu_);
        log_.clear();
        outcome_.reset();
    }
    abort_ = false;
    thread_ = std::jthread([this, preset, gpu_uuid = std::move(gpu_uuid), fan_curve] {
        app::OptimizeHooks hooks;
        hooks.aborted = [this] { return abort_.load(); };
        hooks.log = [this](const std::string& line) {
            {
                std::lock_guard lock(mu_);
                log_.push_back(line);
            }
            wake_();
        };
        app::OptimizeOutcome outcome;
        try {
            outcome = app::run_optimize(preset, gpu_uuid, hooks, fan_curve);
        } catch (const std::exception& e) {   // a thrown exception would otherwise end the process
            outcome.error = std::string("unexpected error: ") + e.what();
        }
        {
            std::lock_guard lock(mu_);
            outcome_ = std::move(outcome);
        }
        running_ = false;
        wake_();
    });
    return true;
}

OptimizeWorker::Snapshot OptimizeWorker::snapshot() const {
    std::lock_guard lock(mu_);
    return {running_.load(), log_, outcome_};
}

}
