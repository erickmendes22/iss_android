// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: camera motion vectors for temporal upscaling (docs/upscaler.md). The scene constants
// give the camera of each frame; the previous one is kept here. TLG_DEBUG_MOTION=1 blends the
// motion vectors as colors into the frame before it is copied to the display.

#pragma once

#include <array>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class ObjectMotion;

class CameraMotion {
public:
    CameraMotion(const Instance& instance, Scheduler& scheduler,
                 VideoCore::TextureCache& texture_cache, Runtime& runtime);
    ~CameraMotion();
    void InvalidateHistory() {
        current.valid = previous.valid = false;
#ifdef ISS_ANDROID_BIONIC
        native_previous_valid = native_motion_valid = false;
        native_frame_prepared = false;
#endif
    }
    void SetObjectMotion(ObjectMotion* motion) noexcept {
        object_motion = motion;
    }
    /// This frame's object motion image (after RecordMotion), for the debug view; or null.
    vk::ImageView ObjectMotionView() const noexcept;
    vk::Image ObjectMotionImage(u32 width, u32 height) const noexcept;

    void SetJitter(std::array<float, 2> value) noexcept {
        previous_jitter = jitter;
        jitter = value;
    }

    [[nodiscard]] bool Enabled() const noexcept {
        return debug_overlay || for_upscaler;
    }

    /// Both cameras and the scene depth of the current frame are known.
    [[nodiscard]] bool Ready() const noexcept {
        return current.valid && previous.valid && depth_id;
    }
#ifdef ISS_ANDROID_BIONIC
    // ISS may have per-pixel object motion before the game's camera layout is known.
    // This is deliberately separate from Ready(): object vectors do not make a valid camera.
    [[nodiscard]] bool IssObjectMotionReady(u32 width, u32 height) const noexcept {
        return depth_id && bool(ObjectMotionImage(width, height));
    }
    // Latch the completed game-camera matrix and the preceding scene-frame
    // matrix BEFORE the display pass starts the temporal upscale.
    void PrepareIssNativeCamera();
    [[nodiscard]] bool IssNativeCameraReady() const noexcept {
        return native_motion_valid && Ready();
    }
    [[nodiscard]] float IssNativeFar() const noexcept { return native_far; }
    // inFAMOUS already offsets its scene projection by a native 2-phase TAA jitter.
    // The FSR dispatch must report that displacement as well as the host viewport
    // jitter, or temporal accumulation samples the wrong subpixel positions.
    // Native projection indices 8/9 are NDC shifts: x right, y up.
    [[nodiscard]] std::array<float, 2> IssNativeJitterPixels(u32 width, u32 height) const noexcept {
        if (!native_previous_valid || !frame_has_camera) return {};
        return {native_previous_jitter_ndc[0] * (0.5f * float(width)),
                -native_previous_jitter_ndc[1] * (0.5f * float(height))};
    }
#endif
    [[nodiscard]] VideoCore::ImageId Depth() const noexcept {
        return depth_id;
    }
    /// The render size in the scene constants (the game's viewport; its targets may be
    /// allocated larger, e.g. 1916x1080 for a 1916x1078 scene), or zero before the first camera.
    [[nodiscard]] std::array<u32, 2> RenderSize() const noexcept {
        return render_size;
    }
    /// Vertical field of view and near/far planes of the current camera.
    [[nodiscard]] float VerticalFov() const noexcept;
    [[nodiscard]] float Near() const noexcept;
    // Current projection, previous projection, and row Z of previous-view * inverse-view.
    [[nodiscard]] std::array<std::array<float, 4>, 3> TaaDepthParameters() const noexcept;

    /// Records the motion vector pass: `depth_view` (depth aspect, General layout) into
    /// `motion_view` (RG16F storage, General), pixels, previous minus current.
    void RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view, vk::ImageView motion_view,
                      u32 width, u32 height);

    /// A bound constant buffer of 864 bytes: checks the scene constant signature.
    void OnConstants(const float* data);
