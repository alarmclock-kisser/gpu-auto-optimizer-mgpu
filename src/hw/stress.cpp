#include "hw/stress.hpp"
#include "core/stress_math.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace gao {

namespace {

constexpr UINT kNvidiaVendorId = 0x10DE;
constexpr int kTile = 64;   // output tile per thread group (16x16 threads, 4x4 outputs each)
constexpr double kTargetBatchMs = 250.0;
constexpr int kMaxDispatches = 2048;
constexpr UINT kBwBytes = 256u * 1024 * 1024;   // per buffer; far above the 4070's 36 MB L2
constexpr UINT kBwGroups = 1024;
constexpr int kBwDispatches = 64;
constexpr size_t kBwRuns = 3;          // consecutive runs that must agree
constexpr int kBwMaxRuns = 20;         // ~1.5 s at full speed; more if clocks are low
constexpr double kBwSettle = 0.01;     // agree = within 1 %

// Grid-stride copy of 16-byte elements between two raw buffers.
const char kCopyShader[] = R"(
#define COUNT (256 * 1024 * 1024 / 16)
#define THREADS (1024 * 256)
RWByteAddressBuffer Src : register(u0);
RWByteAddressBuffer Dst : register(u1);
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    for (uint i = id.x; i < COUNT; i += THREADS) Dst.Store4(i * 16, Src.Load4(i * 16));
}
)";
// The GPU error counter is a 32-bit uint summed over every dispatch in a
// batch; a batch where every element is wrong must not wrap it back to 0.
static_assert(static_cast<long long>(kMaxDispatches) * kStressN * kStressN <= 0xFFFFFFFFLL,
              "a fully wrong batch would overflow the error counter");

// Tiled, register-blocked matmul: every thread computes a 4x4 block of
// C = A * B (16 FMAs per 8 shared-memory reads), compares each element with
// the reference, and counts mismatches. The group stages a 64x16 slice of A
// and a 16x64 slice of B per step; each of the 256 threads loads 4 of each.
const char kShader[] = R"(
#define N 1024
#define TM 64
#define TK 16
StructuredBuffer<float> A : register(t0);
StructuredBuffer<float> B : register(t1);
StructuredBuffer<float> Ref : register(t2);
RWStructuredBuffer<uint> Errors : register(u0);
groupshared float As[TM][TK];
groupshared float Bs[TK][TM];
[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint lin = tid.y * 16 + tid.x;
    uint r0 = gid.y * TM + tid.y * 4;
    uint c0 = gid.x * TM + tid.x * 4;
    // Four float4 rows instead of a float[4][4]: FXC takes minutes to
    // compile a fully unrolled local array, and seconds for this.
    float4 acc0 = 0, acc1 = 0, acc2 = 0, acc3 = 0;
    [loop] for (uint k0 = 0; k0 < N; k0 += TK) {
        [unroll] for (uint l = 0; l < 4; ++l) {
            uint e = lin + l * 256;
            uint ar = e / TK, ac = e % TK;
            As[ar][ac] = A[(gid.y * TM + ar) * N + k0 + ac];
            uint br = e / TM, bc = e % TM;
            Bs[br][bc] = B[(k0 + br) * N + gid.x * TM + bc];
        }
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint k = 0; k < TK; ++k) {
            float4 b = float4(Bs[k][tid.x * 4], Bs[k][tid.x * 4 + 1], Bs[k][tid.x * 4 + 2], Bs[k][tid.x * 4 + 3]);
            acc0 = mad(As[tid.y * 4][k], b, acc0);
            acc1 = mad(As[tid.y * 4 + 1][k], b, acc1);
            acc2 = mad(As[tid.y * 4 + 2][k], b, acc2);
            acc3 = mad(As[tid.y * 4 + 3][k], b, acc3);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    uint base = r0 * N + c0;
    float4 ref0 = float4(Ref[base], Ref[base + 1], Ref[base + 2], Ref[base + 3]);
    float4 ref1 = float4(Ref[base + N], Ref[base + N + 1], Ref[base + N + 2], Ref[base + N + 3]);
    float4 ref2 = float4(Ref[base + 2 * N], Ref[base + 2 * N + 1], Ref[base + 2 * N + 2], Ref[base + 2 * N + 3]);
    float4 ref3 = float4(Ref[base + 3 * N], Ref[base + 3 * N + 1], Ref[base + 3 * N + 2], Ref[base + 3 * N + 3]);
    uint4 bad = (acc0 != ref0) + (acc1 != ref1) + (acc2 != ref2) + (acc3 != ref3);
    uint errs = bad.x + bad.y + bad.z + bad.w;
    if (errs) InterlockedAdd(Errors[0], errs);
}
)";

