#include "hw/nvapi.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

namespace gao {

namespace {

// QueryInterface ids. Proven against this hardware by the project's Python
// implementation (src/backends/nvapi.py, tag v0.9-python) -- see task-5-brief.md.
constexpr unsigned kInitializeId       = 0x0150E828;
// Was 0x0D22BDD7 (a digit-shifted typo the prior Python implementation
// carried for months without noticing, because a failed NvAPI_Unload is
// silent -- nothing reads its return value). The task brief that flagged
// this bug named 0xD22BDD7F as the fix; a standalone read-only probe
// (nvapi_QueryInterface with no other calls, then a harmless
// Initialize->Unload cycle) showed that value ALSO resolves to null, while
// 0xD22BDD7E resolves to a real function pointer that returns NVAPI_OK when
// called right after NvAPI_Initialize(). 0xD22BDD7E also matches
// arcnmx/nvapi-rs's id table (sys/src/nvid.rs) independently of the probe,
// so it is used here instead of the brief's stated value.
constexpr unsigned kUnloadId           = 0xD22BDD7E;
constexpr unsigned kEnumPhysicalGpusId = 0xE5AC921F;
constexpr unsigned kGetBusIdId         = 0x1BE0B8E5;
constexpr unsigned kGetPstates20Id     = 0x6FF81213;
constexpr unsigned kSetPstates20Id     = 0x0F4DAE6B;

// NV_GPU_PERF_PSTATES20_INFO, V2. Whole-buffer size and the version tag that
// must be written into the buffer before every GET and SET call.
constexpr int kBufferSize = 7416;
constexpr unsigned kVersionV2 = kBufferSize | (2u << 16);  // 0x00021CF8

// Byte offsets into the buffer. version/editable/numPstates are the header;
// the two deltas sit inside the P0 entry. All of this is undocumented by
// NVIDIA and comes from the same proven Python source as the ids above.
constexpr int kOffVersion = 0;      // uint32
constexpr int kOffEditable = 4;     // uint32 -- must be 1 or SET is a no-op
constexpr int kOffNumPstates = 8;   // uint32 -- 1 restricts the SET to P0
constexpr int kOffCoreDelta = 40;   // int32, kHz
constexpr int kOffMemDelta = 84;    // int32, kHz

constexpr int kToleranceKhz = 1000;
constexpr unsigned kMaxPhysicalGpus = 64;  // NVAPI_MAX_PHYSICAL_GPUS

using nvapi_status_t = int;
constexpr nvapi_status_t kNvapiOk = 0;

typedef void* (*fn_query)(unsigned);
typedef nvapi_status_t (*fn_initialize)();
typedef nvapi_status_t (*fn_unload)();
typedef nvapi_status_t (*fn_enum)(void**, unsigned*);
typedef nvapi_status_t (*fn_bus_id)(void*, unsigned*);
typedef nvapi_status_t (*fn_get_pstates20)(void*, void*);
typedef nvapi_status_t (*fn_set_pstates20)(void*, void*);

void PutU32(unsigned char* buf, int offset, unsigned value) {
    std::memcpy(buf + offset, &value, sizeof(value));
}

int GetI32(const unsigned char* buf, int offset) {
    int value = 0;
    std::memcpy(&value, buf + offset, sizeof(value));
    return value;
}

}  // namespace

void* Nvapi::QueryFn(unsigned id) {
    if (!query_) return nullptr;
    return ((fn_query)query_)(id);
}

bool Nvapi::Init() {
    // System32 only, as for nvml.dll: never from the exe's own folder.
    HMODULE h = LoadLibraryExA("nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) { error_ = "could not load nvapi64.dll"; return false; }
    lib_ = h;

    query_ = (void*)GetProcAddress(h, "nvapi_QueryInterface");
    if (!query_) { error_ = "nvapi_QueryInterface not found in nvapi64.dll"; return false; }

    auto initialize = (fn_initialize)QueryFn(kInitializeId);
    if (!initialize) { error_ = "NvAPI_Initialize: nvapi_QueryInterface returned null"; return false; }
    if (initialize() != kNvapiOk) { error_ = "NvAPI_Initialize failed"; return false; }
    inited_ = true;  // NvAPI_Unload is now required from the destructor, even if the rest of Init() fails below.

    auto enum_gpus = (fn_enum)QueryFn(kEnumPhysicalGpusId);
    if (!enum_gpus) { error_ = "NvAPI_EnumPhysicalGPUs: nvapi_QueryInterface returned null"; return false; }

    void* handles[kMaxPhysicalGpus] = {};
    unsigned count = 0;
    if (enum_gpus(handles, &count) != kNvapiOk) { error_ = "NvAPI_EnumPhysicalGPUs failed"; return false; }
    if (count > kMaxPhysicalGpus) { error_ = "NvAPI_EnumPhysicalGPUs returned too many GPUs"; return false; }

    gpus_.assign(handles, handles + count);
    if (gpus_.empty()) { error_ = "NvAPI_EnumPhysicalGPUs returned no GPUs"; return false; }

    return true;
}

std::optional<unsigned> Nvapi::GpuIndexForBusId(unsigned bus_id) {
    if (!inited_) { error_ = "NVAPI not initialized"; return std::nullopt; }
    auto get_bus_id = (fn_bus_id)QueryFn(kGetBusIdId);
    if (!get_bus_id) { error_ = "NvAPI_GPU_GetBusId: nvapi_QueryInterface returned null"; return std::nullopt; }
    std::optional<unsigned> match;
    for (size_t i = 0; i < gpus_.size(); ++i) {
        unsigned current_bus_id = 0;
        if (get_bus_id(gpus_[i], &current_bus_id) != kNvapiOk) {
            error_ = "NvAPI_GPU_GetBusId failed";
            return std::nullopt;
        }
        if (current_bus_id == bus_id) {
            if (match) {
                error_ = "multiple NVAPI GPUs share the selected PCI bus";
                return std::nullopt;
            }
            match = static_cast<unsigned>(i);
        }
    }
    if (!match) error_ = "NVAPI has no GPU on the selected PCI bus";
    return match;
}

void* Nvapi::GpuHandle(unsigned gpu) {
    if (!inited_) { error_ = "NVAPI not initialized"; return nullptr; }
    if (gpu >= gpus_.size()) { error_ = "GPU index out of range"; return nullptr; }
    return gpus_[gpu];
}

bool Nvapi::GetPstates20(unsigned gpu, unsigned char* buf) {
    void* handle = GpuHandle(gpu);
    if (!handle) return false;  // error_ set by GpuHandle

    auto get_pstates20 = (fn_get_pstates20)QueryFn(kGetPstates20Id);
    if (!get_pstates20) { error_ = "NvAPI_GPU_GetPstates20: nvapi_QueryInterface returned null"; return false; }

    std::memset(buf, 0, kBufferSize);
    PutU32(buf, kOffVersion, kVersionV2);  // the driver requires the version tag pre-set on entry
    if (get_pstates20(handle, buf) != kNvapiOk) { error_ = "NvAPI_GPU_GetPstates20 failed"; return false; }
    return true;
}

std::optional<std::pair<int, int>> Nvapi::ReadOffsetsMhz(unsigned gpu) {
    unsigned char buf[kBufferSize];
    if (!GetPstates20(gpu, buf)) return std::nullopt;  // error_ set by GetPstates20
    return std::make_pair(GetI32(buf, kOffCoreDelta) / 1000, GetI32(buf, kOffMemDelta) / 1000);
}

bool Nvapi::SetDeltaKhz(unsigned gpu, int offset_bytes, int khz) {
    void* handle = GpuHandle(gpu);
    if (!handle) return false;  // error_ set by GpuHandle

    unsigned char buf[kBufferSize];
    if (!GetPstates20(gpu, buf)) return false;  // error_ set by GetPstates20; buf holds the current state

    auto set_pstates20 = (fn_set_pstates20)QueryFn(kSetPstates20Id);
    if (!set_pstates20) { error_ = "NvAPI_GPU_SetPstates20: nvapi_QueryInterface returned null"; return false; }

    // Minimal SET: restrict the edit to P0 and mark it editable. This is the
    // technique already proven on this hardware; without it the driver can
    // accept the call and report success while leaving the delta untouched.
    PutU32(buf, kOffVersion, kVersionV2);
    PutU32(buf, kOffEditable, 1);
    PutU32(buf, kOffNumPstates, 1);
    PutU32(buf, offset_bytes, static_cast<unsigned>(khz));  // int32 stored bit-for-bit

    if (set_pstates20(handle, buf) != kNvapiOk) { error_ = "NvAPI_GPU_SetPstates20 failed"; return false; }

    // NvAPI_GPU_SetPstates20 can return NVAPI_OK for a delta it did not
    // actually apply. This read-back is the only way to know whether the
    // write landed, and it is the entire reason this function exists.
    unsigned char verify_buf[kBufferSize];
    if (!GetPstates20(gpu, verify_buf)) {
        error_ = "NvAPI_GPU_SetPstates20 read-back failed: " + error_;
        return false;
    }
    const int actual_khz = GetI32(verify_buf, offset_bytes);
    const int diff = actual_khz > khz ? actual_khz - khz : khz - actual_khz;
    if (diff > kToleranceKhz) {
        error_ = "NvAPI_GPU_SetPstates20 read-back mismatch: requested " + std::to_string(khz) +
                 " kHz, driver reports " + std::to_string(actual_khz) + " kHz";
        return false;
    }
    return true;
}

bool Nvapi::SetCoreOffsetMhz(unsigned gpu, int mhz) { return SetDeltaKhz(gpu, kOffCoreDelta, mhz * 1000); }
bool Nvapi::SetMemOffsetMhz(unsigned gpu, int mhz) { return SetDeltaKhz(gpu, kOffMemDelta, mhz * 1000); }

bool Nvapi::ResetOffsets(unsigned gpu) {
    // Two independent verified writes rather than one combined buffer edit:
    // each still goes through SetDeltaKhz, so a driver that silently ignores
    // either half is caught exactly the same way a single-offset set is.
    //
    // Best-effort across both, deliberately: reset is the escape hatch a
    // user reaches for after something went wrong, so a failed core reset
    // must not skip the memory reset. Report failure if either failed, but
    // always attempt both and keep whichever error is more specific (naming
    // which one failed) if both do.
    const bool core_ok = SetDeltaKhz(gpu, kOffCoreDelta, 0);
    const std::string core_error = error_;
    const bool mem_ok = SetDeltaKhz(gpu, kOffMemDelta, 0);
    if (core_ok && mem_ok) return true;
    error_ = "core reset " + (core_ok ? std::string("OK") : "failed (" + core_error + ")") +
             "; mem reset " + (mem_ok ? std::string("OK") : "failed (" + error_ + ")");
    return false;
}

Nvapi::~Nvapi() {
    if (inited_ && query_) {
        auto unload = (fn_unload)QueryFn(kUnloadId);
        if (unload) unload();
    }
    if (lib_) FreeLibrary((HMODULE)lib_);
}

}
