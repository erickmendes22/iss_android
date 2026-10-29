# inFAMOUS FSR3 capture (16): intermittent motion-vector jitter fix

Frame captures: 640x360 -> 1280x720; SDK jitter delta (f001-f000) approximately (0.5,0.333333) render pixels. In this pair global input phase correlation is approximately (0.52,0.24) pixels, while output is nearly stationary (-0.013,0.023 output pixels). A global jitter-sign flip would be a regression.

In f000, 2046 / 230400 pixels have object motion exceeding 20 pixels in at least one axis, with magnitudes up to 126 pixels. They trace static ceiling rails/building geometry, not actual camera translation; f001 has zero pixels exceeding 20 pixels. Camera-motion shader previously accepted any object vector within 25% of screen dimensions versus native camera (160x90 for performance 640x360). Camera-history fallback mode 2 had no displacement bound at all. Both paths could intermittently contaminate the FSR3 history.

Bound object vectors by max(4 pixels, 1.25% of the render dimension) per axis, **relative to the camera motion** in native mode. Reject invalid object motion and fall back to native camera motion; in object-only fallback use zero for those invalid pixels. At 640x360 this is 8x4.5 pixels, rejecting exactly the 2046 observed outliers in f000 and none in f001. Genuine high-speed object motion beyond this bound needs later validation; this is an experimental stability fix, not a general solution to fast animation.

Correct the fallback's depth comparison to a tolerance relative to actual reversed-Z depth, matching the native path. Disable the previous input-detail reactive-map experiment by default; it remains available only when ISS_FSR3_DETAIL_REACTIVE=1 is explicitly set. No changes to jitter, camera matrix, FSR3 history allocation, RCAS or render resolution.

The test must evaluate shaking across more than two frames and while moving the camera; two stable global frames alone cannot prove all temporal jitter is fixed.
