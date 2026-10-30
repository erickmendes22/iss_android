# Capture 21: isolate missing facial motion before changing the renderer

Base `0ffa007bc46dbc413bdee575fc6dd0c94dcabe4f`, CUSA00004 01.00.
Input 640x360, output 1280x720. Both frames use the original pre-FXAA image.
Dispatches 899/900 have reset=0 and SDK history indices 40/41.

## Observed face region

The character's head is near input coordinates x313..347, y157..205. A face
ROI at x321..340, y174..199 has motion magnitude above 0.05 render pixels on
33.9% and 31.2% of pixels in the two frames (this rectangular ROI also includes
background). The spatial motion maps show vectors on the cap and coat, with a
large zero-vector area over the skin. The saved scene depth covers the head
continuously. Maximum head-region components remain small, about 0.114/0.097
pixels horizontally in magnitude.

The soft face is already present in the SDK pre-RCAS history; increasing final
sharpening would not identify the missing temporal information. Reactive and
disocclusion channels are zero over most of this ROI, while history weight is
near maximum. These facts warrant isolating the object's motion source. They
do not prove which vertex shader, draw identity or depth comparison failed.

The aggregate native camera rejections are now classified: the sampled log
reports camera switches, not invalid matrices or jitter. There is no camera
history reset in this exported pair. This is separate from the facial coverage
question.

## Missing observation and bounded capture change

The exported RG16F `motion` is the final composed vector AFTER object validity,
depth matching and outlier rejection. A zero value alone cannot distinguish:

* no instrumented object draw;
* a draw with no matching vertex history;
* valid object vectors rejected against the saved scene depth;
* genuine zero object movement.

Export `object_motion` from the existing RGBA32F attachment only when the user
requests the existing two-frame capture. RG contains raw previous-current
screen movement, B history validity, A written fragment depth. The same frame's
scene depth and final motion are already exported. An unwritten pixel has zero
validity/depth; a written but unmatched pixel has nonzero depth and zero
validity. Valid, depth-mismatched pixels reveal composition rejection directly.

RecordMotion has already transitioned this existing attachment to General.
Copy it without changing layout or contents. No new per-frame image, motion
heuristic, renderer setting, numerical threshold, SDK history rule or ordinary
readback is added. Capturing adds two 3.69 MB raw files at 640x360. Images beyond
the exporter's existing 20 MB per-file bound are explicitly unavailable.

AA metadata declares whether the raw object image is expected. Android waits
for the complete RGBA32F file for each advertised frame, rejects partial or
incorrectly encoded object images, cleans them with the other capture files,
and includes them in the ZIP. Older captures remain readable. The Java test
performs an actual round-trip ZIP and checks these conditions.

This build extends diagnosis; it is not a claim that facial reconstruction has
already been corrected. A new capture with the face visible provides the
previously missing validity/depth evidence needed for a targeted correction.
