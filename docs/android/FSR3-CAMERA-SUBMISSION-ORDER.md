# Associate ISS camera snapshots with graphics submissions

## Reproduced defect

The guest camera hook publishes CPU-side updates to a single mutex-protected
latest snapshot. CameraMotion previously read it when the graphics processor
reached the AA/FSR boundary. GPU commands may be queued while the CPU continues
updating cameras. A complete, valid snapshot can therefore belong to a later
submission. Matrix validation and one read per rendered frame do not detect
this scheduling mismatch.

The regression executes the actual previous PrepareIssNativeCamera method from
4deb0944: submit cameras 100/101, then advance the CPU publisher to camera 160
at another guest object before processing the first frame. The previous method
incorrectly anchors sequence 160. This is a code-level scheduling reproduction,
not a replay proving the exact camera selected in capture 23.

Capture 23 contains frequent camera-switch rejections and SDK history indices
1/2 despite reset=0 within the captured pair. That motivates checking ownership,
but does not establish that every logged switch is spurious.

## Change

SubmitGfx copies the completed camera before draw preparation. The existing
ProcessGraphics coroutine owns this copy by value while suspended. When it
executes, it selects the snapshot through Rasterizer::RunInOrder, which already
copies ordered-task bytes and preserves stage-A/stage-B command order. Direct
recording uses the existing drained fallback. No new queue or global mutable
GPU camera is introduced. Nested indirect buffers inherit the submission's
selection instead of reading the live publisher.

CameraMotion consumes only that selected snapshot, including the existing
bounded summary diagnostics. Missing snapshots remain missing, with no fallback
to a future CPU update. The AA/dispatch pair is still latched once per rendered
frame. Multiple submissions do not replace an already latched frame pair.

Existing camera identity, inverse/scalar/jitter validation, sequence-gap resets,
object-history uniqueness, depth matching and vector-outlier rejection remain.
No Readback, FSR preset, sharpening, reactivity or renderer shader change.
The preceding fragment-depth experiment stays withdrawn.

One approximately 296-byte value is retained per queued graphics command
buffer and transferred once through the existing ordered-task mechanism. There
is no per-draw camera copy, GPU image readback, guest-pointer retention, or new
normal-gameplay log stream. The original Android package/signing/version remain.

## Verification and limits

The updated camera regression passes the delayed-publication case, repeated
samples, AA/dispatch double preparation, pause/resume, missing/invalid matrices,
real camera switches and sequence gaps. The old production preparation fails
the queued-camera assertion. Another executable test uses the production
RunInOrder and submission setter: queued task payloads retain original bytes;
two submissions and nested draws observe 100/101/101; switching to direct mode
drains older packets before selecting the next camera. Disabled queued tasks
and empty snapshots follow the same ordered fallback.

This fixes selection from a future CPU publication. It does not establish that
the last camera updated before submission always owns the main scene when
several guest cameras are updated. It deliberately preserves rejection of real
cuts; it does not remove camera identity checks to hide resets. Hardware visual
confirmation is required before claiming facial shimmer/blur is resolved.
