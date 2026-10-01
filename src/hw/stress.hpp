#pragma once
#include "core/stability.hpp"
#include "hw/nvml.hpp"
#include <memory>
#include <optional>
#include <string>

namespace gao {

// Hidden test mode for hardware check 11: stock hardware never produces a
// wrong value on its own. (Check 12 forces a TDR with `dxcap -forcetdr`
// instead: a long dispatch does not trigger one, because the GPU preempts
// compute work rather than letting Windows reset the driver.)
enum class StressSelftest {
    None,
    WrongResult,   // one reference value is corrupted before upload
};

// The DX11 compute stress load. Multiplies two exact-float matrices (see
// core/stress_math.hpp) and has the GPU count every output element that
// differs from the uploaded CPU reference. Changes no GPU settings.
class Stress {
public:
    Stress();
    ~Stress();
    bool Init(const GpuLuid& adapter_luid, StressSelftest selftest = StressSelftest::None);
    // One batch of dispatches. Batches start at one dispatch and double until
    // they take ~250 ms. After a lost device, the next call recreates it.
    StressBatch Batch();
    // Device-memory bandwidth in GB/s (read + write) from a 256 MB buffer copy,
    // timed with GPU timestamps; median of 3 runs. nullopt when the device was
    // lost or the timing was unusable.
    std::optional<double> MeasureBandwidth();
    const std::string& Error() const { return error_; }
    const std::string& AdapterName() const { return adapter_name_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;   // keeps D3D headers out of every includer
    StressSelftest selftest_ = StressSelftest::None;
    std::string error_;
    std::string adapter_name_;
    GpuLuid adapter_luid_{};
    int dispatches_ = 1;
    bool CreateDevice();
};

}
