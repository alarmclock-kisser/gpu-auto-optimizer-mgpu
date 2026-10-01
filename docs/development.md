# Development

How the code is organised, how to build and test it, and the rules the design depends on. For using the app, see the [README](../README.md).

## Build and test

Visual Studio 2026 with the C++ and CMake components (CMake 4.2 or newer, which knows the VS 2026 generator). From a Developer PowerShell in the repository root:

```powershell
cmake --workflow --preset ci      # configure, build Release, run the tests

cmake --preset default
cmake --build --preset debug
ctest --preset debug

cmake --preset asan               # AddressSanitizer build in build-asan/
cmake --build --preset asan
ctest --preset asan               # needs the MSVC bin\Hostx64\x64 folder on PATH
```

### Build the Windows x64 executables in Visual Studio

These are native C++ executables, not a .NET assembly. The `default` CMake preset selects the Visual Studio 2026 generator and the x64 architecture. Install Visual Studio 2026 with **Desktop development with C++**, the MSVC x64 toolset, and the CMake tools for Windows.

In Visual Studio, use **File > Open > Folder...** and select the repository root. Let CMake configure the `default` preset, select the `release` build configuration/preset, then build the `gao` and `GpuAutoOptimizer` targets (or build the `ALL_BUILD` target to include tests too). The executables are written to:

```text
build\Release\gao.exe
build\Release\GpuAutoOptimizer.exe
```

The same Release build from a Developer PowerShell at the repository root is:

```powershell
cmake --preset default
cmake --build --preset release
ctest --preset release
```

If `cmake` is not on `PATH`, use the CMake bundled with Visual Studio 2026:

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
& "$cmake\cmake.exe" --preset default
& "$cmake\cmake.exe" --build --preset release
& "$cmake\ctest.exe" --preset release
```

To make a release-style zip locally, put `build\Release\gao.exe`, `build\Release\GpuAutoOptimizer.exe`, `README.md` and `LICENSE` in one folder and zip its contents. The CI release workflow stages these same four files and names the archive `GpuAutoOptimizer-<version>-win-x64.zip`; its tagged release also includes a SHA-256 file and provenance attestation. A local build does not automatically create the packaged zip.

Our code builds at `/W4 /permissive-` with warnings as errors; `third_party/` is a system include. Everything is compiled with Control Flow Guard and SDL checks, and both executables are linked CET-compatible and carry an application manifest.

## Layout

```
src/core/     pure logic: no windows.h, no driver calls, no D3D. Unit-tested in CI.
  types.hpp       Telemetry, GpuControl (the hardware seam), AppliedState
  objectives.*    the four profiles as data
  config.*        per-GPU settings and gao.json round-trip
  stress_math.*   exact-float stress inputs and the CPU reference result
  stability.*     the stress run loop and its verdict
  journal.*       write-ahead log of clock candidates; a freeze becomes a ceiling
  manual.*        hand-applied power/core/memory: same bounds, extreme zones, journaled apply
  search.*        the tuner: baseline, power, core, memory, confirmation, soak
  boot.*          when a logon apply may run (strikes, driver, card) and the verified apply
  watchdog.*      the tray app's decisions: re-apply, give up, back off, driver changed
  task_xml.*      the logon task definition
  fan_curve.*     fan curves, the per-second controller and the FanDriver
src/hw/       the only code that touches hardware or the OS state folders.
  nvml.*          telemetry, UUID/name, PCI bus and (when supported) DXGI LUID via NVML
  nvapi.*         clock offsets via NVAPI, verified by read-back
  gpu_control.*   maps NVML devices to NVAPI handles by PCI bus and wires GpuControl
  stress.*        the DX11 compute load, matched by LUID or unique exact adapter name
  app_files.*     gao.json (atomic writes), the crash journal and boot.log, flushed to disk
  boot_task.*     the logon task and the Program Files copy
src/app/
  common.*        logic both programs share: optimize, logon apply, apply-at-logon on/off
  main.cpp        gao.exe, the command line
  gui/            GpuAutoOptimizer.exe: window, tray, optimize worker thread, watchdog loop