std::string Narrow(const wchar_t* w) {
    char buf[256];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string("?");
}

std::string Hr(const char* what, HRESULT hr) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed (hr=0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

}

// The matching NVIDIA adapter, or null. `name` receives its description.
ComPtr<IDXGIAdapter1> find_nvidia_adapter(const GpuLuid& luid, std::string* name) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == kNvidiaVendorId && std::memcmp(&desc.AdapterLuid, luid.data(), luid.size()) == 0) {
            *name = Narrow(desc.Description);
            return adapter;
        }
    }
    return nullptr;
}

struct Stress::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11ShaderResourceView> a, b, ref;
    ComPtr<ID3D11Buffer> errors, staging;
    ComPtr<ID3D11UnorderedAccessView> errors_uav;
    std::vector<float> ha, hb, href;   // host copies, kept for device recreation
    ComPtr<ID3D11ComputeShader> copy_cs;
    ComPtr<ID3D11Buffer> bw_src, bw_dst;
    ComPtr<ID3D11UnorderedAccessView> bw_src_uav, bw_dst_uav;
    ComPtr<ID3D11Query> q_disjoint, q_begin, q_end;
};

Stress::Stress() : impl_(std::make_unique<Impl>()) {}
Stress::~Stress() = default;

bool Stress::Init(const GpuLuid& adapter_luid, StressSelftest selftest) {
    adapter_luid_ = adapter_luid;
    selftest_ = selftest;
    impl_->ha = make_stress_matrix(1);
    impl_->hb = make_stress_matrix(2);
    impl_->href = reference_matmul(impl_->ha, impl_->hb);
    if (selftest_ == StressSelftest::WrongResult) impl_->href[0] += 1.0f;
    return CreateDevice();
}

bool Stress::CreateDevice() {
    Impl& d = *impl_;
    d.device.Reset(); d.ctx.Reset(); d.cs.Reset();
    d.a.Reset(); d.b.Reset(); d.ref.Reset();
    d.errors.Reset(); d.staging.Reset(); d.errors_uav.Reset();
    d.copy_cs.Reset(); d.bw_src.Reset(); d.bw_dst.Reset(); d.bw_src_uav.Reset(); d.bw_dst_uav.Reset();
    d.q_disjoint.Reset(); d.q_begin.Reset(); d.q_end.Reset();
    // Any failure below leaves no device behind, so the next Batch() retries
    // the rebuild instead of dispatching with half-created resources (likely
    // right after a TDR, when resource creation can still fail).
    struct ResetOnFailure {
        Impl& d;
        bool ok = false;
        ~ResetOnFailure() { if (!ok) d.device.Reset(); }
    } guard{d};

    const ComPtr<IDXGIAdapter1> adapter = find_nvidia_adapter(adapter_luid_, &adapter_name_);
    if (!adapter) { error_ = "no DXGI adapter matches the selected NVIDIA GPU"; return false; }

    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1,
                           D3D11_SDK_VERSION, &d.device, nullptr, &d.ctx);
    if (FAILED(hr)) { error_ = Hr("D3D11CreateDevice", hr); return false; }

    ComPtr<ID3DBlob> code, log;
    hr = D3DCompile(kShader, sizeof(kShader) - 1, "stress.hlsl", nullptr, nullptr, "main", "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &log);
    if (FAILED(hr)) {
        error_ = Hr("D3DCompile", hr);
        if (log) error_ += std::string(": ") + static_cast<const char*>(log->GetBufferPointer());
        return false;
    }
    hr = d.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &d.cs);
    if (FAILED(hr)) { error_ = Hr("CreateComputeShader", hr); return false; }

    auto make_srv = [&](const std::vector<float>& host, ComPtr<ID3D11ShaderResourceView>& out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(host.size() * sizeof(float));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(float);
        D3D11_SUBRESOURCE_DATA init{host.data()};
        ComPtr<ID3D11Buffer> buf;
        HRESULT h = d.device->CreateBuffer(&bd, &init, &buf);
        if (SUCCEEDED(h)) h = d.device->CreateShaderResourceView(buf.Get(), nullptr, &out);
        return h;
    };
    if (FAILED(hr = make_srv(d.ha, d.a)) || FAILED(hr = make_srv(d.hb, d.b)) ||
        FAILED(hr = make_srv(d.href, d.ref))) {
        error_ = Hr("input buffer", hr); return false;
    }

    D3D11_BUFFER_DESC ed{};
    ed.ByteWidth = sizeof(UINT);
    ed.Usage = D3D11_USAGE_DEFAULT;
    ed.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ed.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ed.StructureByteStride = sizeof(UINT);
    if (FAILED(hr = d.device->CreateBuffer(&ed, nullptr, &d.errors)) ||
        FAILED(hr = d.device->CreateUnorderedAccessView(d.errors.Get(), nullptr, &d.errors_uav))) {
        error_ = Hr("error counter", hr); return false;
    }
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = sizeof(UINT);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(hr = d.device->CreateBuffer(&sd, nullptr, &d.staging))) {
        error_ = Hr("staging buffer", hr); return false;
    }
    guard.ok = true;
    return true;
}

