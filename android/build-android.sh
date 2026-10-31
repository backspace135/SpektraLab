#!/usr/bin/env bash
# build-android.sh — cross-compile the SpektraLab engine and its Vulkan probes
# for Android aarch64 with the NDK, producing the artefacts issue #2 stage 0
# (tasks 0.1/0.2) needs before any of them touch a device.
#
#   engine/../android/build-android.sh [--abi arm64-v8a] [--api 28] [--clean]
#
# Outputs (all under android/build/):
#   engine/libspektralab_engine.a     the static engine (core+pipeline+vulkan)
#   probes/spk_vulkan_*               the on-device ctest drivers (executables)
#   resources/vulkan/*.spv            the 34 SPIR-V kernels (glslang, vulkan1.0)
#   resources/**                      the baked engine resources
#
# The probes are native executables (not an .apk): they are pushed to the
# device and run under adb from a shell, exactly as the ctest drivers run on
# the desktop. No UI is built here — stage 0 builds nothing but the engine.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"

ABI="arm64-v8a"
API="28"
CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --abi=*) ABI="${arg#*=}" ;;
    --api=*) API="${arg#*=}" ;;
    --clean) CLEAN=1 ;;
    *) echo "usage: $0 [--abi=arm64-v8a] [--api=28] [--clean]" >&2; exit 2 ;;
  esac
done

# --- toolchain locations (Windows host, NDK under the SDK) -------------------
SDK="${ANDROID_SDK_ROOT:-D:/tools/android-sdk}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/28.2.13676358}"
# ANDROID_NDK_HOME may point at a ".broken" stub; use the real tree instead.
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || NDK="$SDK/ndk/28.2.13676358"
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || {
  echo "android.toolchain.cmake not found under $NDK" >&2; exit 1; }

# glslang: prefer PATH, else the copy bundled with the Android emulator.
GLSLANG="$(command -v glslangValidator || true)"
[ -n "$GLSLANG" ] || GLSLANG="$SDK/emulator/lib64/vulkan/glslangValidator.exe"
[ -x "$GLSLANG" ] || { echo "glslangValidator not found" >&2; exit 1; }

# Vulkan headers shipped inside the NDK sysroot (v275) — no separate SDK fetch.
VK_HEADERS="$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include"
[ -f "$VK_HEADERS/vulkan/vulkan_core.h" ] || {
  echo "Vulkan-Headers not found in NDK sysroot: $VK_HEADERS" >&2; exit 1; }

build="$here/build"
[ "$CLEAN" = 1 ] && rm -rf "$build"
mkdir -p "$build"

echo "NDK        : $NDK" >&2
echo "glslang    : $GLSLANG" >&2
echo "Vulkan hdrs: $VK_HEADERS" >&2
echo "ABI/API    : $ABI / android-$API" >&2
echo "build dir  : $build" >&2

# Ninja + the NDK's own clang. ctest drivers are needed, so BUILD_TESTING=ON,
# but the desktop/host/raw targets are OFF (LibRaw is not part of stage 0).
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM="android-$API" \
  -DANDROID_STL=c++_shared \
  -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
  -DSPEKTRALAB_GLSLANG="$GLSLANG" \
  -DSPEKTRALAB_VULKAN_HEADERS="$VK_HEADERS" \
  -DSPEKTRALAB_REQUIRE_VULKAN=ON \
  -DSPEKTRALAB_BUILD_NATIVE_RAW=OFF \
  -DSPEKTRALAB_BUILD_HOST=OFF \
  -DSPEKTRALAB_BUILD_DESKTOP=OFF \
  -DSPEKTRALAB_BUILD_TEST_DLL=OFF \
  -DBUILD_TESTING=ON

ninja -C "$build" \
  spektralab_engine \
  spektralab_vulkan_resources \
  spk_vulkan_smoke spk_vulkan_grain spk_vulkan_pointwise spk_vulkan_resample \
  spk_vulkan_transfer spk_vulkan_geometry spk_vulkan_lut_di \
  spk_android_device_info

echo "OK: engine + probes + SPIR-V built under $build" >&2