#ifdef ISS_ANDROID_BIONIC
    // Capture only a handful of distinct source-buffer layouts, once per gameplay session.
    // Never copy from guest pointers: callers supply the CPU-owned constant ring.
    [[nodiscard]] bool NeedGBufferSamples() const noexcept {
        return for_upscaler && !android_binding_samples_done &&
               android_binding_sample_count < android_binding_samples.size();
    }
    void ObserveGBufferBinding(u64 stage_hash, u32 slot, u32 bytes, const void* ring_copy);
#endif

    /// The G-buffer pass (5+ color targets): its depth is the scene depth.
    void OnGBufferPass(VideoCore::ImageId depth);

    /// The pass copying the finished frame (`frame`, the last target drawn) to the display: the
    /// frame's depth and camera are complete. Records the debug overlay, starts a new frame.
    void OnDisplayPass(VideoCore::ImageId frame);

private:
    struct Camera {
        std::array<float, 12> view{};     ///< world to view, 3x4 rows
        std::array<float, 12> inv_view{}; ///< view to world, 3x4 rows
        std::array<float, 4> proj{};      ///< x scale, y scale, z scale, z offset
        bool valid = false;
    };

    void Overlay(VideoCore::ImageId frame);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    bool debug_overlay = false;
    bool for_upscaler = false;
    vk::UniqueDescriptorSetLayout motion_desc_layout;
    vk::UniquePipelineLayout motion_pipeline_layout;
    vk::UniquePipeline motion_pipeline;

    ObjectMotion* object_motion = nullptr;
    Camera current, previous;
#ifdef ISS_ANDROID_BIONIC
    // Counters identify which inFAMOUS camera-signature prerequisite is absent.
    u32 android_probe_frames = 0;
    u32 android_probe_gbuffer = 0;
    u32 android_probe_constants = 0;
    u32 android_probe_signature = 0;
    u32 android_probe_ready = 0;
    // Independent snapshots from the game's native camera update; never set
    // current.valid here without depth/frame correspondence validation.
    u64 android_camera_last_sequence = 0;
    u32 android_camera_log_count = 0;
    u32 android_camera_frame_updates = 0;
    // Native inFAMOUS camera matrices are x86 row-major 4x4 float matrices,
    // copied from the verified guest update; no guest pointers retained.
    std::array<float, 16> native_previous_composite{};
    std::array<float, 16> native_reprojection{};
    // Game-native projection's subpixel NDC shift (row-vector, row 2).
    // The guest's built-in jitter must not appear as camera-object motion.
    std::array<float, 2> native_previous_jitter_ndc{};
    std::array<float, 2> native_jitter_delta_ndc{};
    u64 native_previous_object = 0;
    u64 native_previous_sequence = 0;
    bool native_previous_valid = false;
    bool native_motion_valid = false;
    bool native_frame_prepared = false;
    float native_near = 10.0f;
    float native_far = 10000000.0f;
    float native_vertical_fov = 1.0f;
    u32 native_frames_valid = 0;
    u32 native_frames_rejected = 0;
    u32 native_frames_repeated = 0;
    u32 native_reject_missing = 0;
    u32 native_reject_matrix = 0;
    u32 native_reject_jitter = 0;
    u32 native_reject_seed = 0;
    u32 native_reject_switch = 0;
    u32 native_reject_gap = 0;
    u32 native_reject_transform = 0;
    struct BindingSample {
        u64 shader_hash = 0;
        u32 slot = 0;
        u32 size = 0;
        std::array<float, 64> first{};
    };
    std::array<BindingSample, 24> android_binding_samples{};
    u32 android_binding_sample_count = 0;
    bool android_binding_samples_done = false;
#endif
    bool frame_has_camera = false;
    std::array<float, 2> jitter{}, previous_jitter{};
    std::array<u32, 2> render_size{};
    VideoCore::ImageId depth_id{};

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline overlay_pipeline;
    /// Debug: the last two finished frames (RGBA8 packed), for the reprojection check.
    std::unique_ptr<VideoCore::Buffer> frames[2];
    u32 frame_index = 0;
};

} // namespace Vulkan
