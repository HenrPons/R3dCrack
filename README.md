# RedCrack

## About the project
This started as a solo project derived from [BitCrack](https://github.com/brichard19/BitCrack). It adds a puzzle-only GUI, live workload controls, automatic hardware detection and independent checkpoints. This is a preview, not a guarantee of finding a puzzle key or earning a reward.
I'm not a dev just an enthusiast so I'd expect problems but as far as I can tell everything seems to be working.

## Support
As I said this started as a small solo project that took on a life of its own and grew into what we have today, it took a few weeks since I had to learn everything from scratch and plenty of time torturing every coding agent to fix the hundreds of bugs my lack of experience created, if you feel like it, support my work with crypto it will be used to buy coffee and energy drinks.

| Asset | Address | QR code |
| --- | --- | --- |
| BTC | `bc1q2gf2v5jquxr35mmqe05eczjuzhn8hfs39n7q99` | <img src="assets/donations/btc.png" alt="BTC donation address QR code" width="160" height="160"> |
| SOL | `3ZNDAjktYNgY2PX3mLSMuS8nPkLvEc2W6JRuF4GjKr3n` | <img src="assets/donations/sol.png" alt="SOL donation address QR code" width="160" height="160"> |
| ETH | `0x1f0Cc174286b144CEd76978A57569E1E96B7E8b0` | <img src="assets/donations/eth.png" alt="ETH donation address QR code" width="160" height="160"> |

## Features

- A GUI and a built-in puzzle catalog.
- CPU and GPU search together, with individual and combined key rates.
- Automatic OpenCL GPU discovery; choose **All GPUs** or one device.
- CPU workers across active Windows processor groups, including multi-socket systems.
- Live CPU/GPU active-time targets, pause/resume and graceful stop with checkpoint saving.
- Separate results and checkpoints for each GPU slice and the CPU partition.
- Known-key self-test and WIF output.

No developer fee, telemetry, payment system, automatic update or license activation is implemented. The search application does not connect to a pool or send results over the network.

## Requirements

- Windows 10/11, 64-bit.
- For GPU search: a compatible GPU and its vendor's OpenCL driver.
- For CPU-only search: no OpenCL runtime is required.
- A writable application directory for settings, checkpoints and results.

The supported build path is Windows x64 with OpenCL. Inherited CUDA/Linux sources are retained for attribution and future development; they are not a validated RedCrack release configuration.

## Build from source

Install Visual Studio 2022 Build Tools with **Desktop development with C++**, the MSVC v143 toolset and Windows SDK. Obtain OpenCL headers and the x64 `OpenCL.lib` import library from the [Khronos OpenCL SDK](https://github.com/KhronosGroup/OpenCL-SDK).

Use PowerShell 7 from the repository root. Either place those dependencies under `deps/OpenCL-SDK/include` and `deps/OpenCL-SDK/lib`, or set `OPENCL_INCLUDE` and `OPENCL_LIB` to their directories.

```powershell
.\scripts\build_gui_windows.ps1
```

Outputs:

```text
x64/Release/RedCrack.exe
x64/Release/clBitCrack.exe
```

The build compiles the kernel embedding tool from the included source; no prebuilt helper is committed. GPU drivers and SDK dependencies are not installed automatically.

Create a portable package, including the Visual C++ runtime, license, user guide and SHA-256 manifest:

```powershell
.\scripts\package_gui_windows.ps1 -SkipBuild
```

The script requires the Visual C++ redistributable files supplied with Build Tools. Generated binaries and archives are intentionally excluded from Git.

## Run

1. Extract the complete portable package into a writable folder.
2. Open **RedCrack.exe**. Keep `clBitCrack.exe` and the runtime DLLs beside it.
3. Check the hardware detected in the activity log. Enable CPU, GPU, or both.
4. Run **Self-test** before starting a real search.
5. Select a puzzle and click **Start search**.

GPU defaults to 80% active time and CPU to 50%. These percentages are **duty-cycle targets**, not exact Windows utilization, power or temperature limits. Initialization is not throttled. At 0%, that category pauses. The GPU percentage applies to every selected GPU.

**Pause** pauses all active workers after their current batch; **Stop and save** finishes the current batch and saves progress. Minimizing keeps the search running. Closing requests a graceful stop, which can take longer during OpenCL initialization.

The GPU fields **B**, **T** and **P** mean blocks, threads and points. They apply to every selected GPU and may require hardware-specific adjustment. The app does not overclock hardware. Processes run at below-normal priority.

## Performance

An author-reported run on an AMD Radeon RX 6750 XT achieved approximately
763 MKey/s with `-b 120 -t 256 -p 768`. This is a tuned configuration; the
GUI defaults are `B 120 / T 256 / P 128`. Driver version and clock settings
were not recorded, so this is not a reproducible benchmark or a guarantee.
Any overclocking is performed externally by the user, not by RedCrack.

## Expectations

Puzzle #71 contains 2^70 candidate keys. At a sustained 763 million keys per
second, scanning the full range takes about 49,031 years. If the key is
uniformly distributed in that range, the mean time to reach it in one
sequential full scan is about 24,516 years. This assumes uninterrupted
operation and unchanged speed; it does not predict when a reward will be won.

## Search ranges and resuming

The v3 layout covers **100% of the puzzle range across the selected engines**:

- **CPU only:** CPU searches the full range.
- **GPU only:** selected GPUs split the full range; selecting a single GPU gives it the full range.
- **CPU + GPU:** CPU receives the upper 1/1024; selected GPUs divide the lower 1023/1024 into contiguous slices.
- Pausing an engine (including setting it to 0%) keeps its range assigned to it. Work is not dynamically reassigned when a worker pauses or completes.
- A worker error stops the other workers rather than silently continuing with incomplete coverage.
- Checkpoint names include engine category, partition and slice layout. Changing modes or the number of selected GPUs uses a different set and can repeat earlier work.
- v2 checkpoints are not imported automatically. Keep them as backups; do not rename them into v3 filenames.
- Normal stops save progress. A crash or result-buffer overflow can repeat work since the previous checkpoint; an incomplete batch is never marked complete.

The catalog is built into the app, not live chain data. Verify that a puzzle
is still open before starting a long search.

See [the full user guide](packaging/README.txt) for filenames and operational details.

## Validation and limitations

```powershell
.\scripts\build_host_tests_windows.ps1
.\scripts\test_checkpoint_windows.ps1
.\scripts\test_hybrid_windows.ps1
.\scripts\test_partitions_windows.ps1
python .\scripts\check_publication.py
```

Tests cover host arithmetic, CPU target coverage, inclusive range endpoints, slice boundaries, checkpoint locking/resume and live workload changes. Slice integration tests run CPU workers against GPU partition coordinates; they do not substitute for physical multi-GPU tests.

Known limitations:

- Physical multi-GPU/multi-socket operation and NVIDIA hardware still need validation.
- CPU search is an initial scalar implementation without SIMD optimization.
- High-DPI layouts and long/non-ASCII installation paths are not fully validated. Use a short, ordinary writable directory.
- GPU/CPU percentages share one control per category, not separate controls for each device.

## Security and privacy

Found keys and WIFs are stored locally in **plaintext**. Treat result files as sensitive. The application directory and worker executable must be trusted; run as a regular user, not administrator. A malicious process running as the same user is outside the isolation guarantees of this desktop app.

See [SECURITY.md](SECURITY.md) for reporting guidance and trust boundaries. The publication check is a heuristic hygiene check, not a complete secret scanner or independent security audit.

## License and attribution

[MIT](LICENSE.MIT). Retain the upstream BitCrack copyright and license notice when redistributing. The original RedCrack SVG/icon assets are supplied under the same license. Microsoft runtime components remain subject to Microsoft's terms.
