#!/usr/bin/env bash
# Build the strict Android CPU from the same runtime revision used by SotC.
set -euo pipefail
cd "$(dirname "$0")/../../.."
src=${ISS_AOT_UPSTREAM:?Set to a Bloodborne-Android checkout}
pin=b6d4cfb6972c84d4029f54765406ec143df3c30e
test "$(git -C "$src" rev-parse HEAD)" = "$pin"
llvm="${ISS_NDK:?Set to Android NDK r29}/toolchains/llvm/prebuilt/linux-x86_64/bin"
out="$PWD/out/bionic/lib"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$out"
for name in guest_cpu_aot.cpp guest_cpu_aot_hooks.inc guest_cpu_native.cpp guest_cpu_native.h guest_cpu_cpuid.h guest_cpu_atomics.h guest_state.h host_call_arm64.S; do
  git -C "$src" show "$pin:cpu/$name" > "$work/$name"
done
git -C "$src" show "$pin:src/guest_cpu.h" > "$work/guest_cpu.h"
cp cpu/aarch64_store.h "$work/"
patch -s -d "$work" -p1 <tools/android/aot/iss-native-runtime.patch
"$llvm/aarch64-linux-android30-clang++" -std=c++20 -O2 -fPIC -shared \
  -fvisibility=hidden -fvisibility-inlines-hidden -fno-strict-aliasing -I"$work" \
  "$work/guest_cpu_aot.cpp" "$work/guest_cpu_native.cpp" "$work/host_call_arm64.S" \
  -Wl,-soname,libisscpu.so -Wl,--no-undefined -ldl -lm -o "$out/libisscpu.so"
"$llvm/aarch64-linux-android30-clang" -shared -nostdlib -Wl,-soname,libbbcpu.so \
  -Wl,--no-as-needed -L"$out" -lisscpu -o "$out/libbbcpu.so"
