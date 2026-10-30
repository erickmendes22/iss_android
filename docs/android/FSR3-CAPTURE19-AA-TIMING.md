# Capture 19: AA input lost before the late FSR fallback

Base: `099d6d80deaaa21dc100fd650ff7610224b96c58`, confirmed in the supplied log.
Game CUSA00004 01.00, FSR3 640x360 -> 1280x720, same 16 patch writes.

## Recorded evidence

| Field | f000 | f001 |
| --- | --- | --- |
| Dispatch ID | 359 | 360 |
| Selected scene | pre-FXAA original | post-FXAA composite |
| Verified original available | yes, 0x10f8d08000, RGBA8 | no, address 0 |
| SDK reset | 0 | 1 |
| SDK history index | 5 | 0 |
| SDK accumulation channel | 99.57% nonzero | 100% zero |

The previous SDK history in f001 is byte-identical to the current history in
f000. Exporting history has not substituted a different frame. All motion
values are finite. The second input also contains mission text absent from the
first: the late fallback has crossed the intended scene/UI boundary.

Motion in this moving-camera pair is mostly zero (median and 90th percentile
zero in both frames). The largest per-component values are about 8 pixels.
Full-image phase correlation has low confidence because of motion, effects,
and the changed source; its displacement must not be treated as a camera ground
truth. This pair cannot validate the earlier static-scene jitter results.

## Production control-flow fault

`OnIssAaOutputMarker` checked `camera_motion.Ready()` before it saved the
verified AA input. It also did not refresh the completed native camera sample
until `RunScaled`. A false/stale Ready value discarded the entire marker. The
existing display-boundary fallback later refreshed the camera and dispatched
with `iss_aa_prefilter` still empty. Switching from the original scene to the
filtered composite deliberately resets SDK history. Removing that reset would
mix differently filtered and potentially HUD-bearing inputs.

## Change

Validate the marker target and save its current-frame AA input independently
of camera readiness. Then refresh the native camera before testing Ready. If
it is still invalid, defer to the existing fallback with the verified source
retained. Unknown addresses and inappropriate formats/extents remain rejected;
the normal frame boundary continues clearing all AA input state. No stale
camera matrices are accepted, and no reset safeguard is removed.

The native-camera/motion policy, shared-mesh identity safeguard, outlier guard,
jitter, resolution, readback, SDK/RCAS settings and package identity are unchanged.

## Validation and limits

The regression test compiles the actual production marker method against fake
camera/cache boundaries. It exercises delayed readiness, recovery on latch,
stale Ready becoming invalid, unknown source/target and mismatched dimensions.
The old early return fails the delayed-readiness and recovery cases.

This removes a demonstrated cause of source switching and history restarts.
It does not establish the reason every native camera sample is rejected, nor
prove moving-camera vectors or whole-session image stability correct. Device
validation and the next exported pair must confirm which route is used.
