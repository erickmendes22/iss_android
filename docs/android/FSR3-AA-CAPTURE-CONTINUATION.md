# FSR3 blur investigation: preserve the pre-FXAA comparison

The user's capture `(12)` contains two input/motion/depth/output frames, but no
pre-FXAA frame. Jittered input moving while a stationary-camera temporal output
stabilizes does not establish a motion-vector defect. No motion/jitter/matrix,
FSR input selection or reconstruction parameter is changed here.

The prior c2316d8 change added the verified AA input address to the PM4 marker,
then requested an `aa_before_fxaa` GPU dump. Three integration gaps remained:

* Both Android export filters and capture cleanup excluded that filename.
* The dump assumed layout General without transitioning the original input image.
* Only RGBA8 inputs were accepted, so another original pixel encoding could
  silently omit the comparison. The export's four-file count could also accept
  incomplete/mixed frames.

The Android UI now shares one filename/size contract for readiness, cleanup and
export. It includes pre-FXAA images and per-frame AA metadata, rejects truncated
or unrelated files, and requires both frames' four original buffers. When a
frame reports that a pre-FXAA image exists, readiness also waits for that image's
complete GPU readback. The bounded user-triggered capture remains two frames.

Pre-FXAA capture preserves supported original encodings (RGBA/BGRA8, RGBA16F,
RGBA32F, B10G11R11 UFLOAT, A2B10G10R10). Metadata records the Vulkan format,
original extent, cropped extent, GPU address, jitter and availability. No HDR
image is converted/reinterpreted as RGBA8. A tracked General/transfer-read
transition is recorded only for a requested capture. Unsupported/missing AA
inputs are explicitly described instead of presented as a successful comparison.

The production Java selector is exercised with two real frame sets and an actual
ZIP round trip, including pre-FXAA RGBA8/16F, metadata, incomplete GPU writes,
invalid dimensions and unrelated files. Existing marker tests now use the new
four-argument marker API; the previous build failed because one assertion still
required the old three-argument API.

The final diagnostic APK must retain package `com.daedalus.iss.bionic`, version
0.2 and the original `aff266dd...6475b` signer. It enables a missing comparison;
it is not evidence that FXAA caused the blur or that the blur is fixed. A fresh
on-device capture is required to compare original AA input -> post-AA/FSR input
-> FSR output, accounting for encoding differences if the original input is HDR.