StressBatch Stress::Batch() {
    StressBatch out;
    const auto start = std::chrono::steady_clock::now();
    auto finish = [&] {
        out.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        return out;
    };
    Impl& d = *impl_;
    if (!d.device && !CreateDevice()) { out.device_lost = true; return finish(); }

    const UINT zero[4] = {0, 0, 0, 0};
    d.ctx->ClearUnorderedAccessViewUint(d.errors_uav.Get(), zero);
    d.ctx->CSSetShader(d.cs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {d.a.Get(), d.b.Get(), d.ref.Get()};
    d.ctx->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uavs[] = {d.errors_uav.Get()};
    d.ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    const int dispatches = dispatches_;
    constexpr UINT groups = kStressN / kTile;
    for (int i = 0; i < dispatches; ++i) d.ctx->Dispatch(groups, groups, 1);
    d.ctx->CopyResource(d.staging.Get(), d.errors.Get());

    // Map blocks until the GPU has finished, so elapsed time is GPU time.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = d.ctx->Map(d.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || d.device->GetDeviceRemovedReason() != S_OK) {
        out.device_lost = true;
        d.device.Reset();   // next Batch() recreates everything
        return finish();
    }
    UINT count = 0;
    std::memcpy(&count, mapped.pData, sizeof(count));
    // The GPU counter is unsigned and spans every dispatch in the batch; clamp
    // so a huge count can never wrap to a negative "no errors".
    out.wrong_values = count > INT_MAX ? INT_MAX : static_cast<int>(count);
    d.ctx->Unmap(d.staging.Get(), 0);
    out.iterations = dispatches;
    finish();
    if (out.elapsed_ms < kTargetBatchMs * 0.6 && dispatches_ < kMaxDispatches) dispatches_ *= 2;
    return out;
}

