"""Real mapped readback lifecycle, sparse offsets, visibility and fallback, with UBSan."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test_bionic_resource_reuse import method

ROOT = Path(__file__).resolve().parents[3]
GPU = ROOT / "port/gpu/shadps4"
CXX = shutil.which("c++")


@unittest.skipUnless(CXX, "C++ compiler unavailable")
class MappedReadbackTests(unittest.TestCase):
    def vulkan_include(self):
        include = os.environ.get("VULKAN_HEADERS_INCLUDE")
        if not include:
            candidate = ROOT.parent / "vulkan_headers_check/include"
            if (candidate / "vulkan/vulkan.hpp").exists(): include = str(candidate)
        if not include:
            self.skipTest("pinned Vulkan-Headers checked separately in native CI")
        return include

    def test_actual_device_write_hook_including_memoized_ranges(self):
        include = self.vulkan_include()
        source = (GPU / "video_core/renderer_vulkan/vk_runtime.cpp").read_text()
        header = (GPU / "video_core/renderer_vulkan/vk_runtime.h").read_text()
        scheduler = (GPU / "video_core/renderer_vulkan/vk_scheduler.h").read_text()
        code = (ROOT / "tools/android/runtime/cpp/mapped_write_hook.cpp").read_text()
        code = code.replace("// PRODUCTION_ACCESS_BUFFER", "\n".join(method(source, signature) for signature in (
            "void Runtime::AccessBuffer(", "void Runtime::AccessBufferImpl(",
            "void Runtime::NoteBufferWrite(", "void Runtime::CopyBuffer(")))
        cache_source = (GPU / "video_core/buffer_cache/buffer_cache.cpp").read_text()
        cache_header = (GPU / "video_core/buffer_cache/buffer_cache.h").read_text()
        code = code.replace("// PRODUCTION_READBACK_TYPE", method(cache_header, "struct DeferredReadback {") + ";")
        code = code.replace("// PRODUCTION_NOTE_WRITE", method(cache_source, "void BufferCache::NoteDeviceWrite("))
        code = code.replace("// PRODUCTION_HOOK_SETTER", method(header, "void SetBufferWriteHook("))
        code = code.replace("// PRODUCTION_REQUIRE_BARRIER", method(scheduler, "void RequireHostReadBarrier("))
        self.compile_run(code, ("-I", include), ((),))

    def test_write_ticks_against_page_reference(self):
        code = r'''
#include <cassert>
#include <random>
#include <array>
#include "video_core/buffer_cache/write_tick_tracker.h"
int main() {
    VideoCore::WriteTickTracker t;
    std::array<uint64_t,512> ref{};
    std::mt19937 random(90231);
    constexpr uint64_t base=(1ull<<26)-256*4096;
    assert(t.Latest(base,1)==0);
    assert(t.Write(base+1,4094,1));
    assert(!t.Write(base+2,4092,1)); // different bytes, identical covered pages
    assert(t.Write(base+1,4094,2)); // next submission must be observed again
    assert(!t.Write(base+1,4094,1)); // an older observation cannot lower the maximum
    ref[0]=2;
    for(unsigned iteration=0;iteration<20000;++iteration) {
        const unsigned a=random()%512,b=a+1+random()%(512-a);
        const uint64_t tick=1+random()%10000; // nested/out-of-order observations remain conservative
        const bool changed=t.Write(base+a*4096+1,(b-a)*4096-2,tick);
        if(!changed)for(unsigned i=a;i<b;++i)assert(ref[i]>=tick);
        for(unsigned i=a;i<b;++i)ref[i]=std::max(ref[i],tick);
        const unsigned x=random()%512,y=x+1+random()%(512-x);
        uint64_t want=0;for(unsigned i=x;i<y;++i)want=std::max(want,ref[i]);
        assert(t.Latest(base+x*4096+1,(y-x)*4096-2)==want);
    }
    t.Write(0,1ull<<40,10001);assert(t.Latest((1ull<<40)-1,1)==10001);
    t.Write(100,0,0);assert(t.Latest(0,1)==10001);
    assert(t.Latest(~0ull,2)==~0ull);
    t.Write((1ull<<40)-1,2,10002);assert(t.Latest(0,1)==~0ull);
}
'''
        self.compile_run(code, (), ((),))

    def compile_run(self, code, flags, cases):
        with tempfile.TemporaryDirectory() as directory:
            source, binary = Path(directory) / "test.cpp", Path(directory) / "test"
            source.write_text(code)
            result = subprocess.run([CXX, "-std=c++20", "-O1", "-pthread",
                "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I", str(GPU),
                *flags, str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for args in cases:
                result = subprocess.run([str(binary), *args], capture_output=True,
                                        text=True, timeout=20)
                self.assertEqual(result.returncode, 0, f"{args}: {result.stderr}")

    def test_production_sparse_mapped_lifecycle(self):
        include = self.vulkan_include()
        source = (GPU / "video_core/buffer_cache/buffer_cache.cpp").read_text()
        header = (GPU / "video_core/buffer_cache/buffer_cache.h").read_text()
        scheduler = (GPU / "video_core/renderer_vulkan/vk_scheduler.cpp").read_text()
        code = (ROOT / "tools/android/runtime/cpp/mapped_readback.cpp").read_text()
        for marker, signature in (
            ("// PRODUCTION_READBACK_TYPE", "struct DeferredReadback {"),
            ("// PRODUCTION_MAPPING_TYPE", "struct ResidencyMapping {"),
            ("// PRODUCTION_BACKING_TYPE", "struct Backing : public Interval {"),
            ("// PRODUCTION_ATTEMPT_TYPE", "struct ResidencyAttempt {"),
        ):
            code = code.replace(marker, method(header, signature) + ";")
        code = code.replace("// PRODUCTION_REQUIRE_BARRIER", method(
            (GPU / "video_core/renderer_vulkan/vk_scheduler.h").read_text(),
            "void RequireHostReadBarrier("))
        code = code.replace("// PRODUCTION_SCHEDULER",
                            method(scheduler, "void Scheduler::FlushHostReadBarrier()"))
        signatures = (
            "bool BufferCache::MappedReadbackPolicy()", "void BufferCache::NoteDeviceWrite(",
            "void BufferCache::MarkDeferredStale(",
            "bool BufferCache::PrepareMappedReadback(", "bool BufferCache::CaptureMappedReadback(",
            "u64 BufferCache::CollectDownloads(",
            "std::shared_ptr<BufferCache::DeferredReadback> BufferCache::StartDeferredReadback(",
            "bool BufferCache::FinishDeferredReadback(", "void BufferCache::RecoverDeferredReadback(",
            "void BufferCache::SettleDeferredReadbacks(", "void BufferCache::PruneDeferredReadbacks()",
            "std::optional<std::pair<vk::DeviceMemory, u64>> BufferCache::AllocateResidency(",
            "void BufferCache::UndoResidency(",
        )
        code = code.replace("// PRODUCTION_BUFFER_METHODS", "\n".join(method(source, s) for s in signatures))
        modes = ("coherent", "noncoherent", "current", "unknown", "hole", "mapfail",
                 "stale", "recheck", "disjoint", "outsidecurrent", "inside64current", "onepage",
                 "invalidatefail", "ready", "readyinvalidatefail", "off", "dma", "precise",
                 "currentoff", "currentready", "currentstale", "currentmapfail", "currentnoncoherent",
                 "replacement", "replacement_stage", "replacement_unrelated",
                 "partial_neighbor", "partial_note", "partial_recheck", "partial_noncoherent",
                 "partial_all", "partial_cpu", "partial_inflight", "partial_clean", "partial_disjoint", "partial_gap",
                 "partial_new_request", "partial_new_request_ready",
                 "priority_inflight", "priority_ready", "priority_neighbor_ready", "priority_same_tick",
                 "priority_current", "priority_join", "priority_join_ready", "priority_coherent", "priority_noncoherent",
                 "hybrid_stage_write", "hybrid_stage_gap", "hybrid_targeted_pages")
        self.compile_run(code, ("-I", include, "-D__ANDROID__=1", "-DISS_ANDROID_BIONIC=1"),
                         tuple((mode,) for mode in modes))
        self.compile_run(code, ("-I", include), (("nonbionic",),))


if __name__ == "__main__":
    unittest.main()
