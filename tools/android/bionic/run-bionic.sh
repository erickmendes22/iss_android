#!/system/bin/sh
# inFAMOUS Second Son Daedalus v0.2 experimental Mali (Bionic; no FSR).
set -eu
files=${FXD_FILES:?ISS Bionic requires a private data directory}
base="$files/issport"
runtime="$base/arm64"
python="$runtime/python/bin/python3.13"
probe="$runtime/bin/iss-probe"
export HOME="$files/home/iss" TMPDIR="$base/tmp" XDG_RUNTIME_DIR="$base/tmp"
export PATH=/system/bin
export LD_LIBRARY_PATH="$runtime/lib:$runtime/python/lib"
export PYTHONHOME="$runtime/python" PYTHONPATH="$base/scripts" PYTHONUTF8=1
export FEX_APP_CONFIG_LOCATION="$HOME/.fex-emu/" FEX_APP_DATA_LOCATION="$HOME/.fex-emu/"
export FEX_APP_CACHE_LOCATION="$HOME/.cache/fex-emu"
export TLG_DATA_DIR="$base" TLG_GAME_DIR="${ISS_GAME_DIR:?Choose the extracted inFAMOUS game folder}"
export ISS_BIONIC=1 ISS_ANDROID_INPUT=1 TLG_ANDROID_INPUT=1
# Compare phase-level host waits without restoring expensive per-packet diagnostics.
export ISS_COMPARE_STATS="${ISS_COMPARE_STATS:-1}"
# Readback outcomes distinguish successful deferred copies from stale-copy fallbacks.
export TLG_READBACK_STATS="${TLG_READBACK_STATS:-$ISS_COMPARE_STATS}"
export TLG_FAULT_PROFILE="${TLG_FAULT_PROFILE:-$ISS_COMPARE_STATS}"
# Native Bionic deliberately has no optional temporal or spatial upscaling.
export TLG_UPSCALER=none ISS_UPSCALER=off TLG_OBJECT_MOTION=0 TLG_FSR1=0 TLG_RCAS=0
export TLG_PAD_FILE="$base/android-pad.state"
export TLG_CONFIG="$base/user/tlg.ini" ISS_CONFIG="$base/user/tlg.ini"
export TLG_GPU_USER_DIR="$base/user"
export TLG_AUDIO_FIFO="$base/audio.fifo"
# Native AudioOut mixes all ports to AudioBridge's fixed s16/stereo/48 kHz FIFO.
# SDL remains available for gamepads; no SDL audio device/JNI is used by Bionic.
unset SDL_AUDIO_DRIVER SDL_AUDIODRIVER SDL_AUDIO_DISK_OUTPUT_FILE SDL_AUDIO_DISK_TIMESCALE
export TLG_PAD_QUIET=1
export ISS_PRESENT_SOCKET="$base/present.sock"
export ISS_SCREEN_W="${ISS_SCREEN_W:-1280}" ISS_SCREEN_H="${ISS_SCREEN_H:-720}"
mkdir -p "$TMPDIR" "$HOME/.fex-emu" "$HOME/.cache/fex-emu" "$base/user" "$base/out"
[ -x "$python" ] || { echo "Bionic Python executable missing" >&2; exit 127; }
[ -x "$probe" ] || { echo "Bionic inFAMOUS probe missing" >&2; exit 127; }
chip=$("$runtime/bin/kgsl-chip-id" 2>/dev/null || true)
# Family-aware defaults: Adreno Turnip, eligible Mali PanVK, others system.
mali_allowed=${ISS_MALI_ELIGIBLE:-0}
case "${ISS_BIONIC_DRIVER:-auto}" in
 gen8) export ISS_VULKAN_DRIVER="$runtime/drivers/gen8/libvulkan_freedreno.so" ;;
 gen7) export ISS_VULKAN_DRIVER="$runtime/drivers/gen7/libvulkan_freedreno.so" ;;
 custom)
  case "${ISS_CUSTOM_DRIVER:-}" in
   "$base"/custom-drivers/*/libvulkan_freedreno.so) export ISS_VULKAN_DRIVER="$ISS_CUSTOM_DRIVER" ;;
   *) echo "Invalid imported driver path" >&2; exit 4 ;;
  esac ;;
 panvk)
  [ "$mali_allowed" = 1 ] || { echo "PanVK requires Mali Android 15+" >&2; exit 4; }
  export ISS_VULKAN_DRIVER="$runtime/drivers/panvk/libvulkan_panvk.so" ;;
 system) unset ISS_VULKAN_DRIVER ;;
 auto) case "$chip" in
   0x44??????) export ISS_VULKAN_DRIVER="$runtime/drivers/gen8/libvulkan_freedreno.so" ;;
   0x43??????|0x06??????) export ISS_VULKAN_DRIVER="$runtime/drivers/gen7/libvulkan_freedreno.so" ;;
   *)
    if [ "$mali_allowed" = 1 ] && [ -s "$runtime/drivers/panvk/libvulkan_panvk.so" ]; then
     export ISS_VULKAN_DRIVER="$runtime/drivers/panvk/libvulkan_panvk.so"
    else
     unset ISS_VULKAN_DRIVER
    fi ;;
  esac ;;
 *) echo "Invalid Bionic Vulkan driver selection" >&2; exit 4 ;;
esac
if [ -n "${ISS_VULKAN_DRIVER:-}" ]; then
 [ -s "$ISS_VULKAN_DRIVER" ] || { echo "Selected Bionic Vulkan driver missing: $ISS_VULKAN_DRIVER" >&2; exit 4; }
 echo "ISS Bionic driver: $ISS_VULKAN_DRIVER, chip=$chip"
else
 echo "ISS Bionic Vulkan: Android system loader, chip=$chip"
fi
echo "ISS Android Bionic: game=$TLG_GAME_DIR, output=${ISS_SCREEN_W}x${ISS_SCREEN_H}"
export ISS_LSFG_STATE_FILE="$base/framegen.state"
export ISS_LSFG_LIB="$runtime/lib/libbloodframe.so"
export ISS_LSFG_DLL="$base/Lossless.dll"

exec "$python" "$base/launch.py"
