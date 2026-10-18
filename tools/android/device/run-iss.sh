#!/system/bin/sh
# Android/Bionic shell supervising the separate glibc/FEX game process.
# Never put rootfs/usr/bin ahead of /system/bin: Android cannot exec those ELFs directly.
set -eu
export PATH=/system/bin:/system/xbin
umask 077
: "${FXD_FILES:?Missing app files directory}"
: "${FXD_ROOT:?Missing glibc rootfs}"
: "${ISS_GAME_DIR:?Missing selected game directory}"
root="$FXD_ROOT"
base="$FXD_FILES/issport"
loader="$root/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
python="$root/usr/bin/python3.13"
game="$ISS_GAME_DIR"
for required in "$loader" "$python" "$base/bin/iss-probe" "$base/lib/libisscpu.so" \
                "$base/lib/libtlggpu.so" "$base/launch.py" "$game/eboot.bin"; do
  if [ ! -r "$required" ]; then
    echo "ISS preflight: missing or unreadable $required" >&2
    exit 31
  fi
done
# Pick the Linux/glibc KGSL Turnip, not a Bionic driver from the Android system.
base_icd="$root/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json"
gen8_icd="$root/usr/share/vulkan/icd.d/freedreno_gen8_icd.aarch64.json"
chip="unknown"
if [ -r "$base/bin/kgsl-chip-id" ]; then
  chip=$("$loader" "$base/bin/kgsl-chip-id" 2>/dev/null) || chip="unavailable"
fi
case "$chip" in
  0x44*)
    icd="$gen8_icd"
    echo "ISS GPU: $chip (Adreno Gen8); selecting bundled Linux/glibc Turnip Gen8"
    ;;
  *)
    icd="$base_icd"
    echo "ISS GPU: $chip; selecting base Linux/glibc Turnip (Gen8 not selected)"
    ;;
esac
if [ ! -r "$icd" ]; then
  echo "ISS preflight: selected Vulkan ICD missing: $icd" >&2
  exit 32
fi
/system/bin/mkdir -p "$base/out" "$base/user" "$FXD_FILES/home/iss" || exit 33
cd "$base" || exit 34
export HOME="$FXD_FILES/home/iss"
export TMPDIR="$root/tmp" XDG_RUNTIME_DIR="$root/tmp"
export FXD_SHM_SOCKET="$root/tmp/.fxshm/sock"
export FEXDROID_PRESENT="$root/tmp/fxpresent.sock"
# Android draws its own controls; avoid two synchronous Xvfb cursor/geometry
# reply round trips for every direct-present frame.
export ISS_HIDE_X11_CURSOR=1
export PYTHONHOME="$root/usr" PYTHONNOUSERSITE=1 PYTHONIOENCODING=utf-8
export TLG_GAME_DIR="$game" TLG_DATA_DIR="$base" TLG_USER_DIR="$base/user"
export ISS_CONFIG="$base/user/tlg.ini"
export TLG_CONFIG="$ISS_CONFIG"
export LD_LIBRARY_PATH="$base/lib:$root/usr/lib/aarch64-linux-gnu:$root/usr/lib:$root/usr/lib/aarch64-linux-gnu/pulseaudio"
# Runtime linker preflight catches future missing transitive dependencies
# without reprocessing the ~130 MiB eboot image.
for target in "$base/bin/iss-probe" "$base/lib/libtlggpu.so" "$base/lib/libisscpu.so" "$root/usr/lib/aarch64-linux-gnu/libvulkan_freedreno_gen8.so"; do
  if ! deps=$("$loader" --list "$target" 2>&1); then
    echo "ISS ELF dependency preflight failed ($target): $deps" >&2
    exit 35
  fi
  case "$deps" in
    *"not found"*|*"cannot open shared object file"*)
      echo "ISS ELF dependency missing ($target): $deps" >&2
      exit 35 ;;
  esac
done
echo "ISS ELF preflight OK: probe, GPU, CPU and Linux Turnip Gen8 dependencies resolved"
# Linux Turnip on KGSL uses software WSI for the Xvfb display. Its Mesa
# fxpresent path sends completed frames directly to the Android bridge.
# Do NOT let the Android bridge poll/copy Xvfb's mutable framebuffer unless
# direct presentation has actually failed (e.g. non-fullscreen X window).
export MESA_VK_WSI_DEBUG=sw
export DISPLAY=:0 SDL_VIDEODRIVER=x11
# SDL3 disk driver feeds the Android AudioTrack FIFO; do not use a host Linux sound daemon.
# SDL3 disk driver: emit raw PCM into the FIFO consumed by Android AudioTrack.
# Older SDL_DISKAUDIOFILE is ignored by SDL3; the SDL_AUDIO_* hints are mandatory.
export SDL_AUDIO_DRIVER=disk SDL_AUDIODRIVER=disk
export SDL_AUDIO_DISK_OUTPUT_FILE="$base/audio.fifo" SDL_AUDIO_DISK_TIMESCALE=0
export SDL_AUDIO_FORMAT=S16LE SDL_AUDIO_CHANNELS=2 SDL_AUDIO_FREQUENCY=48000
export ISS_ANDROID_INPUT=1 TLG_PAD_FILE="$base/android-pad.state" TLG_PAD_QUIET=1
export TLG_APP_NAME="inFAMOUS Second Son" TLG_APP_ID=com.daedalus.iss
export VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES="$icd"
export VK_LOADER_LAYERS_DISABLE='~implicit~'
echo "ISS preflight OK: Android shell, glibc loader, FEX, Vulkan ICD and game entry readable"
echo "ISS launching upstream inFAMOUS preparation under explicit ARM64 glibc loader"
exec "$loader" "$python" "$base/launch.py"