std::optional<double> Stress::MeasureBandwidth() {
    Impl& d = *impl_;
    if (!d.device && !CreateDevice()) return std::nullopt;
    // Created lazily (the stress path never needs 512 MB of buffers) and all
    // or nothing: q_end is created last, and any failure drops what was made,
    // so a later call can never time dispatches against half-built resources.
    if (!d.q_end) {
        auto drop = [&](const std::string& why) {
            d.copy_cs.Reset(); d.bw_src.Reset(); d.bw_dst.Reset(); d.bw_src_uav.Reset(); d.bw_dst_uav.Reset();
            d.q_disjoint.Reset(); d.q_begin.Reset(); d.q_end.Reset();
            error_ = why;
            return std::nullopt;
        };
        ComPtr<ID3DBlob> code, log;
        HRESULT hr = D3DCompile(kCopyShader, sizeof(kCopyShader) - 1, "copy.hlsl", nullptr, nullptr, "main",
                                "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &log);
        if (FAILED(hr)) return drop(Hr("D3DCompile (copy)", hr));
        if (FAILED(hr = d.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &d.copy_cs)))
            return drop(Hr("CreateComputeShader (copy)", hr));
        auto make = [&](ComPtr<ID3D11Buffer>& buf, ComPtr<ID3D11UnorderedAccessView>& uav, const void* init) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kBwBytes;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            D3D11_SUBRESOURCE_DATA data{init, 0, 0};
            HRESULT h = d.device->CreateBuffer(&bd, init ? &data : nullptr, &buf);
            if (FAILED(h)) return h;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = kBwBytes / 4;
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            return d.device->CreateUnorderedAccessView(buf.Get(), &ud, &uav);
        };
        // Pseudo-random source data: an all-zero buffer barely toggles the
        // memory bus, provoking fewer of the retried transfers this
        // measurement exists to detect.
        std::vector<std::uint32_t> noise(kBwBytes / 4);
        std::uint32_t x = 0x9E3779B9u;
        for (auto& w : noise) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; w = x; }
        if (FAILED(hr = make(d.bw_src, d.bw_src_uav, noise.data())) || FAILED(hr = make(d.bw_dst, d.bw_dst_uav, nullptr)))
            return drop(Hr("bandwidth buffers", hr));
        D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC qt{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(hr = d.device->CreateQuery(&qd, &d.q_disjoint)) || FAILED(hr = d.device->CreateQuery(&qt, &d.q_begin)) ||
            FAILED(hr = d.device->CreateQuery(&qt, &d.q_end)))
            return drop(Hr("timestamp queries", hr));
    }
    auto wait = [&](ID3D11Asynchronous* q, void* out, UINT size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const HRESULT hr = d.ctx->GetData(q, out, size, 0);
            if (hr == S_OK) return true;
            if (FAILED(hr) || d.device->GetDeviceRemovedReason() != S_OK ||
                std::chrono::steady_clock::now() > deadline) return false;
            Sleep(1);
        }
    };
    ID3D11UnorderedAccessView* uavs[] = {d.bw_src_uav.Get(), d.bw_dst_uav.Get()};
    ID3D11UnorderedAccessView* none[] = {nullptr, nullptr};
    // An idle card starts in its lowest memory P-state (405 MHz on the 4070)
    // and needs up to a second of load to clock up; single readings came out
    // at half speed in testing. So: keep measuring until kBwRuns consecutive
    // runs agree within kBwSettle, and report their median.
    std::vector<double> runs;
    for (int run = 0; run < kBwMaxRuns; ++run) {
        d.ctx->CSSetShader(d.copy_cs.Get(), nullptr, 0);
        d.ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        d.ctx->Begin(d.q_disjoint.Get());
        d.ctx->End(d.q_begin.Get());
        for (int i = 0; i < kBwDispatches; ++i) d.ctx->Dispatch(kBwGroups, 1, 1);
        d.ctx->End(d.q_end.Get());
        d.ctx->End(d.q_disjoint.Get());
        d.ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        if (!wait(d.q_disjoint.Get(), &dj, sizeof(dj)) || !wait(d.q_begin.Get(), &t0, sizeof(t0)) ||
            !wait(d.q_end.Get(), &t1, sizeof(t1))) {
            if (d.device->GetDeviceRemovedReason() != S_OK) d.device.Reset();
            return std::nullopt;
        }
        if (dj.Disjoint || dj.Frequency == 0 || t1 <= t0) continue;   // clock changed mid-run: discard
        const double seconds = double(t1 - t0) / double(dj.Frequency);
        runs.push_back(2.0 * kBwBytes * kBwDispatches / seconds / 1e9);
        if (runs.size() < kBwRuns) continue;
        std::vector<double> last(runs.end() - kBwRuns, runs.end());
        std::sort(last.begin(), last.end());
        if (last.back() <= last.front() * (1 + kBwSettle)) return last[kBwRuns / 2];
    }
    error_ = "bandwidth did not settle within " + std::to_string(kBwMaxRuns) + " runs";
    return std::nullopt;
}

}
