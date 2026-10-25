// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <source_location>
#include <utility>
#include <vector>
#include <mutex>
#include <thread>
#include <queue>

#include "tlg_toggles.h"
#include "common/assert.h"
#include "common/interval_set.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/host_copy_sources.h"
#include "video_core/renderer_vulkan/vertex_input_state.h"
#include "video_core/renderer_vulkan/parallel_record_queue.h"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;

struct RenderAttachment {
    vk::ImageView image_view;
    vk::ImageLayout image_layout;
    std::array<u32, 4> clear_value;
    union {
        u32 is_clear;
        struct {
            bool has_depth;
            bool depth_clear;
            bool has_stencil;
            bool stencil_clear;
        };
    };
};
static_assert(std::has_unique_object_representations_v<RenderAttachment>);

struct RenderState {
    std::array<RenderAttachment, 8> color_attachments;
    RenderAttachment depth_stencil_attachment;
    u16 width;
    u16 height;
    u16 num_layers;
    u16 num_color_attachments;

    // Native Passes ARM64: exact pipeline formats for Vulkan dynamic-rendering
    // secondary inheritance. Not included in render-state identity unless enabled.
    std::array<vk::Format, 8> native_color_formats{};
    vk::Format native_depth_format{vk::Format::eUndefined};
    vk::Format native_stencil_format{vk::Format::eUndefined};
    vk::SampleCountFlagBits native_samples{vk::SampleCountFlagBits::e1};
    u32 native_eligible{0};

