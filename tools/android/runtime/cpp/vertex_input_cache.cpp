// Actual scheduler and rasterizer methods, actual pinned Vulkan-Hpp, controlled driver.
#include <cassert>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vertex_input_state.h"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using u32 = uint32_t;
using u64 = uint64_t;
namespace IssBionicPerf {
std::atomic<u64> vertex_input_issued{}, vertex_input_skipped{};
bool Enabled() { return true; }
}
static unsigned calls, buffer_calls, begins;
static std::vector<unsigned> observed_strides;
static std::vector<VkBuffer> observed_buffers;
static VKAPI_ATTR void VKAPI_CALL SetInput(VkCommandBuffer, uint32_t nb,
    const VkVertexInputBindingDescription2EXT* b, uint32_t na,
    const VkVertexInputAttributeDescription2EXT* a) {
    ++calls;
    if (nb) {
        assert(b[0].sType == VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT);
        observed_strides.push_back(b[0].stride);
    }
    if (na) assert(a[0].sType == VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT);
}
static VKAPI_ATTR void VKAPI_CALL BindBuffers(VkCommandBuffer, uint32_t first, uint32_t count,
    const VkBuffer* buffers, const VkDeviceSize* offsets) {
    assert(first == 0 && count == 1 && offsets[0] < 100);
    ++buffer_calls;
    observed_buffers.push_back(buffers[0]);
}
static VKAPI_ATTR void VKAPI_CALL BindBuffers2(VkCommandBuffer cmd, uint32_t first, uint32_t count,
    const VkBuffer* buffers, const VkDeviceSize* offsets, const VkDeviceSize* sizes,
    const VkDeviceSize* strides) {
    assert(sizes[0] == 128 && strides[0] == 16);
    BindBuffers(cmd, first, count, buffers, offsets);
}
static VKAPI_ATTR VkResult VKAPI_CALL Begin(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    ++begins; return VK_SUCCESS;
}
void Check(vk::Result r) { assert(r == vk::Result::eSuccess); }

