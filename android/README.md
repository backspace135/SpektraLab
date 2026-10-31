# android/ — stage-0 feasibility for the Qualcomm port (issue #2)

Everything here serves issue #2 (阶段 0 · 可行性验证): prove the engine's
numerics, memory and toolchain on a real Qualcomm Adreno tablet **before any UI
is written**. It is self-contained — the only edits outside this folder are a
small `if(ANDROID)` guard in `engine/CMakeLists.txt` and the `check_math_mode`
fix in `engine/src/gpu/vulkan_gpu.cpp` that this stage surfaced.

| file | what it does |
|---|---|
| `build-android.sh` | Cross-compile the engine + Vulkan probes for aarch64 with the NDK (task 0.1) and emit the 34 SPIR-V kernels (task 0.2). Outputs land in `android/build/`. |
| `android_device_info.cpp` | On-device probe (task 0.3): deviceName, `maxStorageBufferRange`, the memory-type table and the math-mode guard, as one JSON object. Built as `spk_android_device_info`. |
| `run-probes.sh` | Push the probes + `libc++_shared.so` + baked resources to a USB-connected device and run the full probe set (tasks 0.3/0.4). |
| `ANDROID_PARITY.md` | The stage-0 baseline: what reproduces on Adreno 740, the one divergence found and fixed (unfused fma → Dekker), and the two recorded open items that gate stage 1. |

## Run it

```sh
# needs: Android NDK (ANDROID_SDK_ROOT or D:/tools/android-sdk), glslang
# (PATH, or the emulator's bundle), and a device on adb with USB debugging.
android/build-android.sh          # cross-compile + SPIR-V
android/run-probes.sh             # push + run all probes on the device
```

`run-probes.sh` exits non-zero if any probe fails, and prints each probe's
stdout (device facts as JSON, kernel accuracy figures as text). The build and
the push/run are separate so the compile can be iterated without a device.

## Environment notes

- `ANDROID_NDK_HOME` here points at a `.broken` stub; `build-android.sh` falls
  back to the real `ndk/28.2.13676358` tree.
- The probes link `c++_shared`, so `run-probes.sh` pushes `libc++_shared.so`
  from the NDK and sets `LD_LIBRARY_PATH`.
- Vulkan headers come from the NDK sysroot (v275) — no separate SDK fetch.
