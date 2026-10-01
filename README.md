<div align="center">

<img src="docs/images/logo.png" width="200" alt="GPU Auto Optimizer logo: a graphics card with a speedometer">

# GPU Auto Optimizer

**Finds the fastest settings your NVIDIA graphics card is stable at, and keeps them applied.**

[![Release](https://img.shields.io/github/v/release/Rovey/gpu-auto-optimizer?color=2f6fd6)](https://github.com/Rovey/gpu-auto-optimizer/releases/latest)
[![CI](https://github.com/Rovey/gpu-auto-optimizer/actions/workflows/ci.yml/badge.svg)](https://github.com/Rovey/gpu-auto-optimizer/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/github/license/Rovey/gpu-auto-optimizer?color=blue)](LICENSE)
![Windows 10 | 11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4)
![NVIDIA](https://img.shields.io/badge/NVIDIA-Pascal%2B-76B900?logo=nvidia&logoColor=white)

[**Download**](https://github.com/Rovey/gpu-auto-optimizer/releases/latest) · [How it works](#how-it-works) · [Command line](#command-line) · [FAQ](#faq)

<img src="docs/images/dashboard.png" width="860" alt="The GPU Auto Optimizer dashboard: live telemetry, the current tuning, the four profiles and the log">

</div>

## What it does

GPU Auto Optimizer tunes the **power limit** and the **core and memory clock offsets** of an NVIDIA card. It stress-tests every candidate setting and keeps the highest one that computes correct results, then backs off by a safety margin. You pick a goal and press one button; a run takes about ten minutes.

- **Four profiles:** Best of my GPU, Cool & efficient, Quiet and Max performance.
- **Multiple NVIDIA GPUs:** choose a card from the dashboard. Each card keeps its own tune, fan settings and crash history; saved tunes are applied to their matching cards at logon.
- **Verified, not trusted:** every value written to the driver is read back and checked, and a failed apply ends at stock.
- **Crash-proof search:** a journal on disk records each candidate before it is tried, so a setting that froze the machine is never tried again.
- **Stays applied:** the tray app re-applies the tune at every logon, and within about half a minute after a driver reset (TDR).
- **Fan curves:** Silent, Normal, Cool or Aggressive with any profile, or your own; fans stop at idle without cycling on and off, the app learns the lowest speed your fans can actually hold, and the fans go back to the driver on exit, sleep, logoff and crashes.
- **Plays fair:** if MSI Afterburner or another tool changes the settings, it steps aside instead of fighting over them.
- **Native and small:** two C++ programs, under 2 MB together, with no installer, no runtime and no driver or service.

## Quick start

1. Download `GpuAutoOptimizer-<version>-win-x64.zip` from the [latest release](https://github.com/Rovey/gpu-auto-optimizer/releases/latest) and unzip it anywhere.
2. Start **`GpuAutoOptimizer.exe`**.
3. Select a GPU in the dashboard, open **Optimize**, pick a profile and press **Optimize GPU**. The app asks for administrator rights, because it changes clocks and power limits.
4. When the run is done, turn on **Apply at logon** to keep the result after a restart.

> [!NOTE]
> The executables are not code-signed yet, so Windows SmartScreen may warn on the first start. Choose **More info**, then **Run anyway**. To check the download first:
> ```powershell
> (Get-FileHash .\GpuAutoOptimizer-<version>-win-x64.zip -Algorithm SHA256).Hash   # compare with the .sha256 file
> gh attestation verify .\GpuAutoOptimizer-<version>-win-x64.zip --repo Rovey/gpu-auto-optimizer
> ```

Turning on **Apply at logon** copies the app to `%ProgramFiles%\GpuAutoOptimizer\`, so the unzipped folder can be deleted afterwards.

## Requirements

| | |
|---|---|
| OS | Windows 10 or 11, 64-bit |
| GPU | NVIDIA, Pascal (GTX 10-series) or newer, with a current driver |
| Rights | Administrator to optimize or apply; reading telemetry does not need them |

## Profiles

| Profile | Goal | Temperature limit | Tunes |
|---|---|---|---|
| **Best of my GPU** (default) | Highest confirmed clocks with a safety margin, most power the card allows | 75 °C | power, core, memory |
| **Cool & efficient** | Lowest power limit that costs under 2 % speed; clocks stay stock | 65 °C | power |
| **Quiet** | Milder clocks and the lowest power limit that costs under 2 % speed | 80 °C | power, core, memory |
| **Max performance** | Highest confirmed clocks minus one step, highest power limit | 83 °C | power, core, memory |

The safety margin: the search finds the highest stable offset and applies a fraction of it (70 % for Best of my GPU), always at least one step below the edge.

## How it works

```mermaid
flowchart LR
    A[Baseline<br/>30 s at stock] --> B[Power limit]
    B --> C[Core offset<br/>binary search]
    C --> D[Memory offset<br/>bandwidth peak]
    D --> E[Confirm edges<br/>30 s probes]
    E --> F[Safety margin]
    F --> G[Soak<br/>60 s]
    G --> H[Save profile]
```

- **Stress test.** A DirectX 11 compute load in which the GPU checks every value it computes against a known answer. Each probe ends in a verdict: `STABLE`, `WRONG RESULT`, `DEVICE LOST`, `TOO HOT` or `NO TELEMETRY`.
- **Memory stops at the bandwidth peak,** not at the first error. GDDR6 and GDDR6X retry failed transfers, so an overclocked memory bus loses speed long before it returns a wrong result. The search measures bandwidth at each step and keeps the lowest offset within 1 % of the best.
- **Crash journal.** Before a candidate touches the hardware, a `begin` line is flushed to disk. If the machine freezes, the unmatched `begin` becomes a ceiling the next run stays below.
- **Apply at logon.** A scheduled task starts the app in the tray at logon, which applies every saved profile to its UUID-matched GPU. It refuses when the driver version or card changed since tuning; crash strikes are tracked independently per GPU.
- **Multiple GPUs.** The dashboard identifies each NVIDIA GPU by NVML index and PCI bus address, and loads that card's profile by UUID. An unelevated session can switch the dashboard temporarily; restarting as administrator carries the selected card forward and saves the selection. The displayed driver version is system-wide.
- **Tune watchdog.** Every 30 seconds the tray app checks each GPU whose tune it applied. It re-applies after a reset, gives up on that card if its tune is reset four times in an hour, and backs off when another program changed its settings.

## Command line

`gao.exe` ships next to the app and drives the same engine. Commands that target one GPU use the UUID selected in the dashboard (or the first NVIDIA GPU if no selection has been saved); `--probe` lists telemetry for every NVIDIA GPU. Commands that write to the GPU need an elevated shell.

| Command | What it does |
|---|---|
| `gao --optimize best\|quiet\|cool\|max [--fan-curve silent\|normal\|cool\|aggressive]` | Runs the search; exit code 0 when saved, 2 when applied but not saved |
| `gao --apply` | Re-applies the selected GPU's saved profile |
| `gao --reset` | Returns the selected GPU to stock clocks and the default power limit |
| `gao --set-core <mhz>` / `--set-mem <mhz>` / `--set-power <pct>` | Sets one knob directly with read-back (same bounds as the Manual page) |
| `gao --boot on\|off` | Turns apply-at-logon on or off |
| `gao --fan auto` | Hands every fan back to the NVIDIA driver, whatever set it (a running tray app takes them again on its next tick; switch Fan control off to keep the driver in charge) |
| `gao --status` | Saved profiles for all GPUs, the selected GPU's current settings, apply-at-logon state |
| `gao --probe` | Live telemetry: clocks, temperature, fan, power |
| `gao --stress <seconds>` | Runs the stress test alone and prints its verdict; changes nothing |
| `gao --bandwidth` | Measures the current memory bandwidth |

Ctrl+C during `--optimize` restores stock.

## FAQ

<details>
<summary><b>How does it handle more than one NVIDIA GPU?</b></summary>

Choose a card from the GPU dropdown at the top of the dashboard. Each card has its own profile, fan settings and crash strikes; optimizing another card does not replace the first card's profile. Apply at logon applies all saved profiles to their matching cards, while manual **Apply** and **Revert to stock** act on the selected card.
</details>

<details>
<summary><b>Does it work alongside MSI Afterburner?</b></summary>

Yes, as long as only one of them applies settings. If Afterburner applies an offset while GPU Auto Optimizer keeps its tune applied, the watchdog notices, says so in a notification and leaves the settings alone.
</details>

<details>
<summary><b>What happens after a driver update?</b></summary>

The saved profile is tied to the driver version it was tuned on. After an update it is not applied, and the app asks you to optimize again, because a new driver can change what is stable.
</details>

<details>
<summary><b>What if a setting crashes my PC?</b></summary>

During a search, the crash journal makes sure that setting is never tried again, and the next run stays below it. The Manual page uses the same journal for hand-applied values: extreme values need an explicit confirmation, and an unfinished entry resets every GPU to stock at the next logon instead of re-applying. At logon, three crashes in a row within two minutes of applying switch apply-at-logon off.
</details>

<details>
<summary><b>Can it control the fans?</b></summary>

Yes. Each profile comes with a fan curve, and you can pick another (Silent, Normal, Cool, Aggressive) for a run or afterwards. The Fan page lets you edit it: drag the points, and choose a temperature below which the fans stop once the card is cool and idle (the NVIDIA driver controls them there, so a crash or a killed app can never leave them stopped). The optimize run uses the profile's curve, so the tune is tested at the temperatures that curve produces; a quieter curve afterwards shows a warning. The curve runs while the tray app runs with administrator rights; otherwise the driver controls the fans.
</details>

<details>
<summary><b>Does it undervolt?</b></summary>

No. Locking a voltage point froze the reference card during development, so no profile touches the voltage curve. The Manual page explains this instead of hiding it: voltage points and per-P-state multipliers are read-only telemetry, with no knob.
</details>

<details>
<summary><b>Where does it keep its data?</b></summary>

In `%ProgramData%\GpuAutoOptimizer`: `gao.json` (selected GPU and per-GPU profiles/settings), `journal.jsonl` (the crash journal) and `boot.log`. Users can read the folder; only administrators can write it, because the logon task runs with administrator rights.
</details>

## Building from source

Visual Studio 2026 with the C++ and CMake components. From a Developer PowerShell in the repository root:

```powershell
cmake --workflow --preset ci    # configure, build Release, run the tests
```

This produces `build\Release\GpuAutoOptimizer.exe` and `build\Release\gao.exe`. See [docs/development.md](docs/development.md) for the code layout, the test setup and the hardware checklist.

## License

[MIT](LICENSE) © Rovey
