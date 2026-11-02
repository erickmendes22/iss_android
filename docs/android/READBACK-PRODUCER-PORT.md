# Protected-readback regression investigation and ordered page lookup

Build 73 regressed FPS in the user's phone test. Its four combined experiments
were removed in bd27d9731c8ebbbe776a675b3050fb2f6e65d87b. Correctness tests had not
established a phone performance benefit. The specific cause of that phone run
cannot be attributed without a trace, but one extra-work mechanism was reproduced.

## Reproduced batching regression

A controlled harness ran the production build-71 StartDeferredReadback,
FinishDeferredReadback and CollectDownloads with the production build-73
GetReadbackWindow policy. It requested 4 useful bytes on each of 128 pages inside
one 512-KiB window. Requested producers were complete (tick 7), a neighbor was
unfinished (tick 9), and the only copy path was the graphics queue (current tick 10).

| Window policy | Copy commands | Flushes | Guest waits | Useful bytes copied |
| --- | ---: | ---: | ---: | ---: |
| Build 71 batching | 1 | 1 | 1 | 512 |
| Build 73 requested-page narrowing | 2 | 2 | 2 | 512 |

The narrow first copy still enters the graphics stream and waits through newer
work. After it completes, reading the next page needs a second copy. A smaller
first transfer therefore adds a submission/wait without reducing total copied
bytes in this case. This demonstrates a performance regression mechanism, not an
FPS estimate or proof that it alone explains the phone's reported slowdown.

The production lifecycle fixture now uses the real GetReadbackWindow method
instead of a fixed-window stub. Its hybrid_batching case enforces one copy,
Flush and wait for this sequence. The ordinary 512-KiB Hybrid staging envelope is
preserved. No private queue, small-buffer arena promotion, new partial-Hybrid
publication or global ALL_COMMANDS wait widening is re-enabled.

## Single isolated CPU optimization

GPU-write invalidation and CPU-write handling repeatedly call CopiesTouchPages.
For large snapshots the prior predicate walked every copied island, including
writes outside the snapshot. CollectDownloads already emits nonempty, ascending,
disjoint byte intervals: tracker pages are visited in order, and dirty intervals
are enumerated and subtracted in order. Thus interval end pages are nondecreasing,
even with several islands on the same page.

OrderedReadbackCopyPagesOverlap keeps the original linear path for at most 32
copies. For larger lists it rejects writes outside the copied-page envelope in
two inspections, then binary-searches the first interval whose end page reaches
the query. It allocates nothing, mutates no snapshot state, and keeps the same
4-KiB invalidation granularity and old linear helper as the reference predicate.

The differential fixture covers 743,648 queries, unaligned arena origins, shared
pages, multi-page islands, exact borders, empty queries and sizes across the
linear/binary threshold under UBSan. For a 128-island snapshot an outside write
requires 2 copy inspections instead of 128; an internal gap requires at most 11
instead of 128. The common first-island hit stays at one inspection even for long lists. Lists
through 32 copies perform the same inspections as before. These are
bounded CPU-work results, not a whole-frame or phone FPS claim.

The protected Hybrid behind Precise 2, scene camera/world-origin/jitter fixes,
disabled FSR capture/heavy profiling, buffer residency and GPU synchronization
remain at the build-71 baseline. The Bloodframe binary is unchanged. CI builds
and signs one ARM64 APK for this isolated change, with the original certificate.

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
