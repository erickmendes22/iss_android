# Producer-aware protected readbacks on Bionic

Base: build 71 (`abb10993a7416edb020757b3b31a56ef77889a59`). Reference: Castlesea/infamous-second-son-pc-port `493f772cee9cbc62448c2517d477989b8715deac`, especially BeginReadback/FinishReadback, the private ReadbackQueue, and the CPU-clean small-buffer binding path in gpu/overrides/buffer_cache.cpp.

The Android label “Precise 2” still selects protected Hybrid V6 (`TLG_READBACKS=1`, read protection, staged copies). It does not silently switch to native mode 2. The scene-camera/world-origin/depth-jitter corrections and disabled FSR capture/profiling of build 71 are preserved.

## Integrated changes

1. Hybrid staging snapshots now track GPU invalidation per copied 4-KiB page. Completion publishes every valid dirty island on each valid page, recovers changed intervals for a later read, and keeps those pages protected. A CPU write without a fault still invalidates the whole snapshot and waits out an active publication. A requested invalidated page retries through the existing bounded asynchronous path; the final synchronous fallback remains.
2. A small protected request can narrow the ordinary 512-KiB staging window to its requested pages when every requested page has a known completed producer and surrounding pages have newer unfinished writes. Unknown provenance, unfinished requested producers and equally ready neighbors keep the ordinary window. All dirty islands on every selected page are copied. Narrowing transfers alone cannot bypass earlier graphics commands on a single queue.
3. Hybrid can request a second queue in the **same graphics family** when advertised. Device creation retries with one queue if the two-queue request fails. A private copy is eligible only when every copied page has known provenance, its producers have actually been submitted, no sparse binds are pending, and DMA is off. Otherwise the existing graphics copy/Flush path is used. The private submission waits on producer and published sparse-residency timelines, then records a transfer-write → host-read barrier. All subsequent graphics submissions wait for the private read timeline, including when no new sparse binds exist, preventing later arena writes from racing the copy. Graphics producer ticks and private completion serials are distinct fields. Queue command buffers are reused only after completion; storage is bounded at 64, with the normal graphics path used when all slots are busy instead of a GPU wait on the cache owner. The queue drains before arena/staging destruction. This implementation does not use another queue family or introduce ownership transfers.
4. Small read-only buffers may bind an existing fully resident arena directly when CPU/GPU tracking is clean, the offset is aligned, the binding is non-texel and the current runtime hazard test is clear. Current arena identity and page state are checked on every call; there is no persistent cache of guest bytes. Dirty/dynamic/unresident/unproven bindings keep the stream path. This adapts the fork's stable-arena reuse without accepting speculative stamps for writable guest pages.

The scheduler now records the latest successfully driver-submitted graphics tick. Its wait-stage array covers all four SubmitInfo wait slots with ALL_COMMANDS, allowing acquire, readback WAR and sparse-residency dependencies to coexist without an out-of-bounds stage read or a color-only dependency on compute/transfer writes.

## Regression checks and fallbacks

- Production mapped/staged lifecycle tests cover one/all/disjoint GPU page invalidations, CPU invalidation, disabled partial policy, tick revalidation, all islands on one page, concurrent/shared completions, coherent/non-coherent mapped memory, and retry preserving a newer CPU byte on an already published page. Every deferred allocation must be released.
- Production request/arena methods cover ready/unfinished/unknown producers, ordinary window fallback, disabled policy, DMA, dirty CPU/GPU data, alignment, texel bindings, holes, hazards, changed arena identity and a CPU write during validation.
- Production private queue code runs against Vulkan-Hpp mock dispatch: exact regions, transfer/host scopes, producer/residency waits, 70 submissions with bounded command reuse, and teardown completion.
- Production staging submission and graphics submit callback verify unknown/unsubmitted producers (including a partly unknown span), DMA, pending binds, absent queue, correct completion timeline, and the WAR wait with no pending sparse binds. The scheduler fixture includes three simultaneous waits and checks every stage mask and submitted tick.
- CI runs these tests again with its pinned Vulkan headers, then builds the complete ARM64/Bionic renderer and signs/verifies the APK. No phone FPS or image-correctness result is inferred from host tests.

Developer fallbacks: `TLG_HYBRID_STAGE_PARTIAL=0`, `TLG_HYBRID_REQUEST_PAGES=0`, `TLG_READBACK_QUEUE=0`, `TLG_STABLE_SMALL_READ=0`. Single-queue drivers automatically retain the graphics submission path. The new queue is requested only for protected Hybrid, preserving ordinary Relaxed's queue configuration. Existing eager backoff/targeted-staging experiments and texture GC thresholds remain unchanged.

## Build dependency recovery

Build 72 passed runtime tests, including the new pinned-header producer/queue tests,
but failed before renderer compilation: Bloodframe's pinned DXBC submodule repository
`PancakeTAS/dxbc` is no longer public. No DXBC version is substituted.

The performance workflow explicitly opts into `ISS_BLOODFRAME_BOOTSTRAP=1` to recover
only the unchanged AArch64 `libbloodframe.so` and its five license/provenance files
from our successful build 71 APK (run 41363054526, artifact 13106552333). The manifest
pins the Actions ZIP, APK, runtime and all six extracted file hashes. It also pins
the Bloodframe commit, C API header and existing host-sync patch; changing those
inputs rejects reuse. The renderer, CPU runtime, driver and launcher build from
the new source as usual. Archive corruption and changed inputs fail before writes;
GitHub authorization is removed on the signed-blob redirect.

This is a temporary dependency bootstrap, not a new Bloodframe source distribution.
The pinned Actions artifact expires 2027-01-30; after expiration CI fails clearly
unless the exact source or another verified dependency cache has been restored.
Ordinary builds without the opt-in still compile Bloodframe from source. A local
verified download can be supplied to `stage_bloodframe_bootstrap.py --archive ZIP
--commit c29027555cfa8c9fc73c20f03816230becb9f469 --output out/bionic`.
