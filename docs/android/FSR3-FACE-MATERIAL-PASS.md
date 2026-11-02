# Facial capture: distinguish actual exports from inherited MRT bindings

Capture: `inFAMOUS-FSR3-Input-Motion-Output (1)(1).zip`, SHA-256
`028be4a7a09047f6f37448712e0e17fced2187016e415e9a5c3fe9e056fe665a`.
The supplied pair contains the front-facing head, unlike the preceding ZIP.
Runtime: 973ed91599b00b71c3156e2dea63fc88cd5ccd34; 640x360 to 1280x720;
pre-FXAA RGBA8 input, reset=0, SDK history indices 15/16.

## Observations and exclusions

Input ROI x309..330/y156..178 includes face and a small amount of its border.
History-valid object tags cover 93.28%/92.29% of this rectangle. Median relative
tag-depth error among valid tags is 3.49%/3.30%; the 90th percentile is
6.27%/6.29%. The earlier back-facing head's depth agreement cannot be reused
as a claim about the face. The strict 0.5% depth gate must remain intact.

A pinned-SDK replay reproduces the captured processing. For a counterfactual,
replace composed motion only within the head rectangle with its raw valid
object vectors. The resulting f001 facial output differs from the unmodified
replay by only 0.05345/255 mean RGB in output x612..661/y306..363. Thus merely
accepting those rejected vectors does not demonstrate a cure for this face.
Resetting history visibly loses temporal reconstruction and changes this ROI
by 4.0146/255. A view-space unit-scale experiment changes it by 0.08345/255;
neither experiment is a production modification.

Registration is consistent with the supplied jitter. A separate controlled
64-frame reconstruction, generated from a known fixed reference with known
Halton shifts, reduces facial RGB error from 3.1067/255 at the first frame to
1.8015/255 at frame 64. This checks the replay pipeline, not the quality of
the actual game's missing intermediate frames. Do not treat optical-flow
registration on a heavily undersampled face as exact ground-truth animation.

## Concrete selection defect

`RefreshGraphicsKey` decided that a draw was a G-buffer candidate by counting
enabled **bound** color targets before compiling its ordinary shader. PS4
bindings can remain populated from an earlier draw. Five bound targets do not
establish that the current pixel shader writes five targets. A subsequent
auxiliary shader with one export could therefore acquire the motion output
and overwrite motion from a real material pass. The early depth-boundary seal
cannot prevent an unrelated write that happens before that boundary.

After `RefreshGraphicsStages` supplies the ordinary fragment shader's real
`mrt_mask`, intersect that mask with the enabled bound targets. Require five
actual guest exports, excluding motion slot 7, before selecting the motion
variant. Apply this restriction only to ISS Android Bionic. Do not change any
guest draw, depth state, attachment, shader color export, camera policy,
Readback mode, SDK parameter, sharpening or resolution. The normal material
path and its existing skeleton/history checks remain in place. No new images,
per-frame logging, readbacks or allocations are introduced.

The regression compiles the production eligibility predicate, exercises
inherited bindings, zero/one/four exports, disabled targets, the reserved
motion output, and valid five/seven-export material passes. Restoring the
previous bound-only predicate fails the inherited-target case. An integration
check ensures selection reads the ordinary shader mask before motion is added.

This closes a demonstrated pass-selection hole. This ZIP does not contain a
per-pixel producing-shader ID, so it does **not** prove that a particular skin,
back-face or SSS program caused the observed face. A compiled APK passing the
regression still requires visual device confirmation; do not label the facial
artifact solved solely on the basis of this host test.
