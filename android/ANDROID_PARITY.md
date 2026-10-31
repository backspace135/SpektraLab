# Android parity — stage-0 baseline on Adreno (issue #2)

> Status as of **2026-10-31**, on the connected device. This is the stage-0
> deliverable: what reproduces on Qualcomm Adreno, what does not, and the one
> engine change that closes the highest-numbered risk (R1). It follows the
> "Current state" format of `engine/PORT_COVERAGE.md` §5, and is the Adreno
> counterpart to the lavapipe baseline recorded there.
>
> Stage 0 answers one question: **does the engine's numerics reproduce on a
> Qualcomm chip, is there enough memory, and does the toolchain hold?** No UI
> is built or tested here.

## Device under test

| | |
|---|---|
| device | `24018RPACC` (Xiaomi Pad 7-class tablet), `ro.soc.model` = **SM8550** (Snapdragon 8 Gen 2) |
| GPU | **Adreno (TM) 740**, `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU` |
| Vulkan | api **1.3.128**, driver_version 2150252545, vendor 20803 |
| RAM | 7.64 GB total (`/proc/meminfo` MemTotal = 7642916 kB) — **below the 12 GB target** the plan assumes; see "Memory" |
| ABI / SDK | arm64-v8a, Android SDK 36 |
| toolchain | NDK **28.2.13676358** (r28c), glslang **14.3.0** (from the Android emulator bundle) |

Repro: `android/build-android.sh` (cross-compile) then `android/run-probes.sh`
(push + run over adb). Both are self-contained; the device needs only USB
debugging. Probes run as native executables from `/data/local/tmp/spektra`.

## Toolchain (tasks 0.1, 0.2) — PASS

- **0.1 NDK compile.** The engine cross-compiles clean for aarch64 with the
  stock NDK: `src/core`, `src/pipeline`, `src/gpu/vulkan_gpu.cpp`, volk.
  `libspektralab_engine.a` (13.7 MB) and the seven probe executables are
  `ELF 64-bit ARM aarch64 … for Android 28`. The only CMake additions needed
  were an `if(ANDROID)` guard on the Windows-only desktop target and linking
  `android`/`log`; nothing Apple- or Windows-specific leaked into the shared
  engine path.
- **0.2 SPIR-V passthrough.** glslang `--target-env vulkan1.0` compiles all
  **34** kernels; the Adreno driver **accepts every one** — the probes create a
  pipeline per kernel with no compile/link error. Only `GL_GOOGLE_include_directive`
  is used, resolved at build time, so nothing driver-fragile reaches the device.

## Device facts that decide the risks (task 0.3)

From `spk_android_device_info` (JSON on stdout):

| fact | value | risk it settles |
|---|---|---|
| `maxStorageBufferRange` | **134217728 (128 MiB)** | **R3** — same ceiling as lavapipe, *not* the 2 GB the plan estimated. A 45 MP float32 plane (~570 MB) exceeds it; see below. |
| `maxComputeWorkGroupInvocations` / `Size` | 1024 / [1024,1024,1024] | fine (≥ the 256 the kernels use) |
| `maxComputeWorkGroupCount` | [65535,65535,65535] | the 2D group-folding in `dispatch` covers >16.7 MP frames |
| `maxPerStageDescriptorStorageBuffers` / `maxDescriptorSetStorageBuffers` | 16777216 | far above the engine's ≥8 floor |
| `minStorageBufferOffsetAlignment` | 64 | — |
| `maxImageDimension2D` | 16384 | a >16384 px-wide frame renders but cannot be shown as one texture |
| memory heaps | heap 0: **7.83 GB** device_local; heap 1: 256 MB device_local | R2 sizing |
| `has_device_local_compute_type` | **true** | **R2** — Adreno *does* expose DEVICE_LOCAL compute types (indices 0–6 on heap 0), so the engine's allocator does **not** hit "no device-local Vulkan compute memory type". Unified memory: device-local heap 0 *is* system RAM. |

## R1 — double-float numerics (task 0.4): one real divergence, fixed

This was the top risk, and it fired. Two distinct Adreno behaviours surfaced:

### (a) fma is not fused — **resolved by the Dekker path**

`spk_math_probe` returns a residual of exactly 0 on every lane on the Adreno
740: **`precise fma` is lowered to a multiply and an add**, the same lowering
Mesa's lavapipe does — but Adreno is an *integrated GPU*, so the existing gate
(CPU-only) refused it and the engine would not start.

The engine already carries the remedy: an exact Veltkamp/Dekker `two_prod`
selected by specialization constant 0 (`spk_iir_df_acc.comp`,
`spk_geometry_resample_df.comp`). The fix, in `vulkan_gpu.cpp`'s
`check_math_mode`, extends that path from CPU-only to **any device whose probe
residuals are all exactly zero** (a clean unfuse) — while still refusing a
mixed (some lanes fused) or wrong-residual (genuinely fast-math) device. With
it, the double-float kernels land **bit-for-bit on the lavapipe baseline**:

| kernel | Adreno 740 | lavapipe baseline |
|---|---|---|
| `spk_iir_df_acc` | max abs error **1.49011612e-08** | 1.49e-8 |
| `spk_geometry_resample_df` | max abs error **4.99896339e-07** | 5.0e-7 |

