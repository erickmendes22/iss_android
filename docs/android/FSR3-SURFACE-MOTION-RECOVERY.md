# Captured-buffer validation of visible-surface motion recovery

**Superseded after capture 25:** this local capture-23 improvement did not resolve
the reported face problem. The neighbor recovery is withdrawn. See
`FSR3-CAPTURE25-PAIRED-STAGE.md` for the resource-lifetime correction and its limits.

Base: 2de9ae9c. Keep submission-owned camera snapshots. Do not reinstate the
withdrawn fragment-depth experiment. No Readback, resolution, reactivity,
sharpening, history reset, runtime logging or game-rendering change.

## Why the previous investigation missed the cause

Bookkeeping/emitter tests proved isolated code defects, not that fixing those
defects improved the reported face. The capture-23 face ROI was also incorrectly
reused from capture 22 despite the character shifting left. It included much
background, giving misleading high depth-match percentages. Corrected rectangles:

| Capture | Face ROI (input pixels) | f001 history-valid | f001 depth-matched |
| --- | --- | --- | --- |
| 22 | x326..350, y164..191 | 648/648 | 63/648 |
| 23 | x305..324, y168..191 | 437/437 | 56/437 |

In capture 23, (315,180) has scene depth 0.04481674, object tag depth 0.04093392,
and valid raw motion (1.1688954,-0.46800676). The depth mismatch correctly rejects
that layer, but the only fallback is stationary-camera motion (0,0), even though
nearby front-surface samples at the captured scene depth are moving. The source
of the layered depth discrepancy is not conclusively identified: the player
preserves depth at an earlier lighting boundary than the final object attachment.
Do not pretend that the raw farther layer belongs to the visible face.

## Reproduce the actual computation before changing it

`tools/android/fsr3_capture_replay` prepares a separate copy of the exact pinned
SDK with the four Android patches. It seeds f000's captured current color,
accumulation and reactivity, then replays f001 with recorded inputs and state.
Earlier luma history is not completely captured; baseline error is measured.

On lavapipe (LLVM 20.1.2), RGB output mean absolute errors against the phone are
0.025795/255 for capture 22 and 0.011846/255 for capture 23. The original production
motion shader, run as actual SPIR-V, reproduces captured motion within 0.00003815
pixel in both frames of capture 23 using its stationary native camera.

## Correction and controls

Preserve direct depth-matched vectors exactly. For a history-valid, finite,
bounded raw vector rejected **only by depth**, recover an estimate from nearby
samples instead of making the animated surface stationary. Every source must:

* have valid history and pass its own original 0.5% depth gate;
* match the target's scene depth within the same 0.5% tolerance;
* pass the original camera-relative vector-outlier bound;
* move coherently with the target candidate (direction, speed ratio, <1 pixel difference).

Require at least three sources within six input pixels, with weighted motion
variance <=0.09. Keep camera motion for missing/ambiguous history, raw outliers,
nonfinite values, insufficient support, incompatible depth/direction, and
subpixel/static differences <=0.15 pixel. This is a bounded spatial estimate,
not an exact replacement for unavailable per-pixel geometry identity.

FSR already dilates the nearest-depth motion from a 3x3 region, but has no input
flag identifying which zero vectors came from discarded object tags. Therefore
its ordinary dilation cannot replace this validity-aware recovery. The player
uses recovered motion only in native camera mode 3; fallback/history-reset paths
and other games' modes are unchanged. No extra images, copies or CPU readbacks.

## Measurements, not a claim of device confirmation

The actual new SPIR-V changes 599/619 viewport pixels in f000/f001 of capture 23
(0.26%/0.27% of 640x360). In f001 it recovers 307 of the face ROI's 437 pixels.
GPU output agrees with the CPU experiment within one half-float ULP; maximum
difference is 0.00097656 pixel in f000 and 0.00048828 in f001. Both frames of
capture 22 produce bit-identical old/new GPU motion images.

For the recovered face pixels, mean motion disagreement against an independent
DIS image-flow reference falls from 0.8427 to about 0.1374 pixel. This reference
includes captured jitter removal; it is not ground-truth geometry. Replaying
the new motion through FSR reduces the ROI's flow-aligned temporal RGB MAE from
2.3704 to 1.7646/255 (about 26%). Current-input sample MAE also falls
from 5.9117 to 5.0203/255. These measure this pair, not whole-session
smoothness, perceptual quality, or performance on the phone.

Negative control: accepting all bounded raw vectors despite mismatched depths
increases temporal MAE to 3.3899/255. The original depth gate must remain strict.
Lowering the recovery floor from 0.15 to 0.015 pixel also worsened capture 22:
temporal MAE rose from 1.4026 to 1.5575/255. Keep the conservative floor rather
than propagating weak subpixel motion into a stable reconstruction.

The GPU regression executes the production kernel on positive recovery plus
missing target/source history, foreign depth, target/source outliers, conflicting
direction, excessive variance, static motion, valid direct motion, nonfinite
values and a viewport corner. No captured game images are committed.

Reproduction, with NumPy/OpenCV for the comparison:

```sh
python tools/android/fsr3_capture_replay/compare.py CAPTURE23 BASELINE_OUTPUT NEW_OUTPUT \
  --motion NEW_GPU_MOTION --roi 305,168,19,23
```

Hardware confirmation of the remaining facial shimmer/blur and runtime cost is
still required. Do not report this as proof that every scene is fixed.
