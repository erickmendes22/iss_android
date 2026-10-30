# Capture 20 and native camera ownership across rendered frames

Base: `d73ff481252f031e7e9ef2648414a95fbdc0e8cc`, CUSA00004 01.00.

## Observed capture

Both input buffers exactly equal the exported original pre-FXAA buffers.
Dispatches 749/750 have reset=0 and SDK history indices 19/20. The previous
history of f001 exactly equals the current pre-RCAS history of f000. Resolution
is 640x360 -> 1280x720. No source switching or history restart is present in
this pair.

Full-output phase correlation measured (0.01536, -0.00032) output pixels, with
response 0.9902. Static region displacements were below 0.03 pixels; Delsin's
region measured about 0.058 pixels horizontally. Both motion buffers are fully
finite, with component maxima 0.1160 and 0.1927 render pixels. This supports
stability for this pair, not for the whole session or a moving-camera sequence.
The log still contains rejected native camera preparations (often 4/180).
The old summary does not identify their reasons; attributing them all to one
cause would be unsupported.

## Reproduced production faults

The AA marker calls `PrepareIssNativeCamera`, then `RunScaled` calls it again.
The old method returned early only when the publication sequence was identical.
If the guest published another update between those calls, the second call
replaced the current camera and previous-camera anchor within the SAME GPU
frame. Camera reprojection and metadata could therefore refer to a later update
than the first preparation.

In a different rendered frame with no new publication, the same early return
replayed the previous frame's camera transform, instead of using identity for
an unchanged camera. It did not set `frame_has_camera`, so display cleanup then
invalidated an otherwise validated anchor. The next fresh sample had to seed
history again.

The regression executes the actual production preparation method, invalidation
method and display cleanup against a controlled camera publisher. On the base,
both the repeated-publication case and the mid-frame-update case fail. Matrix,
jitter, object-switch, sequence-gap and menu invalidation cases remain guarded.
These deterministic faults are distinct from claiming they caused every logged
rejection or the user's entire visual symptom.

## Correction

Latch one native sample/pair per rendered GPU frame. Repeated calls in that
frame do not reread the asynchronous publisher. Clear the latch at the real
display boundary. Across rendered frames, reuse an unchanged, already validated
sample with identity camera reprojection and zero guest-jitter delta. Object
motion and host viewport jitter still run normally. Never resurrect an
invalidated pair from an unchanged old publication after a menu/settings reset.

Keep all existing finite/scalar/inverse/jitter checks, camera identity and
sequence-gap rules, source selection and the SDK reset on invalid cameras.
The existing once-per-180-frame diagnostic now splits rejection reasons and
counts unchanged-camera frames; it adds no per-frame logging, readbacks or
buffer exports. This lets later device evidence distinguish remaining causes.

No SDK, resolution, Readback, reactive-mask, sharpening, object-motion outlier
threshold, package, version or signing change is included.