    bool operator==(const RenderState& other) const noexcept {
        return std::memcmp(this, &other, sizeof(RenderState)) == 0;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

struct SubmitInfo {
    std::array<vk::Semaphore, 4> wait_semas;
    std::array<u64, 4> wait_ticks;
    std::array<vk::Semaphore, 4> signal_semas;
    std::array<u64, 4> signal_ticks;
    vk::Fence fence;
    u32 num_wait_semas;
    u32 num_signal_semas;

    void AddWait(vk::Semaphore semaphore, u64 tick = 1) {
        wait_semas[num_wait_semas] = semaphore;
        wait_ticks[num_wait_semas++] = tick;
    }

    void AddSignal(vk::Semaphore semaphore, u64 tick = 1) {
        signal_semas[num_signal_semas] = semaphore;
        signal_ticks[num_signal_semas++] = tick;
    }

    void AddSignal(vk::Fence fence) {
        this->fence = fence;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        return fail_op == other.fail_op && pass_op == other.pass_op &&
               depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
    }
};
struct DynamicState {
    struct {
        bool viewports : 1;
        bool scissors : 1;

        bool depth_test_enabled : 1;
        bool depth_write_enabled : 1;
        bool depth_compare_op : 1;

        bool depth_bounds_test_enabled : 1;
        bool depth_bounds : 1;

        bool depth_bias_enabled : 1;
        bool depth_bias : 1;

        bool stencil_test_enabled : 1;
        bool stencil_front_ops : 1;
        bool stencil_front_reference : 1;
        bool stencil_front_write_mask : 1;
        bool stencil_front_compare_mask : 1;
        bool stencil_back_ops : 1;
        bool stencil_back_reference : 1;
        bool stencil_back_write_mask : 1;
        bool stencil_back_compare_mask : 1;

        bool primitive_restart_enable : 1;
        bool rasterizer_discard_enable : 1;
        bool cull_mode : 1;
        bool front_face : 1;

        bool blend_constants : 1;
        bool color_write_masks : 1;
        bool line_width : 1;
        bool feedback_loop_enabled : 1;
    } dirty_state{};

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    /// Commits the dynamic state to the provided command buffer (a null one only clears the
    /// flags that committing clears).
    void Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf);

    /// tlg: passes each dirty state change to `emit` as a small command closure holding only
    /// its own values (recording a copy of the whole state per draw was a hot spot), and clears
    /// the flags it emitted. Flags whose test is disabled stay dirty, as in Commit.
    template <typename Emit>
    void CommitWith(bool depth_bounds_supported, bool color_write_mask_supported,
                    bool feedback_loop_supported, Emit&& emit) {
        using Face = vk::StencilFaceFlagBits;
        if (dirty_state.viewports) {
            dirty_state.viewports = false;
            emit([v = viewports](vk::CommandBuffer c) { c.setViewportWithCount(v); });
        }
        if (dirty_state.scissors) {
            dirty_state.scissors = false;
            emit([v = scissors](vk::CommandBuffer c) { c.setScissorWithCount(v); });
        }
        if (dirty_state.depth_test_enabled) {
            dirty_state.depth_test_enabled = false;
            emit([v = depth_test_enabled](vk::CommandBuffer c) { c.setDepthTestEnable(v); });
        }
        if (dirty_state.depth_write_enabled) {
            // Must be set in a command buffer even if depth test is disabled.
            dirty_state.depth_write_enabled = false;
            emit([v = depth_write_enabled](vk::CommandBuffer c) { c.setDepthWriteEnable(v); });
        }
        if (depth_test_enabled && dirty_state.depth_compare_op) {
            dirty_state.depth_compare_op = false;
            emit([v = depth_compare_op](vk::CommandBuffer c) { c.setDepthCompareOp(v); });
        }
        if (dirty_state.depth_bounds_test_enabled) {
            dirty_state.depth_bounds_test_enabled = false;
            if (depth_bounds_supported) {
                emit([v = depth_bounds_test_enabled](vk::CommandBuffer c) {
                    c.setDepthBoundsTestEnable(v);
                });
            }
        }
        if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
            dirty_state.depth_bounds = false;
            if (depth_bounds_supported) {
                emit([lo = depth_bounds_min, hi = depth_bounds_max](vk::CommandBuffer c) {
                    c.setDepthBounds(lo, hi);
                });
            }
        }
        if (dirty_state.depth_bias_enabled) {
            dirty_state.depth_bias_enabled = false;
            emit([v = depth_bias_enabled](vk::CommandBuffer c) { c.setDepthBiasEnable(v); });
        }
        if (depth_bias_enabled && dirty_state.depth_bias) {
            dirty_state.depth_bias = false;
            emit([k = depth_bias_constant, clamp = depth_bias_clamp,
                  slope = depth_bias_slope](vk::CommandBuffer c) { c.setDepthBias(k, clamp, slope); });
        }
        if (dirty_state.stencil_test_enabled) {
            dirty_state.stencil_test_enabled = false;
            emit([v = stencil_test_enabled](vk::CommandBuffer c) { c.setStencilTestEnable(v); });
        }
        if (stencil_test_enabled) {
            const auto ops = [&](Face face, const StencilOps& o) {
                emit([face, o](vk::CommandBuffer c) {
                    c.setStencilOp(face, o.fail_op, o.pass_op, o.depth_fail_op, o.compare_op);
                });
            };
            if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
                stencil_front_ops == stencil_back_ops) {
                ops(Face::eFrontAndBack, stencil_front_ops);
            } else {
                if (dirty_state.stencil_front_ops) {
                    ops(Face::eFront, stencil_front_ops);
                }
                if (dirty_state.stencil_back_ops) {
                    ops(Face::eBack, stencil_back_ops);
                }
            }
            dirty_state.stencil_front_ops = dirty_state.stencil_back_ops = false;

            const auto reference = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilReference(face, v); });
            };
            if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
                stencil_front_reference == stencil_back_reference) {
                reference(Face::eFrontAndBack, stencil_front_reference);
            } else {
                if (dirty_state.stencil_front_reference) {
                    reference(Face::eFront, stencil_front_reference);
                }
                if (dirty_state.stencil_back_reference) {
                    reference(Face::eBack, stencil_back_reference);
                }
            }
            dirty_state.stencil_front_reference = dirty_state.stencil_back_reference = false;

            const auto write_mask = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilWriteMask(face, v); });
            };
            if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
                stencil_front_write_mask == stencil_back_write_mask) {
                write_mask(Face::eFrontAndBack, stencil_front_write_mask);
            } else {
                if (dirty_state.stencil_front_write_mask) {
                    write_mask(Face::eFront, stencil_front_write_mask);
                }
                if (dirty_state.stencil_back_write_mask) {
                    write_mask(Face::eBack, stencil_back_write_mask);
                }
            }
            dirty_state.stencil_front_write_mask = dirty_state.stencil_back_write_mask = false;

            const auto compare_mask = [&](Face face, u32 v) {
                emit([face, v](vk::CommandBuffer c) { c.setStencilCompareMask(face, v); });
            };
            if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
                stencil_front_compare_mask == stencil_back_compare_mask) {
                compare_mask(Face::eFrontAndBack, stencil_front_compare_mask);
            } else {
                if (dirty_state.stencil_front_compare_mask) {
                    compare_mask(Face::eFront, stencil_front_compare_mask);
                }
                if (dirty_state.stencil_back_compare_mask) {
                    compare_mask(Face::eBack, stencil_back_compare_mask);
                }
            }
            dirty_state.stencil_front_compare_mask = dirty_state.stencil_back_compare_mask = false;
        }
        if (dirty_state.primitive_restart_enable) {
            dirty_state.primitive_restart_enable = false;
            emit([v = primitive_restart_enable](vk::CommandBuffer c) {
                c.setPrimitiveRestartEnable(v);
            });
        }
        if (dirty_state.rasterizer_discard_enable) {
            dirty_state.rasterizer_discard_enable = false;
            emit([v = rasterizer_discard_enable](vk::CommandBuffer c) {
                c.setRasterizerDiscardEnable(v);
            });
        }
        if (dirty_state.cull_mode) {
            dirty_state.cull_mode = false;
            emit([v = cull_mode](vk::CommandBuffer c) { c.setCullMode(v); });
        }
        if (dirty_state.front_face) {
            dirty_state.front_face = false;
            emit([v = front_face](vk::CommandBuffer c) { c.setFrontFace(v); });
        }
        if (dirty_state.blend_constants) {
            dirty_state.blend_constants = false;
            emit([v = blend_constants](vk::CommandBuffer c) { c.setBlendConstants(v.data()); });
        }
        if (dirty_state.color_write_masks) {
            dirty_state.color_write_masks = false;
            if (color_write_mask_supported) {
                emit([v = color_write_masks](vk::CommandBuffer c) { c.setColorWriteMaskEXT(0, v); });
            }
        }
        if (dirty_state.line_width) {
            dirty_state.line_width = false;
            emit([v = line_width](vk::CommandBuffer c) { c.setLineWidth(v); });
        }
        if (dirty_state.feedback_loop_enabled && feedback_loop_supported) {
            dirty_state.feedback_loop_enabled = false;
            emit([v = feedback_loop_enabled](vk::CommandBuffer c) {
                c.setAttachmentFeedbackLoopEnableEXT(v ? vk::ImageAspectFlagBits::eColor
                                                       : vk::ImageAspectFlagBits::eNone);
            });
        }
    }

    /// tlg: true when Commit() would record anything. Flags that Commit() defers (their
    /// test is disabled) do not count.
    [[nodiscard]] bool AnyDirty() const noexcept {
        auto pending = dirty_state;
        if (!depth_test_enabled) {
            pending.depth_compare_op = false;
        }
        if (!depth_bounds_test_enabled) {
            pending.depth_bounds = false;
        }
        if (!depth_bias_enabled) {
            pending.depth_bias = false;
        }
        if (!stencil_test_enabled) {
            pending.stencil_front_ops = pending.stencil_back_ops = false;
            pending.stencil_front_reference = pending.stencil_back_reference = false;
            pending.stencil_front_write_mask = pending.stencil_back_write_mask = false;
            pending.stencil_front_compare_mask = pending.stencil_back_compare_mask = false;
        }
        static constexpr decltype(dirty_state) clean{};
        return std::memcmp(&pending, &clean, sizeof(pending)) != 0;
    }

    /// Invalidates all dynamic state to be flushed into the next command buffer.
    void Invalidate() {
        // tlg: named flags only; padding bits set by a memset made AnyDirty() always true.
        dirty_state.viewports = true;
        dirty_state.scissors = true;
        dirty_state.depth_test_enabled = true;
        dirty_state.depth_write_enabled = true;
        dirty_state.depth_compare_op = true;
        dirty_state.depth_bounds_test_enabled = true;
        dirty_state.depth_bounds = true;
        dirty_state.depth_bias_enabled = true;
        dirty_state.depth_bias = true;
        dirty_state.stencil_test_enabled = true;
        dirty_state.stencil_front_ops = true;
        dirty_state.stencil_front_reference = true;
        dirty_state.stencil_front_write_mask = true;
        dirty_state.stencil_front_compare_mask = true;
        dirty_state.stencil_back_ops = true;
        dirty_state.stencil_back_reference = true;
        dirty_state.stencil_back_write_mask = true;
        dirty_state.stencil_back_compare_mask = true;
        dirty_state.primitive_restart_enable = true;
        dirty_state.rasterizer_discard_enable = true;
        dirty_state.cull_mode = true;
        dirty_state.front_face = true;
        dirty_state.blend_constants = true;
        dirty_state.color_write_masks = true;
        dirty_state.line_width = true;
        dirty_state.feedback_loop_enabled = true;
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
            depth_bias_slope != slope) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if (blend_constants != blend_constants_) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (!std::ranges::equal(color_write_masks, color_write_masks_)) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }
};

