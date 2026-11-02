# Gameplay instrumentation cleanup

The device report after the camera/jitter fixes is that the whole-scene tremor appears gone. Preserve scene-owned submission cameras, world-origin translation, effective depth/color jitter, the FSR input, resolution and sharpening. This change starts performance work by removing diagnostic work from normal gameplay.

- Remove the in-game capture action and launcher ZIP export. Do not arm/create the private capture directory at launch. Existing saved captures and game saves are untouched.
- The performance wrapper forces `ISS_FSR3_CAPTURE=0` and `ISS_FSR3_DIAGNOSTICS=0`, and clears both SDK/display capture trigger environment variables. Native capture also defaults off, so stale trigger files cannot initiate readback. Developer capture implementation remains available outside the performance wrapper.
- Default `ISS_COMPARE_STATS` to zero. Readback and fault profiling inherit this default; explicit profiling overrides remain supported. Keep startup/error reporting and ordinary log export.
- Skip diagnostic MRT layout census, matrix/binding samples and periodic FSR counters/reports. Keep frame cleanup, depth snapshot and scene camera selection independent of diagnostics.
- Share the existing material-binding predicate with the camera observer. Reject other shaders, slots and sizes before mapping validation / sparse memory copies on the direct draw path. The selector retains the same accepted layouts, validation, ambiguity handling and history rules.

Validation: the local runtime suite passed 190 tests (10 unavailable-dependency skips), followed by passing targeted policy/capture tests after the final edits. Production camera preparation/selection executes for repeated frames, pauses, queued submissions, world translation and scene ownership with diagnostics both off and on. The production dump-trigger function is tested with an existing trigger: unset/zero capture flags leave it unconsumed, while explicit developer capture remains bounded. Android compilation and APK verification are performed by CI.

This establishes eliminated CPU/diagnostic work, not a measured phone FPS gain. Compare sustained FPS/frame pacing on the same device, scene, resolution and power settings before attributing a performance change.
