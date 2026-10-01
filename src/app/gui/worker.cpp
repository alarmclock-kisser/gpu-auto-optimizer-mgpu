#include "app/gui/worker.hpp"
#include "hw/gpu_control.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include "hw/stress.hpp"
#include <algorithm>
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

ManualWorker::~ManualWorker() = default;

bool ManualWorker::start(std::string gpu_uuid, int max_temp_c) {
    if (running_.exchange(true)) return false;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard lock(mu_);
        result_.reset();
        error_.clear();
    }
    thread_ = std::jthread([this, gpu_uuid = std::move(gpu_uuid), max_temp_c] {
        try {
            Nvml nvml;
            if (!nvml.Init()) {
                std::lock_guard lock(mu_);
                error_ = "NVML init failed: " + nvml.Error();
                running_ = false;
                wake_();
                return;
            }
            std::string why;
            const auto gpus = app::enumerate_gpus(nvml, &why);
            if (!why.empty()) {
                std::lock_guard lock(mu_);
                error_ = why;
                running_ = false;
                wake_();
                return;
            }
            const auto it = std::find_if(gpus.begin(), gpus.end(),
                                         [&](const app::GpuInfo& gpu) { return gpu.uuid == gpu_uuid; });
            if (it == gpus.end()) {
                std::lock_guard lock(mu_);
                error_ = "the selected GPU is unavailable";
                running_ = false;
                wake_();
                return;
            }
            const auto luid = nvml.DeviceLuid(it->index);
            Stress load;
            if (!load.Init(luid, it->name)) {
                std::lock_guard lock(mu_);
                error_ = "stress init failed: " + load.Error();
                running_ = false;
                wake_();
                return;
            }
            const unsigned index = it->index;
            const StabilityResult r = run_stability([&] { return load.Batch(); },
                                                  [&] { return nvml.Read(index); }, 60.0, max_temp_c);
            std::lock_guard lock(mu_);
            result_ = r;
        } catch (const std::exception& e) {
            std::lock_guard lock(mu_);
            error_ = std::string("unexpected error: ") + e.what();
        }
        running_ = false;
        wake_();
    });
    return true;
}

ManualWorker::Snapshot ManualWorker::snapshot() const {
    std::lock_guard lock(mu_);
    return {running_.load(), result_, error_};
}

}