using SubmitFunc = Common::UniqueFunction<void, SubmitInfo&>;

/// tlg: a block of deferred Vulkan commands. Commands are closures placed in
/// fixed storage (no allocation per command) and run in order on the recording thread.
class RecordChunk {
public:
    static constexpr size_t Capacity = 128 * 1024;

    /// Returns false (and leaves `func` untouched) when the chunk has no room.
    template <typename Func>
    bool Push(Func&& func) {
        using Command = TypedCommand<std::decay_t<Func>>;
        static_assert(sizeof(Command) <= Capacity, "recorded command is too large");
        const size_t offset = (used + alignof(Command) - 1) & ~(alignof(Command) - 1);
        if (offset + sizeof(Command) > Capacity) {
            return false;
        }
        auto* command = new (storage + offset) Command(std::forward<Func>(func));
        if (last) {
            last->next = command;
        } else {
            first = command;
        }
        last = command;
        used = offset + sizeof(Command);
        PrefetchAhead();
        return true;
    }

    /// Raw storage in the chunk for a command's variable-length data; null when full.
    void* Allocate(size_t bytes, size_t align) {
        const size_t offset = (used + align - 1) & ~(align - 1);
        if (offset + bytes > Capacity) {
            return nullptr;
        }
        used = offset + bytes;
        PrefetchAhead();
        return storage + offset;
    }

