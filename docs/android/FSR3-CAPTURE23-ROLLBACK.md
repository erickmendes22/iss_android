# Capture 23: withdraw the fragment-depth experiment

User report: the 6115fb5f build made the image worse. Restore the complete
runtime and launcher source tree of 4deb0944, before the fragment-depth change.
Do not bundle another unverified numerical fix with this rollback.

## Evidence from the supplied capture

The log identifies 6115fb5fd25c41e11ff8d3247d50bcc05a8a7957, CUSA00004
01.00, 640x360 to 1280x720. Both inputs are verified pre-FXAA originals.
The same 16 resolution/blur/shadow patch writes are active as in capture 22.

Both 640x360 RGBA32F raw object-motion images are finite. Neither contains
a negative B value: none of the recorded valid motion pixels identifies a
shader-exported depth source. Consequently this capture does not support
the preceding hypothesis that a face depth export was the missing fix.
It also cannot prove that the change caused the reported regression elsewhere
in the session. Its removal is a conservative rollback, not a confirmed fix.

There are 208761/210226 history-valid object pixels in frames 0/1;
206728/208180 also pass the unchanged scene-depth gate. The initial rectangular
ROI x326..350, y164..198 was incorrectly reused from capture 22. In capture 23
it includes substantial background, because the head has moved left. Its
92.52%/96.94% matching percentages do NOT describe the face. The corrected face
ROI x305..324, y168..191 contains 437 pixels, all history-valid in both frames.
Only 62/437 and 56/437 pass the original depth gate. This invalidates the earlier
inference that facial depth rejection had improved.

The SDK reports dispatches 839/840, reset=0, history indices 1/2. Thus the
requested pair itself does not reset, but begins just after a history restart.
The log repeatedly counts 8-10 camera-object switches per 180 rendered frames.
The current policy resets FSR when the native camera pair is rejected, even
if the object-only fallback reconstructs a frame. These are measured history
interruptions; whether they represent real cuts or a mismatched CPU camera
publication remains unresolved. Never ignore a camera cut simply to retain
history.

The pre-FSR image already contains severe low-resolution edge sampling and
the reported scene coloration. FSR softens that source. This does not establish
the origin of the coloration, or mean that temporal reconstruction cannot add
artifacts. Distinguish scene rendering, source selection, and FSR history.

## Rollback validation

All files outside this report must match 4deb0944 byte for byte. In particular,
restore raster-depth motion tags, positive validity tests, capture metadata,
and the original shader-cache key. Preserve all earlier depth/outlier guards,
unique object-history checks, AA-boundary source selection and camera latching.
No readback mode, resolution, driver, reactivity or sharpening change.

The rollback APK retains the established package, version and signing key.
This withdraws the last experiment; it does not claim to solve the facial blur.
