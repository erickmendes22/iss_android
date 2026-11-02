#!/system/bin/sh
# inFAMOUS Second Son — Daedalus Fork 0.1 on Android Bionic. No Debian/Xvfb/glibc.
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
# Developer profiling is opt-in; gameplay avoids its clocks and fault counters.
export ISS_COMPARE_STATS="${ISS_COMPARE_STATS:-0}"
# Readback outcomes distinguish successful deferred copies from stale-copy fallbacks.
export TLG_READBACK_STATS="${TLG_READBACK_STATS:-$ISS_COMPARE_STATS}"
export TLG_FAULT_PROFILE="${TLG_FAULT_PROFILE:-$ISS_COMPARE_STATS}"
# FSR 3.1 uses the existing native Vulkan scene/depth path; keep it opt-in.
# Never overwrite the Android launcher's FSR selection with a hardcoded "none".
case "${TLG_UPSCALER:-none}" in
 fsr3) export TLG_UPSCALER=fsr3 ISS_UPSCALER=fsr3 TLG_OBJECT_MOTION=1 ISS_OBJECT_MOTION=1 TLG_JITTER=1 ISS_JITTER=1 ;;
 *) export TLG_UPSCALER=none ISS_UPSCALER=off TLG_OBJECT_MOTION=0 ISS_OBJECT_MOTION=0 TLG_JITTER=0 ISS_JITTER=0 ;;
esac
# Capture is disabled in the performance build, including stale trigger files.
export ISS_FSR3_CAPTURE=0 ISS_FSR3_DIAGNOSTICS=0
unset TLG_DUMP_TRIGGER TLG_DUMP_DIR TLG_DUMP_FRAMES TLG_PRESENT_DUMP_TRIGGER TLG_PRESENT_DUMP_COUNT
export TLG_FSR1=0 TLG_RCAS=0
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
# Family-aware defaults: Gen8 on 8xx; v27 R1 on 6xx/7xx; system elsewhere.
case "${ISS_BIONIC_DRIVER:-auto}" in
 gen8) export ISS_VULKAN_DRIVER="$runtime/drivers/gen8/libvulkan_freedreno.so" ;;
 gen7) export ISS_VULKAN_DRIVER="$runtime/drivers/gen7/libvulkan_freedreno.so" ;;
 custom)
  case "${ISS_CUSTOM_DRIVER:-}" in
   "$base"/custom-drivers/*/libvulkan_freedreno.so) export ISS_VULKAN_DRIVER="$ISS_CUSTOM_DRIVER" ;;
   *) echo "Invalid imported driver path" >&2; exit 4 ;;
  esac ;;
 system) unset ISS_VULKAN_DRIVER ;;
 auto) case "$chip" in
   0x44??????) export ISS_VULKAN_DRIVER="$runtime/drivers/gen8/libvulkan_freedreno.so" ;;
   0x43??????|0x06??????) export ISS_VULKAN_DRIVER="$runtime/drivers/gen7/libvulkan_freedreno.so" ;;
   *) unset ISS_VULKAN_DRIVER ;;
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