tests/        doctest, core only
third_party/  doctest, nlohmann/json, Dear ImGui: vendored as source, no package manager
```

## Profiles as data

`src/core/objectives.cpp` defines each profile as a thermal ceiling, a perf push (the fraction of the highest stable offset that is applied) and which of core, memory and power tuning it enables:

| Profile | `--optimize` | Max temp | Perf push | Core | Memory | Power |
|---|---|---|---|---|---|---|
| BestOfMyGpu (default) | `best` | 75 °C | 0.7 | on | on | on |
| Quiet | `quiet` | 80 °C | 0.4 | on | on | on |
| CoolAndEfficient | `cool` | 65 °C | 0.3 | off | off | on |
| MaxPerformance | `max` | 83 °C | 1.0 | on | on | on |

The applied offset is always at least one step below the confirmed edge. Profiles with a perf push below 0.5 also look for the lowest power limit that costs less than 2 % score.

## Design decisions

| Decision | Why |
|---|---|
| NVAPI and NVML directly, not MSI Afterburner | An earlier version drove Afterburner by editing its profile files; the edits never reached the hardware, and Afterburner offers no per-step read-back, which the whole search depends on. |
| Instability = a wrong result or a lost device | A GPU computes wrong before it crashes, so a self-checking compute load catches instability earliest and cheapest. A TDR (`DXGI_ERROR_DEVICE_REMOVED`) is recoverable, unlike a freeze. The load's iterations per second double as the score. |
| Memory stops at the bandwidth peak | GDDR6/GDDR6X retry failed transfers, so memory overclocks lose bandwidth long before they return wrong results. |
| Edges are confirmed, then backed off | A 3 s probe can pass by luck; the edges get 30 s probes before the safety margin is applied, and the result must pass a 60 s soak. |
| No undervolting | Locking a voltage point hard-froze the reference RTX 4070, and reshaping the curve gave no measurable gain on a power-limited card. |
| Fans through NVML, stop zone via the driver | NVIDIA's legacy NVAPI fan API is gone on RTX 20-series and newer; NVML's `nvmlDeviceSetFanSpeed_v2` is public and verified by reading the target back. Below the stop threshold the driver owns the fans, so no failure of this app can leave them stopped. |
| Tray app plus logon task | Driver settings are volatile: a reboot or driver reset clears them. The logon task starts the tray app, whose watchdog keeps the tune applied; three crashing logons in a row switch it off. |
| One profile per GPU | Optimizing a card updates only that card's profile. Logon apply and the watchdog operate on every saved profile independently; manual Apply/Revert and the live dashboard target the selected GPU. |
| Stable GPU identity | NVML UUID is the persisted profile key, PCI bus ID maps to NVAPI, and NVML's LUID maps to the DXGI adapter when available. If NVML cannot provide the LUID, the stress test accepts only a unique exact NVIDIA adapter-name match. Device indices can vary between APIs or boots, so they are resolved at runtime rather than saved as identity. |
| Per-GPU configuration migration | `gao.json` stores device records keyed by UUID, including each card's profile, fan settings and boot crash strikes, plus the selected UUID. The loader migrates the previous single-profile shape into a device record, preserving existing settings. |
| Fail closed on ambiguous mapping | If a selected GPU cannot be uniquely mapped between NVML and NVAPI, control reports an error instead of guessing a handle. DXGI matching prefers LUID; if that is unavailable, duplicate-name matches fail instead of running the stress test on a guessed GPU. |
| Dear ImGui on DX11 | One small binary with no runtime, and the D3D11 device is in the process anyway for the stress load. |

## Rules the design depends on

- `src/core/` is pure logic. It reaches hardware only through `GpuControl` and callbacks, so CI tests it without a GPU.
- Every hardware write is verified by reading it back; a mismatch is a failure. Any failed apply ends at stock.
- The crash journal's `begin` line is flushed to disk before a clock candidate touches the hardware.
- Everything the elevated logon task touches is admin-only: the copy in `%ProgramFiles%` and the state folder in `%ProgramData%`. A state folder someone else created first, or a link in its place, is refused. Both locations come from the registry, not from environment variables.
- The CRT is linked statically and every non-system DLL (`d3d11`, `dxgi`, `d3dcompiler_47`, `dwmapi`) is delay-loaded from System32, so nothing placed next to the executable is loaded.

## Testing

`core_tests` covers `src/core/` and runs in CI on a GitHub-hosted `windows-2025` runner, once normally and once under AddressSanitizer. CI also builds both executables, so a link error in the hardware layer is caught there.

The hardware layer cannot run in CI (the runner has no GPU). It is checked by hand on the reference RTX 4070 through [hardware-checks.md](hardware-checks.md); record the results there. The multi-GPU mapping check requires a system with at least two NVIDIA GPUs and remains a separate manual check.

## Releases

The version lives in `src/core/version.hpp`. Pushing a `v*` tag that matches it builds, tests and packages both executables into a **draft** GitHub release with a SHA-256 file and a build provenance attestation; publishing the draft is a manual step. Running the Release workflow by hand is a dry run that only uploads the zip as an artifact.
