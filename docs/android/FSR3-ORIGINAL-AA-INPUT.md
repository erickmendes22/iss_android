# Original AA input for Android FSR3

Capture `(13)` contains same-frame pre-FXAA and post-FXAA RGBA8 UNORM scenes,
both 640x360. Their encodings/extents match, with approximately 95.2% of RGB
components unchanged. At sigma 0.7 pixels, the standard deviation of high-pass
luminance falls from 3.936 to 2.723 and from 3.929 to 2.720 (~30.8%). This metric
includes aliasing energy and is not proof of perceptual quality or a sole cause
of blur. Exact hashes/method/results are in FSR3-FXAA-CAPTURE13.json.

The SDK can reconstruct from that verified original scene without removing the
game's AA/composite pass or changing its output/HUD target. On Android FSR3 scaled
sessions, the renderer now uses the current AA marker's original input only if
it is sampled RGBA8 UNORM with exactly the composite's dimensions. Missing
markers, HDR, different formats/extents, non-sampled images and other upscalers
retain the previous source. No guessed/G-buffer target is used. The original
pass still executes. The selected input receives its tracked General/compute
read transition and its own Vulkan usage flags are passed to the SDK.

History resets when the source policy changes. Motion vectors, jitter, camera
matrices, depth snapshots, reconstruction settings, resolution and presentation
redirects are untouched. Internal comparison override: ISS_FSR3_PRE_FXAA=0.
No new launcher setting is added.

The bounded capture exports `aa_before_fxaa`, `aa_after_fxaa`, the SDK's actual
`input`, `output`, motion and depth, plus selected-source metadata. A new capture
can therefore verify that the SDK input is the original scene, while still
retaining the game's filtered result for comparison. Device quality validation
remains pending; no measured FPS or final blur-resolution claim is made.
