"""Compile and execute the Bionic exact push-descriptor replay safety contract."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HEADER = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_descriptor_replay_memo.h"


class DescriptorReplayMemoTest(unittest.TestCase):
    def test_exact_replay_and_invalidations(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if not compiler:
            self.skipTest("no C++20 compiler available")
        source = r"""
#include <array>
#include <cstdint>
#include <span>
#include "video_core/renderer_vulkan/vk_descriptor_replay_memo.h"
int main() {
    Vulkan::DescriptorReplayMemo<std::uint64_t, std::uint64_t> memo;
    const std::array<std::uint64_t, 3> one{2, 4, 6};
    const std::array<std::uint64_t, 3> two{2, 4, 7};
    const auto yes = [&](std::uint64_t cb, std::uint64_t layout,
                         std::uint64_t tick, std::uint64_t epoch, unsigned bind,
                         const auto& payload) {
        return memo.NeedsPush(cb, layout, tick, epoch, bind, std::span(payload));
    };
    if (!yes(1, 5, 11, 1, 0, one)) return 1;
    if (yes(1, 5, 11, 1, 0, one)) return 2; // exact replay
    if (!yes(1, 5, 11, 1, 0, two)) return 3; // changed descriptor
    if (!yes(1, 6, 11, 1, 0, two)) return 4; // incompatible pipeline layout
    if (!yes(2, 6, 11, 1, 0, two)) return 5; // new command buffer
    if (!yes(2, 6, 12, 1, 0, two)) return 6; // reused handle next submission
    if (!yes(2, 6, 12, 2, 0, two)) return 7; // raw Vulkan cmd access
    if (!yes(2, 6, 12, 2, 1, two)) return 8; // independent compute bind point
    if (yes(2, 6, 12, 2, 1, two)) return 9;
    memo.Invalidate(0);
    if (!yes(2, 6, 12, 2, 0, two)) return 10; // fallback descriptor path
    if (yes(2, 6, 12, 2, 1, two)) return 11; // compute state not invalidated
    memo.InvalidateAll();
    if (!yes(2, 6, 12, 2, 1, two)) return 12;
    const std::array<std::uint64_t, 0> empty{};
    if (!yes(2, 6, 12, 2, 1, empty)) return 13; // fail closed
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            (path / "test.cpp").write_text(source)
            compile_cmd = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                           "-I", str(ROOT / "port/gpu/shadps4"),
                           str(path / "test.cpp"), "-o", str(path / "test")]
            subprocess.run(compile_cmd, check=True, timeout=45)
            subprocess.run([str(path / "test")], check=True, timeout=10)

    def test_recording_epoch_is_observed(self):
        scheduler = (ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h").read_text()
        resources = (ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_common.cpp").read_text()
        self.assertIn("descriptor_stream_epoch.fetch_add(1", scheduler)
        self.assertIn("scheduler.DescriptorStreamEpoch()", resources)
        self.assertIn("scheduler.CurrentTick()", resources)
        self.assertIn("DescriptorReplayMemo<vk::CommandBuffer, vk::PipelineLayout>", resources)
        self.assertIn("ReplayMemo().Invalidate", resources)
        self.assertIn("TLG_DESCRIPTOR_REPLAY", resources)


if __name__ == "__main__":
    unittest.main()