    void Execute(vk::CommandBuffer cmdbuf) {
        for (CommandBase* command = first; command;) {
            CommandBase* const next = command->next;
            command->Execute(cmdbuf);
            command->~CommandBase();
            command = next;
        }
        first = last = nullptr;
        used = 0;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return first == nullptr;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return used;
    }

private:
    /// The recording thread last read these cache lines: the first write to each has to take
    /// it back from that core. Requesting ownership a few lines ahead overlaps those
    /// transfers instead of stalling on each (RecordPrefetch).
    void PrefetchAhead() const {
        if (!TlgToggle::Disabled(TlgToggle::RecordPrefetch)) {
            __builtin_prefetch(storage + used + 384, 1, 3);
            __builtin_prefetch(storage + used + 448, 1, 3);
        }
    }

    struct CommandBase {
        virtual ~CommandBase() = default;
        virtual void Execute(vk::CommandBuffer cmdbuf) = 0;
        CommandBase* next{};
    };
    template <typename Func>
    struct TypedCommand final : CommandBase {
        explicit TypedCommand(Func&& func_) : func{std::move(func_)} {}
        explicit TypedCommand(const Func& func_) : func{func_} {}
        void Execute(vk::CommandBuffer cmdbuf) override {
            func(cmdbuf);
        }
        Func func;
    };

    alignas(64) std::byte storage[Capacity];
    size_t used = 0;
    CommandBase* first{};
    CommandBase* last{};
};

