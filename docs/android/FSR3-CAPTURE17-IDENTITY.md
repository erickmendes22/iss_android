# Capture 17 and shared-mesh history identity

Base: efc214c9208192cb73d9e20921e1427590db90c8, the build recorded in ZIP 17.

The current pair has no large object-vector spikes: maxima are 0.8765 and
1.7148 render pixels. Static rails, floor and crates have zero/negligible motion;
the nonzero vectors follow the character. The SDK history advances 84 -> 85,
with reset=false. Frame 0 current history equals frame 1 previous history byte
for byte. Global phase correlation measures approximately (-0.792,-0.608)
render pixels in the input and (0.029,0.002) output pixels after reconstruction.
This is consistent with the reported Halton phase change (-0.8125,-0.5556).
It supports the previous correction in this pair; it cannot prove stability
through an entire camera movement or gameplay session.

## Reproducible allocator fault

Motion::History identifies otherwise identical mesh draws by occurrence order.
If frame N renders A then B, and frame N+1 culls A, the first draw for B receives
A's previous vertex positions. The same failure occurs when draw order swaps.
The buffer range/topology checks do not distinguish those instances. This is
a demonstrated allocator fault, not proof that the ZIP 17 pair contains it.

The ISS-specific policy rejects a previous match if the previous frame had
multiple occurrences of that draw identity. It also rejects a second current
occurrence. Missing/ambiguous object vectors retain the existing camera-motion
fallback. Stable unique draws recover history after an unambiguous frame.
Other renderer targets keep the existing occurrence-based policy.

The policy cannot predict a new second occurrence before that draw arrives;
the first current draw can still be matched if the previous frame was unique.
The existing capture-16 vector guard therefore remains in place. Animated
duplicate meshes lack trustworthy per-instance identity and use camera fallback;
their independent animation needs device validation.

No changes to Halton jitter, source selection, reactive-map defaults, FSR SDK,
render/output resolutions, RCAS, readback mode or Android package/signing.
The regression runs the actual allocator, reproduces the old wrong A->B match,
then verifies rejection, unique-instance recovery, topology changes and resets.
The existing allocator test also runs to preserve the non-ISS behavior.
