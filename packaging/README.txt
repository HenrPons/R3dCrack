REDCRACK - WINDOWS x64 PREVIEW

GETTING STARTED
Extract the entire ZIP into a writable folder and open RedCrack.exe.
Keep clBitCrack.exe and all DLLs beside it. Do not run inside the ZIP.
No Visual Studio or Python installation is needed to run the app.
Requires Windows 10/11 x64. CPU mode works without OpenCL; GPU mode
requires the manufacturer's OpenCL driver. NVIDIA hardware is not yet
validated. Run Self-test with your chosen engines before searching.

CONTROLS
Select a puzzle and enable CPU, GPU, or both. Defaults: GPU 80%, CPU 50%.
Sliders can be changed live; 0% pauses that engine. Pause pauses all active workers after their current batch.
Stop and save waits for the current work and saves checkpoints.
Minimize keeps searching. Closing requests a graceful stop and save.
Stopping during OpenCL initialization can take longer.
Percentages target ACTIVE TIME, not exact Windows utilization, temperature,
or power consumption. Initialization is not throttled. Processes run at
below-normal priority. Adjust the settings to keep your computer responsive.
GPU defaults: All GPUs, blocks (B) 120, threads (T) 256, points (P) 128. These are
starting values, not hardware-specific recommendations. Settings persist
in settings.ini. No overclocking is performed.

HARDWARE DETECTION
The GUI detects OpenCL GPUs on startup and lists their driver-provided names.
Choose All GPUs or a single GPU. The GPU percentage applies to every active
GPU; its speed display is their combined rate. B/T/P settings apply to all
selected GPUs. Hardware details appear in the activity log.
CPU workers span all active Windows processor groups, including multi-socket
systems. The CPU percentage applies to the complete CPU worker pool.
Restart the app after adding/removing hardware or changing drivers.

RANGES AND CHECKPOINTS
The v3 layout covers 100% of the puzzle across the selected engines:
- CPU only: full puzzle range.
- GPU only: full range split across selected GPUs (one GPU gets all of it).
- CPU + GPU: CPU gets the upper 1/1024, GPUs split the lower 1023/1024.
A paused engine retains its range; there is no dynamic reassignment.
Any worker error stops the remaining workers.

Checkpoint/result filenames include puzzle, v3, engine, partition and
slice index/count, for example checkpoint_p71_v3_gpu_full_slice0of1.txt.
Changing mode or GPU count creates a different set and may repeat work.
v2 and legacy checkpoints are not imported; keep them as backups.
Do not rename checkpoints to force them into a different range.
Self-tests use separate result files. Checkpoint writes are atomic and
protected against simultaneous worker use. Normal stops save progress.
After a crash or result-capacity error, resume the previous checkpoint.
On result overflow, reduce B/P before resuming. The incomplete batch is
not marked complete; repeating some work is intentional.

SOURCE AND VERIFICATION
Source: https://github.com/HenrPons/RedCrack
Release/source revision: see BUILD_INFO.txt in the package.
Build and test instructions are in the repository README.
SHA256SUMS.txt detects file corruption, not publisher identity.

PREVIEW STATUS
Includes multithreaded CPU search, OpenCL GPU search, live load controls,
dark GUI, WIF output, pause/resume and independent checkpoints.
CPU is a conservative initial implementation without SIMD optimization.
Run Self-test on your machine before searching. A passing self-test does
not guarantee performance or success on another puzzle.
Physical multi-GPU/multi-socket operation, high-DPI layouts and NVIDIA
hardware still require validation.
No developer fee, payment system or license activation is implemented.

OPEN SOURCE
Derived from BitCrack by Ben Richard. See LICENSE.MIT; retain upstream
copyright and license notices when redistributing source or binaries.
The original RedCrack icon and editable SVG source are available in the
repository under the same MIT license.
Microsoft Visual C++ runtime DLLs retain Microsoft's applicable terms.
