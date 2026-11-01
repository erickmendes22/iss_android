# Capture (5): continue beyond the guest display image

The user confirmed that the remaining tremor was visible immediately after
requesting the sequence. Do not treat small registration values as proof that
the reported event was absent. The defect is perceived across the game, with
more visible loss of facial detail; it is not an isolated shaking face.

## What the existing data establishes

Build 68 / merge 25267151d49a722f4757873af9b14b98d0b167a0 produced
120 output/present pairs over 2.583643333 seconds (dispatches 1349–1468), with
no SDK resets. `trace_present` RGB bytes differ negligibly from `trace_output`.
Static-region registration spans about 0.03–0.045 output pixels and repeats
with the 32-phase jitter cycle. Most static pixels are unchanged; changes
are predominantly one 8-bit code value. This does not establish how large the
user perceives the instability after Android presentation.

`trace_present` is the completed guest display image, BEFORE
`PostProcessingPass::Render`, the copy into the AHardwareBuffer, and Android's
SurfaceControl transaction. Its previous label must not be interpreted as a
capture of the physical screen. Reviewing the code found no application of
FSR jitter to the later viewport/crop. That source review does not establish
that the device's complete presentation path is visually stable.

Controlled static SDK tests, using a frozen reference derived from the captured
output and synthetic known jitter samples, are counterfactual tests rather than
replays of the missing original input sequence. FP32 did not reduce temporal
noise relative to FP16. The face-reference MAE was 1.446 vs 1.433 /255.
An sRGB-decoding/linear-output variant reduced temporal RMS modestly (face
0.390 vs 0.409 /255) but increased reference MAE (1.549 vs 1.433 /255).
Neither result demonstrates a rendering fix. The live color-space semantics
must not be inferred solely from a UNORM format or these synthetic tests.
No precision, color-space, jitter, sharpening or resolution change is enabled.

The second full SDK frame has no valid object-history tags, but its first two
output head registrations differ by only about (0.004,-0.011) pixels. A zero
object vector is not, by itself, evidence of a missing animation vector on that
frame. Do not bypass depth rejection, mark an unrelated history-policy issue
as the whole-scene cause, or claim the face alone explains the symptom.

## Additional requested-only evidence

The same 120-frame capture now transports its owned sample index and serial
from the completed guest display into `Frame`, then into the AHB submission.
It adds:

- `trace_ahb`: a direct central pixel crop after presenter post-processing and
  the final blit, before the host overlay and BEFORE releasing queue-family
  ownership to Android. No copy runs outside a requested capture.
- `presenter_info`: SDK serial, native frame counter, AHB generation/index,
  source/output dimensions, crop, submission time and explicit copy status.
- `android_info`: receipt and application of the tagged native frame,
  SurfaceControl source/destination geometry, window size, FIFO mode,
  callback completion and Android latch timestamp when available.

The protocol remains 40 bytes/version 3. Tagged FRAME messages use previously
unused format/count fields; HELLO/BUFFER/MODE/RELEASE semantics and fences do
not change. Untagged messages remain unchanged. Android callback contexts own
their frame identifiers, so reordered callbacks cannot relabel an older frame
as the newest one. Old-session completions do not write into the current trace.
No callback, geometry, queue mode, release policy or GPU wait is changed for
rendering. Optional latch-stat queries and file writes run only for tagged
capture transactions.

The AHB copy uses the TransferSrc support already recorded at buffer creation.
It inserts a requested-only TransferWrite→TransferRead barrier, copies the
crop, and restores TransferDst before the existing handoff. Unsupported formats
and staging-allocation failures are recorded explicitly. The ordinary path and
the source-frame branch of frame generation are covered; generated interpolation
frames are not newly captured.

The exporter retains old ZIP support. For a new trace it requires complete AHB
bytes when `ahb_status=copied`, matching SDK serials and a terminal Android event
for the same native frame counter. Skipped scene/presentation indices remain
permitted. Metadata and pixel reads can perturb timings; these are recording
and Android latch times, not a lossless readback of SurfaceFlinger's final
composition or the physical panel.

## Validation

The native callback fixture runs the production packet parser, trace writers,
show/complete callbacks and frame dispatcher. It tests untagged frames, the
0/119 bounds, invalid tags, reordered callbacks and obsolete sessions.
The readback fixture compiles the production CaptureAhb body against Vulkan-Hpp
and checks copy coordinates, both layout barriers, queue ownership, duplicate
suppression, unsupported transfer and allocation failure. The Java exporter
fixture rejects partial copies, missing completion, wrong Android frame IDs
and mismatched SDK serials. CI compiles the complete Android C/C++ integration.

This extends diagnosis; it is not a claimed correction of the remaining tremor.

## GPU-to-host readback visibility correction

The capture helpers previously waited for GPU completion and invalidated mapped
CPU memory, but omitted the TransferWrite-to-HostRead memory-domain dependency.
This does not establish a rendering defect; it weakens the guaranteed validity
of the diagnostic readbacks. Add a staging-buffer barrier after every image
copy and before its deferred host read, keeping the existing completion waits,
VMA invalidation and image-reuse barriers. No render-image policy is changed.
The same dependency is included in the offline replay transfer helper before
using it to compare candidate rendering changes.

Khronos specifies the required device-write / host-read barrier / completion
wait / cache-invalidation chain:
https://docs.vulkan.org/refpages/latest/refpages/source/vkInvalidateMappedMemoryRanges.html

The production barrier helper is compiled against Vulkan-Hpp. A mock dispatch
verifies transfer-write to host-read scopes, exact buffer/range, ignored queue
families and placement before deferred reads in BOTH capture helpers. This
corrects the diagnostic contract; it must not be presented as a device tremor fix.