/// tlg: notified when the scheduler's rendering scopes begin and end, for work that cannot span
/// them (TLG_OCCLUSION_MODE=4: Vulkan occlusion queries begun in a rendering scope end in it).
/// Called on the thread that owns the scheduler; record with Scheduler::Record.
class RenderingObserver {
public:
    virtual ~RenderingObserver() = default;
    /// The rendering scope has just begun.
    virtual void OnBeginRendering() = 0;
    /// The rendering scope is about to end.
    virtual void OnEndRendering() = 0;
    // Observers with active GPU queries cannot inherit those queries in a
    // secondary command buffer without the Vulkan inheritedQueries feature.
    virtual bool AllowsSecondaryRendering() const noexcept { return false; }
};

class Scheduler {
public:
    /// `threaded_recording`: Vulkan commands passed to Record() are recorded by a worker thread.
    explicit Scheduler(const Instance& instance, bool threaded_recording = false);
    ~Scheduler();

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush(SubmitInfo& info);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush();

    /// An arena write needs device-to-host visibility at this submission's completion.
    void RequireHostReadBarrier(
        vk::PipelineStageFlags2 stage = vk::PipelineStageFlagBits2::eAllCommands,
        vk::AccessFlags2 access = vk::AccessFlagBits2::eMemoryWrite) {
        host_read_barrier = true;
        host_read_src_stages |= stage;
        host_read_src_access |= access;
    }
    void FlushHostReadBarrier();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish(std::source_location caller = std::source_location::current());

