#!/usr/bin/env bash
# Build ISS's OWN Vulkan/HLE code; GoW3/Bloodborne supply the Android backend only.
set -Eeuo pipefail
cd -- "$(dirname "$0")/../../.."
ndk=${ISS_NDK:-${ANDROID_NDK_HOME:?Set ISS_NDK or ANDROID_NDK_HOME}}
api=${ISS_BIONIC_API:-30}
jobs=${ISS_JOBS:-2}
prefix=$PWD/.local-deps/android/bionic-prefix
headers=${ISS_BIONIC_HEADERS:-$PWD/.local-deps/root/usr}
build=$PWD/out/bionic/gpu
[[ -f "$prefix/.done-ffmpeg" ]] || { echo "Run tools/android/bionic/build_deps.sh" >&2; exit 1; }
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH CMAKE_PREFIX_PATH
export PATH="$headers/bin:$PATH"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$prefix/share/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
# The Android workflow configures port/gpu directly, bypassing port/build.sh.
# Apply BOTH the compact descriptor pool and the native-HAL dispatch fix before
# CMake sees the FSR-Vulkan headers. Otherwise the renderer compiles against the
# unpatched ABI while the source expects getInstanceProcAddr.
fsr_module="$PWD/port/gpu/third_party/fsr-vulkan"
fsr_patch_dir="$PWD/port/gpu/patches/fsr-vulkan"
[[ -f "$fsr_module/upstream/ffx-1.1.4/sdk/src/backends/vk/ffx_vk.cpp" ]] || {
  echo "Missing pinned FSR-Vulkan SDK in Android checkout" >&2
  exit 1
}
for patch_name in 0004-ffx-vk-compact-descriptor-pool-for-mobile.patch 0007-fsr3-direct-hal-dispatch.patch 0008-ffx-vk-compatible-memory-fallback.patch 0009-fsr3-on-demand-history-introspection.patch; do
  patch="$fsr_patch_dir/$patch_name"
  [[ -f "$patch" ]] || { echo "Required FidelityFX patch absent: $patch_name" >&2; exit 1; }
  if ! git -C "$fsr_module" apply --reverse --check "$patch" 2>/dev/null; then
    git -C "$fsr_module" apply --check "$patch" || {
      echo "Unable to apply FidelityFX patch: $patch_name" >&2
      exit 1
    }
    git -C "$fsr_module" apply "$patch"
  fi
  echo "FidelityFX patch verified: $patch_name"
done
grep -q 'PFN_vkGetInstanceProcAddr getInstanceProcAddr;' "$fsr_module/include/ffx_vk_portable.h" || {
  echo "FidelityFX native-HAL ABI missing after patch application" >&2
  exit 1
}
mkdir -p "$build/.cmake/api/v1/query"
touch "$build/.cmake/api/v1/query/codemodel-v2"
cmake -S port/gpu -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$api -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DISS_ANDROID_BIONIC=ON -DTLG_LTO=ON -DTLG_PGO=off \
  -DCMAKE_FIND_ROOT_PATH="$prefix" -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
  "-DCMAKE_PREFIX_PATH=$prefix;$headers" \
  -Dmagic_enum_DIR="$headers/share/cmake/magic_enum" \
  -Dtsl-robin-map_DIR="$headers/share/cmake/tsl-robin-map" \
  -DVulkanMemoryAllocator_DIR="$headers/share/cmake/VulkanMemoryAllocator" \
  -DBoost_DIR="$headers/lib/cmake/Boost-1.92.0" \
  -Dboost_headers_DIR="$headers/lib/cmake/boost_headers-1.92.0"
python3 tools/android/bionic/audit_link.py --build "$build" --prefix "$prefix" --ndk "$ndk"
if [[ ${1:-} = --configure-only ]]; then exit 0; fi
# Collect all further compile failures during one CI run instead of stopping at first object.
cmake --build "$build" --target tlggpu --parallel "$jobs" -- -k 0
mkdir -p out/bionic/lib
cp "$build/libtlggpu.so" out/bionic/lib/
# The native C preflight produced loader/HLE objects earlier in this workflow.
# Check their actual tlggpu_* imports against this freshly linked Android .so,
# preventing missing desktop-only C APIs from reaching the final probe link.
python3 tools/android/bionic/check_gpu_abi.py --ndk "$ndk" --root out/bionic
