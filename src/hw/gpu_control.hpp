#pragma once
#include "core/types.hpp"
#include "hw/nvapi.hpp"
#include "hw/nvml.hpp"
#include <optional>
#include <string>

namespace gao {

// Wires the two drivers into the single struct core code sees. nvml and
// nvapi must outlive the returned GpuControl (its callbacks capture them by
// reference); both are expected to have had Init() called already.
std::optional<GpuControl> make_gpu_control(Nvml& nvml, Nvapi& nvapi, unsigned gpu, std::string* why = nullptr);

}
