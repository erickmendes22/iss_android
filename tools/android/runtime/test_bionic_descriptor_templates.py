"""Exercise compact descriptor snapshots and real Vulkan-Hpp entry-point integration."""
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
class DescriptorTemplateTests(unittest.TestCase):
    def compile_run(self, code, flags=(), cases=((),)):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            source.write_text(code)
            result = subprocess.run([CXX, "-std=c++20", "-O1", "-pthread", "-I", str(GPU),
                                     *flags, str(source), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for args in cases:
                result = subprocess.run([str(binary), *args], capture_output=True, text=True,
                                        timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_payloads_arrays_lifetime_and_rejection(self):
        self.compile_run(r'''
#include <cassert>
#include <array>
#include <limits>
#include "video_core/renderer_vulkan/descriptor_template_data.h"
using namespace Vulkan;
struct Buffer {uint64_t handle, offset, range;};
struct Image {uint64_t sampler, view, layout;};
struct Write {
    const void* pNext{};uint32_t dstBinding{},dstArrayElement{},descriptorCount{},descriptorType{};
    const Buffer* pBufferInfo{};const Image* pImageInfo{};const uint64_t* pTexelBufferView{};
};
int main() {
    DescriptorTemplateData plan;
    assert(plan.Add(0,2,7,DescriptorPayload::Buffer,sizeof(Buffer),alignof(Buffer)));
    assert(plan.Add(4,3,2,DescriptorPayload::Image,sizeof(Image),alignof(Image)));
    assert(plan.Add(7,1,4,DescriptorPayload::Texel,sizeof(uint64_t),alignof(uint64_t)));
    std::array<Buffer,2> buffers{{{1,16,48},{0,0,UINT64_MAX}}}; // null descriptor, whole size
    std::array<Image,3> images{{{0,11,5},{0,12,5},{23,0,1}}};
    uint64_t texel=999;
    std::array<Write,3> writes{{{nullptr,0,0,2,7,buffers.data()},
                               {nullptr,4,0,3,2,nullptr,images.data()},
                               {nullptr,7,0,1,4,nullptr,nullptr,&texel}}};
    std::vector<uint64_t> words((plan.bytes+7)/8);
    assert(plan.Pack(writes,words));
    auto saved=words;
    buffers[0]={90,99,900};images[0].view=80;texel=666; // simulate producer reuse before recording
    auto bytes=reinterpret_cast<const std::byte*>(words.data());
    Buffer first{};std::memcpy(&first,bytes+plan.slots[0].offset,sizeof(first));assert(first.handle==1);
    Image second{};std::memcpy(&second,bytes+plan.slots[1].offset+sizeof(Image),sizeof(second));
    assert(second.view==12);
    uint64_t old_texel{};std::memcpy(&old_texel,bytes+plan.slots[2].offset,8);assert(old_texel==999);
    assert(!plan.Pack(writes,std::span<uint64_t>{words.data(),1}));
    auto reject=[&](Write bad){auto copy=writes;copy[1]=bad;assert(!plan.Pack(copy,words));};
    auto bad=writes[1];bad.dstBinding++;reject(bad);
    bad=writes[1];bad.dstArrayElement=1;reject(bad);
    bad=writes[1];bad.descriptorCount=2;reject(bad);
    bad=writes[1];bad.descriptorType=6;reject(bad);
    bad=writes[1];bad.pImageInfo=nullptr;reject(bad);
    bad=writes[1];bad.pNext=&texel;reject(bad);
    assert(!plan.Pack(std::span<Write>{writes.data(),2},words));
    assert(!plan.Add(7,1,4,DescriptorPayload::Texel,8,8)); // repeated binding
    assert(!plan.Add(9,0,4,DescriptorPayload::Texel,8,8));
    assert(!plan.Add(9,1,4,DescriptorPayload::Texel,8,3));
    assert(!plan.Add(9,1,4,DescriptorPayload::Texel,8,16));
    assert(!plan.Add(9,UINT32_MAX,4,DescriptorPayload::Texel,8,8));
    assert(!plan.Add(9,1,4,DescriptorPayload::Texel,SIZE_MAX,8));
    auto wrong=plan;wrong.slots[1].stride=8;assert(!wrong.Pack(writes,words));
    DescriptorTemplateData empty;assert(!empty.Pack(writes,words));
    DescriptorTemplateData limit;
    assert(limit.Add(0,8192,4,DescriptorPayload::Texel,8,8));
    assert(!limit.Add(1,1,4,DescriptorPayload::Texel,8,8));
}
''', flags=("-fsanitize=undefined", "-fno-sanitize-recover=all"))

    def test_vulkan_hpp_production_methods(self):
        include = os.environ.get("VULKAN_HEADERS_INCLUDE")
        if not include:
            candidate = ROOT.parent / "vulkan_headers_check/include"
            if (candidate / "vulkan/vulkan.hpp").exists():
                include = str(candidate)
        if not include:
            self.skipTest("optional Vulkan-Headers checkout unavailable; native CI compiles integration")
        source = (GPU / "video_core/renderer_vulkan/vk_pipeline_common.cpp").read_text()
        bodies = "\n".join(method(source, signature) for signature in (
            "void Pipeline::BuildDescriptorTemplate(", "void Pipeline::BindResources("))
        fixture = (ROOT / "tools/android/runtime/cpp/descriptor_templates.cpp").read_text()
        # The real BindResources now uses recording-thread descriptor replay,
        # so the executable fixture must compile those same production helpers.
        start = source.index("namespace {\nusing ReplayCache = ")
        end = source.index("} // namespace", start) + len("} // namespace")
        memo_helpers = source[start:end]
        code = fixture.replace("// PRODUCTION_MEMO_HELPERS", memo_helpers)
        code = code.replace("// PRODUCTION_METHODS", bodies)
        self.compile_run(code, flags=("-I", include, "-D__ANDROID__=1", "-DISS_ANDROID_BIONIC=1"),
                         cases=((), ("direct",), ("compute",), ("off",), ("missing",), ("failure",),
                                ("mismatch",), ("ordinary",), ("unsupported",), ("switchdirect",),
                                ("offdirect",), ("empty",)))
        self.compile_run(code, flags=("-I", include), cases=(("nonbionic",),))

    def test_present_clear_and_filter_across_aspects_and_fallback(self):
        include = os.environ.get("VULKAN_HEADERS_INCLUDE")
        if not include:
            candidate = ROOT.parent / "vulkan_headers_check/include"
            if (candidate / "vulkan/vulkan.hpp").exists():
                include = str(candidate)
        if not include:
            self.skipTest("optional Vulkan-Headers checkout unavailable")
        source = (GPU / "video_core/renderer_vulkan/vk_presenter.cpp").read_text()
        bodies = "\n".join(method(source, signature) for signature in (
            "[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers()",
            "[[nodiscard]] vk::ImageBlit MakeImageBlit(", "static vk::Rect2D FitImage(",
            "static bool SkipPresentClear()", "static void BlitPresentedFrame("))
        fixture = (ROOT / "tools/android/runtime/cpp/present_fast_path.cpp").read_text()
        self.compile_run(fixture.replace("// PRODUCTION_METHODS", bodies),
                         flags=("-I", include), cases=((), ("fallback",)))


if __name__ == "__main__":
    unittest.main()