    /// tlg: waits for the work submitted so far but leaves the command buffer being
    /// recorded open, so a CommandBuffer() the caller holds stays valid (Finish() submits it:
    /// recording into a submitted buffer crashes AMD's driver).
    void WaitSubmitted(std::source_location caller = std::source_location::current());

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick, std::source_location caller = std::source_location::current(),
              u64 submit_ns = 0);

    /// Attempts to execute operations whose tick the GPU has caught up with.
    void PopPendingOperations();

    /// Starts a new rendering scope with provided state.
    void BeginRendering(const RenderState& new_state);

    /// Ends current rendering scope.
    void EndRendering();

    /// Called before any bindings/dynamic state of the next draw are emitted.
    void SplitRecordingAtBoundary(const RenderState& next_state);

    /// tlg: TLG_FULL_BARRIERS=1 records a full memory barrier around every render pass and
    /// every compute dispatch, to tell a missing barrier from everything else.
    static bool FullBarriers();
    static void RecordFullBarrier(vk::CommandBuffer cmdbuf);

    /// Sets a function to be called on every scheduler submission.
    void SetSubmitCallback(SubmitFunc&& on_submit) {
        this->on_submit = std::move(on_submit);
    }

    /// tlg: while draw packets are in flight the draw recording thread (stage B) owns the
    /// deferred recording state (record_chunk, full_chunks). A call from any other thread
    /// first runs `hook(context, caller)`: the rasterizer drains the pipe on stage A and logs
    /// the caller, so two threads never use record_chunk at once (EmitVertexBuffers crash).
    void SetOwnerHook(void (*hook)(void*, void*), void* context) {
        owner_hook = hook;
        owner_context = context;
    }

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the current command buffer for recording on the calling thread. With threaded
    /// recording this first waits until all commands passed to Record() are recorded, then
    /// records directly (Record() included, so callers may mix both) until the next
    /// KickRecording() or submission.
    [[gnu::noinline]] vk::CommandBuffer CommandBuffer() {
        CheckOwner(__builtin_return_address(0));
        // A raw primary command call cannot execute inside a scope begun with
        // eContentsSecondaryCommandBuffers. Close the scope before switching
        // to direct mode, then the next DrawRecord begins another scope.
        if (native_secondary_enabled && is_rendering) {
            EndRendering();
        }
        if (recording_threaded && !direct_mode) {
            SyncRecording();
            direct_mode = true;
            direct_recordings.fetch_add(1, std::memory_order_relaxed);
            TraceDirectRecording(__builtin_return_address(0));
        }
        // Raw callers may bind a graphics pipeline with static vertex input. Even if
        // already in direct mode, do not reuse state across an untracked command stream.
        vertex_input_state.Invalidate();
        return current_cmdbuf;
    }

    /// Records dynamic vertex format only when changed. Buffer addresses remain per draw.
    void RecordVertexInput(std::span<const vk::VertexInputBindingDescription2EXT> bindings,
                           std::span<const vk::VertexInputAttributeDescription2EXT> attributes);

    /// Call before recording a helper graphics pipeline with static vertex input.
    void InvalidateVertexInput() {
        CheckOwner(__builtin_return_address(0));
        vertex_input_state.Invalidate();
    }

    /// TLG_RECORDER_TRACE=1: prints the most frequent CommandBuffer() callers every 2000 calls.
    static void TraceDirectRecording(void* caller);

    /// Records `func(vk::CommandBuffer)` in order with other commands. The closure must own
    /// everything it uses (capture by value): it may run later on the recording thread.
    template <typename Func>
    void Record(Func&& func) {
        // The normal Record() path redirects Vulkan draw/bind state into the
        // secondary while an eligible dynamic-rendering scope is active.
        // BeginRendering/EndRendering use RecordPrimary() instead.
        if (native_secondary_enabled && IsRecordingDeferred()) {
            const unsigned lane = parallel_lane;
            RecordPrimary([this, lane, fn = std::forward<Func>(func)](
                              vk::CommandBuffer primary) mutable {
                const vk::CommandBuffer active =
                    native_secondary_scopes[lane].active
                        ? native_secondary_scopes[lane].buffer
                        : primary;
                fn(active);
            });
            return;
        }
        RecordPrimary(std::forward<Func>(func));
    }

    template <typename Func>
    void RecordPrimary(Func&& func) {
        CheckOwner(__builtin_return_address(0));
        if (!recording_threaded || direct_mode) {
            func(current_cmdbuf);
            return;
        }
        if (TlgToggle::Disabled(TlgToggle::ThreadedRecording)) {
            SyncRecording();
            direct_mode = true;
            func(current_cmdbuf);
            return;
        }
        if (!record_chunk->Push(std::forward<Func>(func))) {
            full_chunks.push_back(std::move(record_chunk));
            record_chunk = AcquireChunk();
            const bool pushed = record_chunk->Push(std::forward<Func>(func));
            ASSERT(pushed);
        }
    }

    /// tlg: true when commands passed to Record() normally go to the Vulkan recording thread
    /// (it may still record some directly, after a CommandBuffer() call).
    [[nodiscard]] bool RecordsOnThread() const noexcept {
        return recording_threaded && !TlgToggle::Disabled(TlgToggle::ThreadedRecording);
    }

    /// True when Record() defers commands (and RecordData() copies into chunks).
    [[nodiscard]] bool IsRecordingDeferred() const noexcept {
        return recording_threaded && !direct_mode &&
               !TlgToggle::Disabled(TlgToggle::ThreadedRecording);
    }

    /// Makes room for `bytes` of RecordData() plus the command that uses them in the current
    /// chunk: data and command must share a chunk, which is recycled once executed.
    void ReserveRecordData(size_t bytes) {
        CheckOwner(__builtin_return_address(0));
        if (!IsRecordingDeferred()) {
            return;
        }
        if (bytes + 1024 > RecordChunk::Capacity) {
            // Too large for a chunk (The Last Guardian issues copies with thousands of regions):
            // record directly until the next kick. RecordData() then returns the caller's span,
            // which outlives the command because Record() runs it immediately in direct mode.
            SyncRecording();
            direct_mode = true;
            direct_recordings.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (RecordChunk::Capacity - record_chunk->Size() < bytes + 1024) {
            full_chunks.push_back(std::move(record_chunk));
            record_chunk = AcquireChunk();
        }
    }

    /// Copies `data` into recording storage that lives until the command that uses it has
    /// been recorded (the same chunk). With threaded recording off, returns `data` itself.
    template <typename T>
    std::span<const T> RecordData(std::span<const T> data) {
        if (!IsRecordingDeferred() || data.empty()) {
            return data;
        }
        // Room for the data and the command that follows it, so both stay in one chunk.
        const size_t bytes = data.size_bytes();
        ReserveRecordData(bytes + alignof(T));
        if (!IsRecordingDeferred()) {
            return data; // ReserveRecordData switched to direct recording (oversized data)
        }
        auto* dst = static_cast<T*>(record_chunk->Allocate(bytes, alignof(T)));
        std::memcpy(dst, data.data(), bytes);
        return {dst, data.size()};
    }

    /// Hands recorded commands to the recording thread. Called at points where no caller holds
    /// the raw command buffer (end of draws and dispatches); small batches are kept.
    void KickRecording(bool force = false);

    /// Waits until every recorded command is in the command buffer.
    void SyncRecording();

    /// tlg: guest memory copies on the recording thread (small ones, RecordHostCopy) or the
    /// copy threads (TlgCopy::Async) must be done before the guest learns the GPU is past them
    /// (it may then rewrite the memory, e.g. UI vertices: flickering) and before the
    /// submission that reads them. Waits for all.
    void WaitHostCopies();
    static bool HostCopyRangesEnabled();
    void NoteHostCopySource(u64 address, u64 size);
    void WaitHostCopiesFor(u64 address, u64 size);

    /// Runs `copy` on the recording thread in order with the commands (the thread spins for
    /// work anyway, so small copies cost no wakeup); WaitHostCopies() covers it.
    template <typename Func>
    void RecordHostCopy(Func&& copy) {
        CheckOwner(__builtin_return_address(0));
        Record([copy = std::forward<Func>(copy), this, lane = parallel_lane,
                seq = ++host_copies_issued](vk::CommandBuffer) mutable {
            AfterRecordingPrefix(lane, [copy = std::move(copy), this, seq] {
                copy();
                host_copies_done.store(seq, std::memory_order_release);
            });
        });
    }

    /// tlg: runs `signal` once the guest memory copies issued so far are done, without
    /// waiting here: the recording thread reaches it after the copies queued before it and hands
    /// it to the copy threads' completion (TlgCopy::AfterCopies).
    void SignalAfterHostCopies(std::function<void()> signal);

    /// tlg: before a guest-visible write that is not deferred (WriteData, end-of-shader
    /// fences, flip): waits until every signal handed to SignalAfterHostCopies ran, so that
    /// the write cannot overtake an earlier fence. Otherwise the guest, seeing the later value,
    /// may free memory the earlier fence then writes into (corrupted heap, guest fault).
    void WaitDeferredSignals();

    /// tlg: whether a render pass is open.
    [[nodiscard]] bool IsRendering() const noexcept {
        return is_rendering;
    }
    [[nodiscard]] bool IsNativeSecondaryRendering() const noexcept {
        return native_secondary_current_scope;
    }

    /// tlg: see RenderingObserver (one at a time; nullptr removes it).
    void SetRenderingObserver(RenderingObserver* observer) noexcept {
        rendering_observer = observer;
    }

    /// Whether a render pass with exactly this state is open.
    [[nodiscard]] bool IsRenderingWith(const RenderState& state) const {
        return is_rendering && render_state == state;
    }

    /// tlg: counts a draw or dispatch recorded since the last submission.
    void CountWork() noexcept {
        work_since_submit.fetch_add(1, std::memory_order_relaxed);
    }

    /// tlg: draws and dispatches recorded since the last submission.
    [[nodiscard]] u32 WorkSinceSubmit() const noexcept {
        return work_since_submit.load(std::memory_order_relaxed);
    }

    /// Whether BeginRendering(state) would start a new render pass.
    [[nodiscard]] bool WillBeginRendering(const RenderState& state) const {
        return !(is_rendering && render_state == state);
    }

    /// CommandBuffer() calls that waited for a recording thread (TLG_FRAME_STATS).
    static inline std::atomic<u64> direct_recordings{0};

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return work_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (work_semaphore.IsFree(tick)) {
            return true;
        }
        work_semaphore.Refresh();
        return work_semaphore.IsFree(tick);
    }

    /// Returns the scheduler timeline semaphore.
    [[nodiscard]] Semaphore* GetWorkSemaphore() noexcept {
        return &work_semaphore;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
        pending_ops.emplace(std::move(func), CurrentTick());
        num_pending_ops.fetch_add(1, std::memory_order_release);
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
        }
        priority_pending_ops_cv.notify_one();
    }

    static std::mutex submit_mutex;

