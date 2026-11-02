# Remaining scene instability: capture #4 and presentation sequence

The device ZIP from build 67 has SDK dispatches 869/870, reset=0, phase count
32. The second frame's previous SDK color history exactly equals the first
frame's before-RCAS history (all half-float values identical). Camera ownership
continues matching the rendered scene, without new camera rejections.

Subpixel registration of three independent static regions gives output shifts
of (0.0030, 0.0003), (0.0035, 0.0011), and (0.0028, -0.0011) output pixels.
These two frames do not reproduce the reported intermittent whole-scene tremor.
A local replay with the production SDK gives RGB MAE 0.05964/255 against the
captured device output. This is a short seeded replay, not a reconstruction of
the complete preceding session. It does not prove that every frame is stable.

The remaining video variation cannot be diagnosed from the H.264 recording:
the largest measured detail drops in the latest video coincide with encoder
I-frames. This is a limitation of that measurement, not evidence that the user's
remaining symptom is exclusively a recording artifact.

## Requested diagnostic sequence

The existing capture action still records the two full SDK frames, then also
records a bounded sequence of 120 central 640x360 pixel crops of:

- `trace_output`: SDK output (after any existing sharpening), before native UI
  and the guest's final display composition;
- `trace_present`: actual completed host display image consumed by the presenter.

The crops are direct image-to-buffer copies, with no scaling or video encoding.
At 1280x720, their origin is (320,180). Each `trace_info` gives the exact crop,
monotonic CPU time, SDK dispatch frame ID, reset, jitter and frame duration.
These are command-recording timestamps, not GPU presentation timestamps.

The capture token travels with the display image under the existing display
mutex. Presentation never labels an old image with the global latest frame.
Repeated presentation of one token is captured once. Missing `trace_present`
indices identify skipped presentation, rather than shifting every subsequent
pair. Tokens remain unique across new capture requests.

The sequence is dormant outside a capture request. At 1280x720, each active
scene requests one 921,600-byte output copy plus at most one presentation copy.
The maximum additional raw file data is 221,184,000 bytes; ZIP size depends on
the scene. It creates staging buffers, not additional GPU render targets.
Readback and file writes can affect timings, particularly during the initial
full SDK pair; timestamps and frame durations retain that limitation.

Wait at least 10 seconds after requesting the sequence before exiting/exporting.
The exporter requires all 120 output crops and metadata files and at least one
presentation crop. It allows skipped presentation frames and preserves support
for legacy two-frame captures. Cleanup includes only recognized capture files.

This change adds diagnosis; it does not claim a new rendering fix or alter FSR
motion, history, jitter, sharpening, shader selection or camera ownership.

Validation: compiled C++ sequence/crop policy test, Java exporter round-trip and
partial-sequence checks, existing runtime checks, and Android native CI build.
Device validation must still establish whether the residual event first appears
in the SDK output or in the final display image.
