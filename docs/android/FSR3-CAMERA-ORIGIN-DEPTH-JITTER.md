# Continue the camera-origin and depth-prepass correction

Baseline: the exact merge `d1199f5c07b3d7dd39eb2dc13f8c1593d889271b`
recorded in `inFAMOUS-FSR3-Input-Motion-Output (2)(1).zip`.

The previous conversation ended after a camera-translation correction and a
depth/color jitter correction, while reviewing backface culling. Its uncommitted
implementation was not present in the available checkouts. This change
reconstructs those two fixes from the supplied continuation, on the captured
build, rather than reverting to earlier AA, sharpening or motion-depth recovery
experiments. The earlier reported 64-frame SDK and Vulkan 32-phase measurements
are conversation evidence, not newly executed measurements of this patch.

## Camera-relative matrices

The verified +0x430/+0x4b0 pair is mutually inverse but camera-relative.
The +0x200 matrix carries translation too. Multiplying +0x200 by +0x4b0
recovers a translation with an identity basis; validate this property before
accepting the sample. Save its translated row (-C) beside the previous composite.
Reprojection must include `T(C_current-C_previous)` between the current relative
inverse and the previous relative composite. Compute the origin and its delta
in double precision before uploading the existing float push constants.

Keep submission-owned snapshots, frame latching, camera-object and sequence-gap
rejection, guest jitter accounting and the existing per-pixel motion guards.
No additional capture, GPU resource or readback is introduced.

The production-method fixture now supplies the translated matrix as well as the
relative pair. Its world-translation case leaves the relative pair fixed while
moving the camera by three units at a depth of 1000 units: expected horizontal
motion is 0.988463 input pixels at width 640. It also checks stationary following
frames and nonfinite translated matrices.

## One jitter policy from the first depth draw

Discover scene-depth identity using guest address, dimensions and format.
Latch the preceding scene's identity at the frame boundary. Learning a new
identity during G-buffer rendering only makes it eligible next frame; early
depth-only and later material draws therefore both use zero on discovery.
For an eligible resource, direct and indirect depth-tested geometry receive
the same Halton shift, without waiting for this frame's G-buffer detection.
Clip-disabled and non-depth-tested full-screen draws remain unshifted.

RunScaled computes the effective displacement using the same policy, passes it
to camera reconstruction, FSR dispatch and the existing diagnostic metadata.
Settings changes reset the identity anchor. No fallback enables jitter midway
through a frame. Other platforms retain their existing rasterizer policy.

## Backface culling review

The viewport change adds a common x/y translation. It does not change viewport
width/height, the sign of triangle area, guest cull mode or guest front-face
selection. Those states still pass through UpdatePrimitiveState unchanged.
There is no basis for disabling culling as part of these two fixes.

Device validation remains necessary: these corrections do not establish that
every reported interval of shimmer or blur has been eliminated.