private:
    void AllocateWorkerCommandBuffers();

    std::unique_ptr<RecordChunk> AcquireChunk();

    void RecorderThread(std::stop_token stoken);
    void HandOverParallel();
    void SplitRecording();
    template <typename Func>
    void AfterRecordingPrefix(unsigned lane, Func&& task) {
        if (parallel_recording) {
            parallel_recording->AfterPrefix(lane, std::forward<Func>(task));
        } else {
            task();
        }
    }
    void BeginParallelBatch();
    std::span<const vk::CommandBuffer> EndRecordingBuffers();

    void SubmitExecution(SubmitInfo& info);

    void PriorityPendingOpsThread(std::stop_token stoken);

private:
    const Instance& instance;
    Semaphore work_semaphore;
    CommandPool command_pool;
    std::unique_ptr<CommandPool> parallel_pool;
    struct NativeSecondaryScope {
        vk::CommandBuffer buffer{};
        bool active = false;
    };
    std::array<NativeSecondaryScope, 2> native_secondary_scopes{};
    std::array<std::unique_ptr<CommandPool>, 2> native_secondary_pools{};
    bool native_secondary_enabled = false;
    bool native_secondary_current_scope = false;
    std::unique_ptr<ParallelRecordQueue<RecordChunk, vk::CommandBuffer>> parallel_recording;
    unsigned parallel_lane = 0;
    size_t parallel_bytes = 0;
    size_t parallel_target = 256 * 1024;
    std::array<vk::CommandBuffer, 2> submission_buffers{};
    DynamicState dynamic_state;
    bool host_read_barrier = false;
    vk::PipelineStageFlags2 host_read_src_stages{};
    vk::AccessFlags2 host_read_src_access{};
    VertexInputState vertex_input_state;
    SubmitFunc on_submit{};
    vk::CommandBuffer current_cmdbuf;
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    u32 pending_polls = 0; // tlg
    bool popping_pending_ops = false; ///< tlg: guarded by pending_ops_mutex (recursive)
    std::atomic<u32> num_pending_ops{0}; ///< tlg: pending_ops.size(), checked without the lock
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
    RenderState render_state;
    bool is_rendering = false;
    RenderingObserver* rendering_observer = nullptr; ///< tlg
    // tlg: threaded recording
    std::unique_ptr<RecordChunk> record_chunk;
    void (*owner_hook)(void*, void*) = nullptr;
    void* owner_context = nullptr;

    [[gnu::always_inline]] void CheckOwner(void* caller) {
        if (owner_hook && !DrawPipe::OnStageB()) [[unlikely]] {
            owner_hook(owner_context, caller);
        }
    }
    std::vector<std::unique_ptr<RecordChunk>> full_chunks;
    std::mutex recorder_mutex;
    std::condition_variable_any recorder_cv;
    std::condition_variable_any recorder_idle_cv;
    std::deque<std::unique_ptr<RecordChunk>> recorder_queue;
    std::vector<std::unique_ptr<RecordChunk>> free_chunks;
    bool recorder_busy = false;
    std::atomic<size_t> queued_chunks{0}; ///< recorder_queue.size() for lock-free polling
    bool recorder_sleeping = false; ///< waiting on recorder_cv (guarded by recorder_mutex)
    std::atomic<u32> work_since_submit{0}; ///< tlg: CountWork(), reset by each submission
    bool direct_mode = false; ///< the command buffer is recorded on the caller's thread
    u64 host_copies_issued = 0;
    HostCopySources host_copy_sources;
    std::atomic<u64> host_copies_done{0};
    std::atomic<u64> deferred_signals_issued{0}; ///< by the thread recording (A or B)
    std::shared_ptr<std::atomic<u64>> deferred_signals_done =
        std::make_shared<std::atomic<u64>>(0);
    // tlg: whether recorder_thread runs, read on every Record(). std::jthread::joinable()
    // compares thread ids, which on Windows (libc++ over winpthreads) costs two system calls.
    bool recording_threaded{false};
    std::jthread recorder_thread;
    tracy::VkCtxScope* profiler_scope{};
};

} // namespace Vulkan