namespace Vulkan {
struct Instance {
    bool dynamic_input = true;
    bool IsVertexInputDynamicState() const { return dynamic_input; }
};
struct Scheduler {
    VertexInputState vertex_input_state;
    bool recording_threaded = true, direct_mode = false, switch_direct = false;
    bool native_secondary_enabled = false, is_rendering = false;
    std::atomic<std::uint64_t> descriptor_stream_epoch{1};
    void EndRendering() { is_rendering = false; }
    bool owner_invalidates = false;
    unsigned owner_checks = 0;
    std::atomic<unsigned> direct_recordings{};
    vk::CommandBuffer current_cmdbuf{reinterpret_cast<VkCommandBuffer>(1)};
    struct Pool { vk::CommandBuffer Commit() { return vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(1)}; } } command_pool;
    struct Dynamic { void Invalidate() {} } dynamic_state;
    std::vector<std::unique_ptr<std::byte[]>> storage;
    std::vector<std::function<void(vk::CommandBuffer)>> commands;
    void CheckOwner(void*) {
        ++owner_checks;
        if (owner_invalidates) { vertex_input_state.Invalidate(); owner_invalidates = false; }
    }
    static void TraceDirectRecording(void*) {}
    void Drain() { for (auto& c : commands) c(current_cmdbuf); commands.clear(); storage.clear(); }
    void SyncRecording() { Drain(); }
    void ReserveRecordData(size_t) {
        if (switch_direct) { SyncRecording(); direct_mode = true; switch_direct = false; }
    }
    template<class T> std::span<const T> RecordData(std::span<const T> data) {
        if (!recording_threaded || direct_mode || data.empty()) return data;
        auto copy = std::make_unique<std::byte[]>(data.size_bytes());
        std::memcpy(copy.get(), data.data(), data.size_bytes());
        auto result = std::span<const T>{reinterpret_cast<const T*>(copy.get()), data.size()};
        storage.push_back(std::move(copy)); return result;
    }
    template<class F> void Record(F&& f) {
        if (!recording_threaded || direct_mode) f(current_cmdbuf);
        else commands.emplace_back(std::forward<F>(f));
    }
    void RecordVertexInput(std::span<const vk::VertexInputBindingDescription2EXT>,
                           std::span<const vk::VertexInputAttributeDescription2EXT>);
    void AllocateWorkerCommandBuffers();
    void BeginParallelBatch() {}
    // PRODUCTION_INLINE_METHODS
};
struct Rasterizer {
    Scheduler& scheduler;
    Instance& instance;
    struct VertexBinds {
        u32 num_buffers = 1;
        std::vector<vk::VertexInputBindingDescription2EXT> bindings{{.binding=0, .stride=16, .inputRate=vk::VertexInputRate::eVertex, .divisor=1}};
        std::vector<vk::VertexInputAttributeDescription2EXT> attributes{{.location=0, .binding=0, .format=vk::Format::eR32G32B32A32Sfloat, .offset=0}};
        std::vector<vk::Buffer> host_buffers{vk::Buffer{reinterpret_cast<VkBuffer>(11)}};
        std::vector<vk::DeviceSize> host_offsets{0}, host_sizes{128}, host_strides{16};
    } vertex_binds;
    void EmitVertexBuffers();
};
// PRODUCTION_METHODS
} // namespace Vulkan

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "deferred";
    if (mode == "off") setenv("TLG_VERTEX_INPUT_CACHE", "0", 1);
    const bool cached = mode != "off" && mode != "nonbionic";
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdSetVertexInputEXT = SetInput;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindVertexBuffers = BindBuffers;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBindVertexBuffers2 = BindBuffers2;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkBeginCommandBuffer = Begin;
    Vulkan::Scheduler s;
    if (mode == "direct") s.direct_mode = true;
    if (mode == "unthreaded") s.recording_threaded = false;
    if (mode == "switchdirect") s.switch_direct = true;
    Vulkan::Instance i;
    Vulkan::Rasterizer r{s, i};
    r.EmitVertexBuffers();
    r.vertex_binds.host_buffers[0] = vk::Buffer{reinterpret_cast<VkBuffer>(22)};
    r.vertex_binds.host_offsets[0] = 64;
    r.EmitVertexBuffers();
    assert(s.owner_checks >= 2); // ownership must be checked before a possible cache hit
    // Mutation of producer data before the recorder runs must not change queued commands.
    r.vertex_binds.bindings[0].stride = 32;
    s.Drain();
    assert(calls == (cached ? 1u : 2u));
    for (auto stride : observed_strides) assert(stride == 16);
    assert(buffer_calls == 2 && observed_buffers[0] != observed_buffers[1]);
    r.EmitVertexBuffers(); s.Drain(); assert(observed_strides.back() == 32);
    auto test_change = [&](auto change) {
        const auto before = calls;
        change(); r.EmitVertexBuffers(); s.Drain(); assert(calls == before + 1);
    };
    test_change([&] { r.vertex_binds.bindings[0].binding = 3; });
    test_change([&] { r.vertex_binds.bindings[0].inputRate = vk::VertexInputRate::eInstance; });
    test_change([&] { r.vertex_binds.bindings[0].divisor = 2; });
    test_change([&] { r.vertex_binds.attributes[0].location = 2; });
    test_change([&] { r.vertex_binds.attributes[0].binding = 3; });
    test_change([&] { r.vertex_binds.attributes[0].format = vk::Format::eR32Sfloat; });
    test_change([&] { r.vertex_binds.attributes[0].offset = 4; });
    test_change([&] { s.InvalidateVertexInput(); }); // static helper pass
    test_change([&] { (void)s.CommandBuffer(); }); // raw command buffer, including already direct
    test_change([&] { (void)s.CommandBuffer(); });
    test_change([&] { s.owner_invalidates = true; }); // owner drain changes earlier state
    test_change([&] { s.AllocateWorkerCommandBuffers(); }); // same recycled VkCommandBuffer handle
    assert(begins == 1);
    test_change([&] { r.vertex_binds.bindings.clear(); });
    test_change([&] { r.vertex_binds.attributes.clear(); r.vertex_binds.num_buffers = 0; });
    const auto empty_calls = calls;
    r.EmitVertexBuffers(); s.Drain(); assert(calls == empty_calls + (cached ? 0 : 1));
    r.vertex_binds.bindings.push_back({.binding=0, .stride=16, .inputRate=vk::VertexInputRate::eVertex, .divisor=1});
    r.vertex_binds.bindings[0].pNext = &begins;
    r.EmitVertexBuffers(); r.EmitVertexBuffers(); s.Drain(); // unknown chains never cache
    assert(!s.vertex_input_state.Matches(r.vertex_binds.bindings, r.vertex_binds.attributes));
    i.dynamic_input = false;
    r.vertex_binds.num_buffers = 1;
    const auto before_static = calls;
    r.EmitVertexBuffers(); s.Drain(); assert(calls == before_static);
    assert(IssBionicPerf::vertex_input_issued == calls);
    assert(IssBionicPerf::vertex_input_skipped == (cached ? 2u : 0u));
}
