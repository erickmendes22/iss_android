"""Real scheduler queue, cuts, raw access and host ordering on two live threads."""
import unittest
from test_bionic_resource_reuse import method
import test_bionic_mapped_readback as mapped


@unittest.skipUnless(mapped.CXX, "C++ compiler unavailable")
class ParallelRecordingTests(unittest.TestCase):
    vulkan_include = mapped.MappedReadbackTests.vulkan_include
    compile_run = mapped.MappedReadbackTests.compile_run

    def test_production_parallel_recording_and_ordered_publication(self):
        include = self.vulkan_include()
        renderer = mapped.GPU / "video_core/renderer_vulkan"
        header = (renderer / "vk_scheduler.h").read_text()
        source = (renderer / "vk_scheduler.cpp").read_text()
        code = (mapped.ROOT / "tools/android/runtime/cpp/parallel_recording.cpp").read_text()
        code = code.replace("// PRODUCTION_CHUNK", method(header, "class RecordChunk {") + ";")
        code = code.replace("// PRODUCTION_INLINE", "\n".join(("template <typename Func>\n" if s.startswith(("void Record(", "void RecordHostCopy(", "void AfterRecordingPrefix(")) else "template <typename T>\n" if s.startswith("std::span") else "") + method(header, s) for s in (
            "void Record(Func&& func)", "void RecordPrimary(Func&& func)", "void ReserveRecordData(", "std::span<const T> RecordData(",
            "void RecordHostCopy(Func&& copy)", "void AfterRecordingPrefix(", "vk::CommandBuffer CommandBuffer()",
            "void RequireHostReadBarrier(")))
        code = code.replace("// PRODUCTION_METHODS", "\n".join(method(source, s) for s in (
            "std::unique_ptr<RecordChunk> Scheduler::AcquireChunk()",
            "void Scheduler::HandOverParallel()", "void Scheduler::SplitRecordingAtBoundary(",
            "void Scheduler::SplitRecording()", "void Scheduler::BeginParallelBatch()",
            "std::span<const vk::CommandBuffer> Scheduler::EndRecordingBuffers()",
            "void Scheduler::KickRecording(", "void Scheduler::SyncRecording()",
            "void Scheduler::SignalAfterHostCopies(", "void Scheduler::WaitHostCopies()",
            "void Scheduler::WaitDeferredSignals()", "void Scheduler::FlushHostReadBarrier()")))
        self.compile_run(code, ("-I", include), ((),))


if __name__ == "__main__":
    unittest.main()
