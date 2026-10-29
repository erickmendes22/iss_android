# Capture 14: source verified; internal history inspection

The effective SDK color input equals `aa_before_fxaa` byte for byte in both frames.
640x360 RGBA8 is reconstructed to 1280x720. Both depth images are finite, nonzero,
and contain real geometry; motion is finite. This establishes successful source
selection. It does not establish that all remaining softness is a defect.

## Offline experiments

Built the exact pinned SDK `c64f093404125e960813f95f1996c14780f0dd69`, applying
Android backend patches 0004, 0007 and 0008, on x86-64 software Vulkan
(llvmpipe LLVM 20.1.2). Replayed the raw captured color/depth/motion with the
reported jitter, near=10, far=10000000, vertical FOV=1, sharpening=0.8.

* A controlled 32-phase reconstruction of a known synthetic reference improved
  from 29.5452 dB PSNR at frame 0 to 31.6934 dB at frame 31. Inverting the supplied
  jitter instead produced 27.2723 dB at frame 31. This validates the convention
  in that controlled experiment, not the actual displacement of every guest pass.
* On 32 repeats of the same captured input and fixed jitter, automatic vs explicit
  unit exposure changed RGB by 0.08763/255 mean absolute error. Exposure was not
  the dominant source of softness in this controlled frozen-input experiment.
* Neither experiment includes the phone's earlier history or reactive mask.
  They therefore cannot reproduce or exonerate its complete temporal state.
* Adding read-only introspection AND copying/restoring all four internal image
  layouts preserved both replay output frames byte for byte:
  f000 SHA256 c2b908eb0eca8aad5f65cee0e6ba814c0ac2b8fdd0117f10374b14a478ad43ac
  f001 SHA256 ec66b6ae036b1866397b4e414ede61fbddefbc64a0455f3ead4e670ee3b5a846

## Targeted capture extension

Only on the existing user capture request, export these exact SDK resources:

* `fsr_history_previous`: the SDK's previous ping-pong color history, RGBA16F.
* `fsr_before_rcas`: current internal upscaled color consumed by RCAS, RGBA16F.
* `fsr_accumulation`: current accumulation resource, R8 UNORM.
* `fsr_reactivity`: final reactive/disocclusion/shading-change/accumulation channels,
  RGBA8 UNORM, as defined in the pinned SDK.
* `fsr_info`: effective SDK history frame index, reset request, phase count,
  current/previous jitter, normalized motion scale, timing and pre-exposure.

Resource handles, formats, extents and actual layouts come from the pinned SDK.
No guessed image addresses. Every diagnostic transfer restores the original
layout immediately; the backend's tracked state remains valid. Unsupported
images/layouts are omitted and explicitly reported via `sdk_images`.
No extra image copies, readbacks, history resets, motion changes or parameter
changes during ordinary play. The Java ZIP exporter waits for all advertised
SDK stages and rejects partial files or wrong SDK encodings.

Capture 14 lacks these resources and the history-reset state. A device capture
with these stages is needed to locate whether detail is rejected by the history
validation or lost between accumulation and RCAS. No speculative blur fix is
claimed by this diagnostic change.
