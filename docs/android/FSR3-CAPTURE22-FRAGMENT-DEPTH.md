# Capture 22: valid face vectors rejected by their recorded depth

Base: 4deb094420dd2826e5ba24b6e5f3ec4bcc94243a, CUSA00004 01.00,
640x360 -> 1280x720. Both requested RGBA32F object-motion images are present.
The FSR pair has reset=0 and history indices 36/37, using original pre-FXAA input.

## What the raw data establishes

Face ROI: input x326..350, y164..198 (816 pixels). In f001 all 816 have
history validity B=1. Only 81 pass the existing depth comparison; 735 (90.07%)
fail. The head is not simply missing every motion pipeline. In f000 most
object history is unavailable, including much of the coat; this is separate
from f001's depth rejection and does not justify relaxing history identity.

At f001 (335,175), scene depth is 0.0488153398, while object-motion A is
0.0440880284. The approximately 9.68% difference fails the 0.5% relative depth
gate by design. Similar face differences have a median near -7.1%. These
small, finite raw RG vectors are therefore replaced by stationary-camera
vectors. The coat's raw and scene depths match in the inspected region.

The depth gate must remain strict: a genuinely occluded layer can produce the
same disagreement. This capture alone does NOT identify the face's pixel shader
or establish that it exports FragDepth.

## Reproduced emitter defect and correction

EmitFragmentMotion always recorded FragCoord.z. However the recompiler already
defines a separate Output FragDepth variable when the guest pixel shader exports
depth, and writes guest depth exports to it. Vulkan depth tests then consume that
shader output rather than the original raster depth. Using FragCoord.z as the
motion depth tag is incorrect for these programs.
The emitter already enables DepthReplacing for these programs and keeps
EarlyFragmentTests mutually exclusive, so an exported depth is not an ignored
early-test output in this path.

Read the actual FragDepth output in the motion epilogue when it exists. Keep
FragCoord.z for programs without an explicit export. Neither the game's depth
nor RG motion is changed; only the depth associated with the vector is corrected.

For ISS Android captures, negate the existing validity value on depth-exporting
programs. All three composition paths use abs(B)>0.99. Thus B=-1 identifies a
valid shader-exported depth, B=+1 a valid raster-depth source. Zero and partial
validity remain rejected at exactly the previous magnitude threshold. The sign
makes the next face capture identify this program property per pixel, without
another image attachment, storage allocation, or per-frame logging.

All outlier and depth thresholds stay unchanged. No reactivity, sharpening,
resolution, camera identity, object-history identity or Readback policy changes.
The next device capture must confirm whether this corrected path is used by the
face and whether its tagged depth now matches. A remaining positive-B mismatch
would point to a different layer/pass rather than an explicit depth-export bug.

## Verification

The regression executes the actual production fragment-motion emitter against
semantic SPIR-V operations, with distinct raster and shader-export depths. The
base fails this fixture. The corrected emitter passes host and Android variants,
front/behind-camera cases, and validity magnitudes 0, 0.5, 0.989, 0.995 and 1.
RG displacement is preserved; signed magnitude has identical eligibility. This
is a code-level test, not a replay of the supplied game or a GPU device test.

The shader cache key hashes recompiler sources, including this emitter. The
local CMake-equivalent key changes from 80be84261828 to 4f959aa3ce85, so old
translated programs cannot silently override the fix. Shader recompilation on
the first launch is expected. Package, version and release signing remain the
same to allow an in-place APK update.