`spk_math_probe` itself now passes on-device (`math_mode_ok: true`).

### (b) `pow` precision on the BT.709 breakpoint — **open, recorded, root-caused**

`spk_vulkan_pointwise` / `spk_cctf_decode` mode 3 (BT.709 OETF) fails on the
sample at the linear/power join: it renders **0.018** against the reference
**0.0180551012**, delta **5.51e-05** against a tolerance of 1.13e-07.

Root cause, established with an on-device diagnostic (a one-shot Vulkan runner
and an engine-shaped kernel, since removed): the mode-3 breakpoint
`alpha·pow(beta,0.45) − (alpha−1)` is computed by a **runtime `pow`** — glslang
emits an `ExtInst Pow` rather than folding it (verified on 14.3) — so the
breakpoint is exactly as precise as the *driver's* `pow`. Measured:

| who | breakpoint |
|---|---|
| MSVC host (Windows test reference) | `0x3da66554` |
| MinGW host (Android test reference) | `0x3da66555` |
| Adreno 740 runtime pow | ≈`0x3da66554` region but shifted enough to mis-route the boundary sample |
| colour.cpp double truth, narrowed | `0x3da66555` |

The value differs by **one ULP between the two host C runtimes already**, and
the Adreno `pow` adds its own error on top. Two consequences:

1. **No single literal fixes it.** Hard-coding `0x3da66555` makes the Adreno
   probe green but turns the desktop NVIDIA/MSVC probe red (its reference
   expects `0x3da66554`), and vice-versa. The breakpoint is genuinely
   non-portable as long as it is a per-runtime `pow`.
2. The power branch itself is within ~1 ULP on Adreno (it returns
   0.0180000048 for the true 0.0180000009) — so this is a *boundary-routing*
   precision issue, not a wrong transfer curve.

**Decision deferred to stage 1.** The correct fix is to stop deriving the
breakpoint in-shader: compute it once on the host (double, matching
colour.cpp) and pass it in via the meta buffer or a specialization constant, so
shader and reference share one value. That is a small wire/ABI decision
(`API-SPEC-callable-render-service.md`), not something to slip into a porting
probe — which is why it is recorded here rather than patched. **Not** resolved
by widening the tolerance: the breakpoint must stay exact or the encode/decode
pair drifts (colour bugs are silent, AGENTS trap 11).

Everything else in pointwise (the other 129 cases) passes on Adreno.

## ctest summary on Adreno (task 0.3)

| probe | result | note |
|---|---|---|
| `spk_android_device_info` | ✅ | device facts above; `math_mode_ok:true` |
| `spk_vulkan_smoke` | ✅ | FIR oracle, `iir_df_acc` 1.49e-8, headless session + render |
| `spk_vulkan_pointwise` | ❌ | `cctf_decode` BT.709 breakpoint only — the runtime-`pow` breakpoint is non-portable by 1 ULP across host runtimes and drivers (R1b, root-caused; fix deferred to stage 1) |
| `spk_vulkan_grain` | ✅ | Philox exact, Poisson/lognormal distributions in tolerance |
| `spk_vulkan_resample` | ✅ | mirror bilinear, max err 3.15e-7 |
| `spk_vulkan_transfer` | ✅ | ranges, dependencies, lifetimes |
| `spk_vulkan_geometry` | ✅ | 192 cases, df resample 4.999e-7 |
| `spk_vulkan_lut_di` | ✅ | 96 cases, max err 6.97e-7 |

## ctest summary on Adreno (task 0.3)

**7 of 8 probes green** on the Adreno 740 with the fma fix in place; the one
red is the recorded, root-caused BT.709 breakpoint (R1b) — a single sample in
one mode of one kernel, not a kernel class.

## What stage 0 did *not* run (and why)

- **0.5 memory stress (R2 under load).** Not run: needs a real 45 MP / 102 MP
  RAW and the `Debug.MemoryInfo`/`dumpsys` harness. The static facts are
  recorded above and they are **tighter than the plan assumed**: the device's
  128 MiB `maxStorageBufferRange` means a full-res 45 MP plane cannot be a
  single SSBO, so stage 1 must either tile the frame into ≤128 MiB storage
  buffers or confirm the driver accepts a larger binding (the CPU-only
  `probe_cpu_storage_range` lift does not apply to a GPU device). Combined with
  this tablet's **7.64 GB** RAM (under the 12 GB target), strip/tiled rendering
  is likely **required**, not optional. This is the stage-1 gate.
- **0.6 performance & thermal baseline (R4).** Not run: needs the timed
  live/preview/full renders and a 10-minute sustained run. The engine renders
  headless on-device (smoke passes), so the harness is ready; the numbers are
  not yet measured.
- **Whole-image parity against the Python/Metal oracle.** Out of scope for
  stage 0 and needs the reference tree; the grain probe says as much itself
  ("whole-frame Python/Metal parity remains unverified").

## Verdict for stage 0

**The toolchain holds, the engine builds and runs on Adreno, and the
double-float numerics reproduce bit-for-bit once the Dekker path covers an
unfused-fma GPU.** Two recorded divergences remain open and gate stage 1: the
BT.709 `pow` breakpoint (R1b) and the 128 MiB storage-buffer ceiling against a
45 MP frame (R3, on a 7.64 GB device). Neither is silently passed — both are
named above with their measurements.
