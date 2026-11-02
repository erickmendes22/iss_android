# Roll back build 73 protected-readback experiments after reported FPS loss

The user tested build 73 and reported lower FPS. The package had enabled four
performance experiments together; passing host correctness tests did not establish
a speedup on the phone, and there is no captured phone trace isolating one change.

Restore the GPU source and production runtime fixtures exactly to build 71
(`abb10993a7416edb020757b3b31a56ef77889a59`). Remove the private queue, requested-page
producer selection, page-scoped Hybrid staging and small-buffer arena promotion.
Restore the scheduler's original wait-stage policy too: build 73 widened every
wait to ALL_COMMANDS, including waits unrelated to readback. The private queue's
WAR guard also made every following graphics submission depend on its last copy.
These are concrete additional synchronization costs, not a proven attribution of
the phone's FPS loss. No speculative replacement is enabled in the rollback.

The original protected Hybrid mode behind “Precise 2”, camera/world-origin/jitter
fixes and disabled FSR capture/heavy profiling are preserved. The Bloodframe
bootstrap below is build preparation only; its library is identical to build 71.

Regression validation: require a zero diff against build 71 for all files under
`port/` and `tools/android/runtime/`, except the independent Bloodframe recovery
tests. The restored submission/Hybrid/recovery tests pass (15 cases), as do the restored
production mapped/staged lifecycle tests (3 cases). Reuse the already compiled,
signed and verified build 71 APK for immediate rollback instead of recompiling an
identical renderer. On-device FPS recovery still needs user validation.

Rollback APK: https://github.com/erickmendes22/iss_android/actions/runs/41363054526/artifacts/13106552333

This restores the pre-regression baseline; it is not a new performance improvement.

## Build dependency recovery

Build 72 passed runtime tests, including the new pinned-header producer/queue tests,
but failed before renderer compilation: Bloodframe's pinned DXBC submodule repository
`PancakeTAS/dxbc` is no longer public. No DXBC version is substituted.

The performance workflow explicitly opts into `ISS_BLOODFRAME_BOOTSTRAP=1` to recover
only the unchanged AArch64 `libbloodframe.so` and its five license/provenance files
from our successful build 71 APK (run 41363054526, artifact 13106552333). The manifest
pins the Actions ZIP, APK, runtime and all six extracted file hashes. It also pins
the Bloodframe commit, C API header and existing host-sync patch; changing those
inputs rejects reuse. The renderer, CPU runtime, driver and launcher build from
the new source as usual. Archive corruption and changed inputs fail before writes;
GitHub authorization is removed on the signed-blob redirect.

This is a temporary dependency bootstrap, not a new Bloodframe source distribution.
The pinned Actions artifact expires 2027-01-30; after expiration CI fails clearly
unless the exact source or another verified dependency cache has been restored.
Ordinary builds without the opt-in still compile Bloodframe from source. A local
verified download can be supplied to `stage_bloodframe_bootstrap.py --archive ZIP
--commit c29027555cfa8c9fc73c20f03816230becb9f469 --output out/bionic`.
