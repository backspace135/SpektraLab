#!/usr/bin/env bash
# run-probes.sh — push the stage-0 probes to a connected Android device and run
# them (issue #2 tasks 0.3 and 0.4).
#
#   android/run-probes.sh [--serial <id>] [--device-dir /data/local/tmp/spektra]
#
# Pushes the probes, libc++_shared.so and the baked resources to the device,
# then runs spk_android_device_info and every spk_vulkan_* ctest driver under
# adb. Each probe's exit code and stdout are captured; a non-zero exit is
# reported, never silently passed.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$here/build/engine"

SDK="${ANDROID_SDK_ROOT:-D:/tools/android-sdk}"
ADB="$SDK/platform-tools/adb.exe"
NDK="$SDK/ndk/28.2.13676358"
LIBCXX="$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
DEVICE_DIR="/data/local/tmp/spektra"
SERIAL=""
for arg in "$@"; do
  case "$arg" in
    --serial=*) SERIAL="${arg#*=}" ;;
    --device-dir=*) DEVICE_DIR="${arg#*=}" ;;
    *) echo "usage: $0 [--serial=ID] [--device-dir=PATH]" >&2; exit 2 ;;
  esac
done
ADB_S=("$ADB")
[ -n "$SERIAL" ] && ADB_S=("$ADB" -s "$SERIAL")

# Git Bash rewrites absolute POSIX paths in arguments to Windows paths; the
# device-side paths must reach adb verbatim, while the local (host) paths must
# be native Windows form for adb.exe. So: no path conversion at all, and we
# hand adb native host paths ourselves.
export MSYS_NO_PATHCONV=1
winpath() { cygpath -m "$1"; }   # mixed (forward-slash) Windows path for adb.exe
build_w="$(winpath "$build")"
LIBCXX_W="$(winpath "$LIBCXX")"

PROBES=(spk_android_device_info spk_vulkan_smoke spk_vulkan_pointwise \
        spk_vulkan_grain spk_vulkan_resample spk_vulkan_transfer \
        spk_vulkan_geometry spk_vulkan_lut_di)

echo "== device ==" >&2
"${ADB_S[@]}" shell getprop ro.product.model >&2
"${ADB_S[@]}" shell getprop ro.soc.model >&2 || true

echo "== pushing probes + resources ==" >&2
"${ADB_S[@]}" shell "mkdir -p '$DEVICE_DIR'" >&2
for p in "${PROBES[@]}"; do
  "${ADB_S[@]}" push "$build_w/$p" "$DEVICE_DIR/$p" >&2 || exit 1
done
"${ADB_S[@]}" push "$LIBCXX_W" "$DEVICE_DIR/libc++_shared.so" >&2 || exit 1
# Resources: the baked profiles/LUTs plus the freshly built SPIR-V.
"${ADB_S[@]}" push "$build_w/resources/." "$DEVICE_DIR/resources" >&2 || exit 1
"${ADB_S[@]}" shell "chmod 755 $DEVICE_DIR/spk_*" >&2

echo "== running probes ==" >&2
overall=0
for p in "${PROBES[@]}"; do
  echo "--- $p ---" >&2
  out="$("${ADB_S[@]}" shell "cd '$DEVICE_DIR' && LD_LIBRARY_PATH='$DEVICE_DIR' ./'$p' '$DEVICE_DIR/resources'" 2>&1)"
  code=$?
  printf '%s\n' "$out"
  echo "[exit=$code]" >&2
  [ "$code" = 0 ] || overall=1
done
echo "== overall: $([ "$overall" = 0 ] && echo PASS || echo FAIL) ==" >&2
exit "$overall"
