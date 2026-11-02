# Select the ISS camera from its rendered material transforms

Base: build 66 / merge `f0e75a250414b5c76db5884e12d7595dba3437be`.
The previous translation and depth-prepass fixes remain intact.

## Report and evidence

The reported fault is global scene instability, with more visible loss of
facial detail. It is not an isolated facial animation defect. In the supplied
8.84-second video, stationary scenery has coordinated episodes at approximately
1.250, 2.769, 4.298, 5.813 and 7.367 seconds. The corresponding floor detail
energy falls by approximately 24–32%, then recovers. Android controls remain
stable. The episodes do not follow the recorder's I-frame schedule.

Capture (3) uses build 66. Its two ordinary FSR output frames are stable to
approximately 0.004 output pixel on three scenery crops; this does not cover
the intermittent video episodes or the final display. Its SDK replay differs
from the captured output by 0.0365/255 RGB MAE. Forcing a history reset changes
the whole reconstructed scene, not only the face. The log also records 77
camera-object-switch rejections among 3051 active camera preparations. The
video and log lack synchronized per-frame reset records: these observations
must not be presented as proof that every video episode is a specific reset.

## Reproduced integration defect

The guest hook observes a general camera-update function. Its publisher kept
one latest snapshot. Submission ownership fixed asynchronous lifetime, but
never established that this latest camera generated the main scene. A later
auxiliary update displaced the valid main camera; the next main-camera update
then displaced the auxiliary one. Both changes invalidated temporal history.

The executable regression runs the actual production camera preparation and
G-buffer observer for 180 frames. Inserting a valid auxiliary camera after the
main camera at frames 45, 90 and 135 makes the former latest-camera policy
produce six object-switch rejections. Matching the camera to the material
transform produces zero; the initial history seed remains required. A genuine
scene-camera cut still resets, and missing matches never reuse a CPU latest
snapshot.

## Bind ownership to geometry

The logged material-instance layout is 212 bytes: nine object-basis floats,
then a row-vector model-to-clip matrix. For uniformly scaled instances,
`clip[0..2] = scale * object_basis * camera_composite[0..2]`. Independent
objects in capture (3) recover the same camera basis and projection despite
different object rotations and scales. Two independent captured instance
records are regression fixtures; they are not synthesized from the matcher.

Keep the latest snapshot of each of at most 16 camera objects. Copy that set
into each graphics submission and the existing ordered recording queue. At a
known material binding on the scene G-buffer, solve the single positive scale
and check the full first three clip rows. Accept only a relative RMS residual
at most 0.0001, a valid object basis and valid native camera matrices/scalars.
The supported vertex layouts are the verified hashes 40b18d54, 6042bbde and
56673dd1 at slot 0, with whole 212-byte instances.

Reject missing, expired, nonfinite, singular, incompatible and ambiguous
candidates. Parallel cameras with different world origins remain ambiguous;
recency is not a tie-breaker for differing poses. Equivalent aliases prefer the
already established camera object. Do not widen the depth/motion gates, remove
camera-cut rejection, change sharpening, or invent an affine camera from one
object's model-to-clip matrix.

One matching camera is latched with the scene depth for the rendered frame.
Later submissions cannot overwrite it. A changed depth target before camera
preparation requires another match. Normal history construction still uses
the copied native camera, including its verified world origin and inverse.

Storage is bounded, with no per-draw allocation, new GPU image or GPU readback.
The existing 180-frame summary adds scene_matches/non_scene_latest counters.
No legacy single-camera injection remains in the production CameraMotion API.
Tests cover queued ownership, auxiliary-last publication, real cuts, ambiguity,
staleness, malformed data, bank eviction and captured-instance correspondence.

This corrects a demonstrated camera-ownership defect in the global FSR path.
Visual confirmation on the device is still required before declaring the
reported periodic instability completely eliminated.
