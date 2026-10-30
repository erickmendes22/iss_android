# Offline FSR3 capture and production-motion replay

This harness keeps captured-buffer experiments reproducible instead of replacing
them with CPU bookkeeping tests. No game images, binaries, APKs or raw captures
are committed. The SDK Vulkan setup/helpers derive from the pinned MIT-licensed
FSR-Vulkan test `tests/fsr3_vk_backend_smoke.cpp` (copyright 2026 Q2RTX FSR Vulkan
contributors). Its original notice remains in each generated translation unit.

Requirements: C++17, CMake, Ninja, Vulkan headers/loader, software Vulkan such as
Mesa lavapipe, and glslangValidator. Initialize the pinned FSR-Vulkan submodule.

```sh
python tools/android/fsr3_capture_replay/prepare.py --output /tmp/iss-replay-source
cmake -S /tmp/iss-replay-source -B /tmp/iss-replay-build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/iss-replay-build --target iss_replay iss_motion --parallel 2
glslangValidator -V --target-env vulkan1.1 -S comp \
  port/gpu/shadps4/video_core/host_shaders/camera_motion.comp -o /tmp/iss-motion.spv
```

Preparation archives the exact SDK commit into a separate owned source copy,
applies the same four Android backend patches, and adds frame-index seeding only
to that copy. It never adds a write/seed API to the player SDK or Android APK.

```sh
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
/tmp/iss-replay-build/iss_replay CAPTURE_DIR OUTPUT_DIR [MOTION_OVERRIDE_DIR]
/tmp/iss-replay-build/iss_motion CAPTURE_DIR MOTION_OUTPUT_DIR /tmp/iss-motion.spv
ISS_MOTION_REPLAY=/tmp/iss-replay-build/iss_motion ISS_MOTION_SPIRV=/tmp/iss-motion.spv \
  python -m unittest discover -s tools/android/runtime -p test_iss_motion_recovery_gpu.py
```

Both drivers currently use 640x360 input and 1280x720 output. `iss_replay` uses
recorded color/depth/motion, jitter, timing and SDK history index. After a first
dispatch it seeds the SDK's current color, accumulation and reactivity from
captured f000 resources, then replays f001. Earlier luma/exposure history is not
fully captured; quantify the baseline error against the phone before comparing
variants. With captures 22/23, mean RGB output differences were 0.02580/0.01185
in 8-bit units. This is a close numerical reproduction, not a byte-exact replay
of the full preceding session.

`iss_motion` runs the **actual production GLSL** compiled to SPIR-V. It currently
uses native mode 3 with an identity camera transform and zero guest jitter,
suitable for these stationary-camera captures and synthetic guard cases.
Do not use it to claim reproduction of moving-camera captures without supplying
their actual matrix/jitter constants. The old kernel reproduced all captured
motion components within 0.00003815 pixel in both frames of capture 23.

See `docs/android/FSR3-SURFACE-MOTION-RECOVERY.md` for measured results, corrected
face ROIs and the limits of the image-derived motion reference.
