// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_camera_motion.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "tlg_toggles.h"
#ifdef ISS_ANDROID_BIONIC
#include "../../../tlggpu.h"
#endif

#include "video_core/host_shaders/camera_motion_comp.h"
#include "video_core/host_shaders/camera_motion_debug_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_object_motion.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct PushConstants {
    std::array<float, 12> reproject;
    std::array<float, 4> proj;
    std::array<float, 4> prev_proj;
    std::array<float, 2> size;
    std::array<float, 2> jitter;
    std::array<float, 2> previous_jitter;
    u32 mode;
};

/// a * b for 3x4 affine matrices (rows [R | t]).
std::array<float, 12> Multiply(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            float v = c == 3 ? a[r * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[r * 4 + k] * b[k * 4 + c];
            }
            out[r * 4 + c] = v;
        }
    }
    return out;
}

} // namespace

CameraMotion::CameraMotion(const Instance& instance_, Scheduler& scheduler_,
                           VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_} {
    const char* env = std::getenv("TLG_DEBUG_MOTION");
    debug_overlay = env && env[0] == '1';
    const char* upscaler = std::getenv("TLG_UPSCALER");
    // The upscaler can be switched on from the menu at any time: the camera is always tracked
    // unless TLG_UPSCALER=none.
    for_upscaler = !(upscaler && std::strcmp(upscaler, "none") == 0);
    const auto device = instance.GetDevice();
    if (for_upscaler) {
        const std::array<vk::DescriptorSetLayoutBinding, 3> motion_bindings = {{
            {.binding = 0,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
            {.binding = 2,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .descriptorCount = 1,
             .stageFlags = vk::ShaderStageFlagBits::eCompute},
        }};
        motion_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(motion_bindings.size()),
            .pBindings = motion_bindings.data(),
        }));
        const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                          .offset = 0,
                                          .size = sizeof(PushConstants)};
        motion_pipeline_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*motion_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        }));
        const auto motion_module = CompileSPV(CAMERA_MOTION_COMP, device);
        motion_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = motion_module,
                              .pName = "main"},
                    .layout = *motion_pipeline_layout,
                }));
        device.destroyShaderModule(motion_module);
    }
    if (!debug_overlay) {
        return;
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 3,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    }));
    const auto module = CompileSPV(CAMERA_MOTION_DEBUG_COMP, device);
    overlay_pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
    for (auto& frame : frames) {
        frame = std::make_unique<VideoCore::Buffer>(instance, 0, 3840ull * 2160 * 4,
                                                    VideoCore::MemoryType::DeviceLocal);
    }
    std::printf("GPU: camera motion debug overlay on\n");
}

CameraMotion::~CameraMotion() = default;

float CameraMotion::VerticalFov() const noexcept {
#ifdef ISS_ANDROID_BIONIC
    if (native_motion_valid) return native_vertical_fov;
#endif
    return 2.0f * std::atan(1.0f / current.proj[1]);
}

float CameraMotion::Near() const noexcept {
#ifdef ISS_ANDROID_BIONIC
    if (native_motion_valid) return native_near;
#endif
    // depth = zs + zo / z is 0 at the near plane.
    return -current.proj[3] / current.proj[2];
}

std::array<std::array<float, 4>, 3> CameraMotion::TaaDepthParameters() const noexcept {
    const auto transform = Multiply(previous.view, current.inv_view);
    return {current.proj, previous.proj,
            {transform[8], transform[9], transform[10], transform[11]}};
}

vk::ImageView CameraMotion::ObjectMotionView() const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->View() : vk::ImageView{};
}

vk::Image CameraMotion::ObjectMotionImage(u32 width, u32 height) const noexcept {
    return object_motion && object_motion->Enabled() ? object_motion->Image(width, height)
                                                      : vk::Image{};
}

#ifdef ISS_ANDROID_BIONIC
void CameraMotion::PrepareIssNativeCamera() {
    // The fullscreen upscale consumes this game's native scene depth.
    // Use the camera copied with these submitted commands. Reading the live
    // CPU publisher here can select an update from a later queued frame.
    // The AA marker and RunScaled both prepare the camera. A newer CPU update
    // between those calls must not replace the camera pair within one GPU frame.
    if (!for_upscaler || !depth_id || native_frame_prepared) return;
    native_frame_prepared = true;
    const TlgIssCameraSnapshot sample = native_submission_camera;
    const u64 sequence = sample.sequence;
    if (!sequence) {
        native_previous_valid = native_motion_valid = false;
        current.valid = previous.valid = false;
        ++native_frames_rejected;
        ++native_reject_missing;
        return;
    }
    if (sequence == native_previous_sequence) {
        // Same validated guest camera in a NEW rendered frame: the camera did
        // not move. Do not replay the preceding frame's transform or let the
        // display boundary discard this anchor. Never revive an invalidated
        // pair after a menu/settings change with an old sample.
        if (native_previous_valid) {
            native_reprojection = {};
            for (int i = 0; i < 16; i += 5) native_reprojection[i] = 1.0f;
            native_jitter_delta_ndc = {};
            native_motion_valid = current.valid = previous.valid = true;
            frame_has_camera = true;
            ++native_frames_valid;
            ++native_frames_repeated;
        } else {
            native_motion_valid = current.valid = previous.valid = false;
            ++native_frames_rejected;
            ++native_reject_seed;
        }
        return;
    }
    const bool scalars_ok =
        std::isfinite(sample.near_plane) && sample.near_plane > 0.0f &&
        std::isfinite(sample.far_plane) && sample.far_plane > sample.near_plane &&
        std::isfinite(sample.vertical_fov) && sample.vertical_fov > 0.1f &&
        sample.vertical_fov < 3.14f &&
        std::isfinite(sample.aspect) && sample.aspect > 0.2f && sample.aspect < 8.0f;
    bool inverse_ok = scalars_ok && sample.guest_object != 0;
    if (inverse_ok) {
        // Verified against an actual CUSA00004 camera dump: the matrices at
        // +0x430 and +0x4b0 multiply to identity in ROW-MAJOR order.
        for (int row = 0; row < 4 && inverse_ok; ++row) {
            for (int col = 0; col < 4; ++col) {
                double value = 0.0;
                for (int k = 0; k < 4; ++k) {
                    const float a = sample.composite[row * 4 + k];
                    const float b = sample.inverse[k * 4 + col];
                    if (!std::isfinite(a) || !std::isfinite(b)) {
                        inverse_ok = false;
                        break;
                    }
                    value += double(a) * double(b);
                }
                if (!inverse_ok || !std::isfinite(value) ||
                    std::abs(value - (row == col ? 1.0 : 0.0)) > 0.02) {
                    inverse_ok = false;
                    break;
                }
            }
        }
    }
    if (!inverse_ok) {
        native_previous_valid = native_motion_valid = false;
        current.valid = previous.valid = false;
        ++native_frames_rejected;
        ++native_reject_matrix;
        return;
    }

    // Native projection row 2 also contains the game's own subpixel TAA
    // jitter. The Android rasterizer applies a SECOND viewport jitter for FSR.
    // Motion vectors must exclude the guest jitter step between frames;
    // otherwise a completely stationary camera creates phantom half-pixel
    // movement (measured in ISS log 9).
    const std::array<float, 2> guest_jitter_ndc{
        sample.projection[8], sample.projection[9]};
    const bool guest_jitter_ok = std::isfinite(guest_jitter_ndc[0]) &&
                                 std::isfinite(guest_jitter_ndc[1]) &&
                                 std::abs(guest_jitter_ndc[0]) < 0.02f &&
                                 std::abs(guest_jitter_ndc[1]) < 0.02f;
    if (!guest_jitter_ok) {
        native_previous_valid = native_motion_valid = false;
        current.valid = previous.valid = false;
        ++native_frames_rejected;
        ++native_reject_jitter;
        return;
    }
    native_jitter_delta_ndc = native_previous_valid
        ? std::array<float, 2>{
              native_previous_jitter_ndc[0] - guest_jitter_ndc[0],
              native_previous_jitter_ndc[1] - guest_jitter_ndc[1]}
        : std::array<float, 2>{};

    const bool same_camera = native_previous_valid &&
                             native_previous_object == sample.guest_object;
    // Frame IDs advance by several camera-update calls per actual scene frame.
    // Large gaps (menu/loading/pause) must reset temporal history.
    const bool consecutive = native_previous_sequence &&
                             sequence > native_previous_sequence &&
                             sequence - native_previous_sequence <= 64;
    native_motion_valid = same_camera && consecutive;
    if (!native_previous_valid) ++native_reject_seed;
    else if (!same_camera) ++native_reject_switch;
    else if (!consecutive) ++native_reject_gap;
    if (native_motion_valid) {
        // PS4/inFAMOUS stores row-vector camera matrices. For a homogeneous
        // CURRENT clip-space row vector, the PREVIOUS clip coordinate is:
        //   clip_previous = clip_current * inverse(current) * composite(previous)
        // The GLSL shader instead computes dot(row, column_vector), so upload
        // the TRANSPOSE of this product. The earlier previous*inverse product
        // was mathematically incorrect despite passing camera-inverse checks.
        // For a captured static-camera jitter pair the correct transform has
        // about (+0.00104,-0.00185) XY translation in its last row; the old
        // product wrongly placed motion in the Z-dependent matrix entries.
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                double value = 0.0;
                for (int k = 0; k < 4; ++k) {
                    // Upload transpose(inv(current) * composite(previous)).
                    // C[row,col] = sum_k inv(current)[col,k] * prev[k,row].
                    // The old indices computed transpose(prev * inv(current)),
                    // leaving camera reprojected vectors near zero during motion.
                    value += double(sample.inverse[col * 4 + k]) *
                             double(native_previous_composite[k * 4 + row]);
                }
                if (!std::isfinite(value) || std::abs(value) > 1.0e6) {
                    native_motion_valid = false;
                    ++native_reject_transform;
                    break;
                }
                native_reprojection[row * 4 + col] = float(value);
            }
            if (!native_motion_valid) break;
        }
    }
    std::memcpy(native_previous_composite.data(), sample.composite,
                sizeof(sample.composite));
    native_previous_object = sample.guest_object;
    native_previous_sequence = sequence;
    native_previous_jitter_ndc = guest_jitter_ndc;
    native_previous_valid = true;
    native_near = sample.near_plane;
    native_far = sample.far_plane;
    native_vertical_fov = sample.vertical_fov;

    // A verified native camera sample is a valid frame anchor even when
    // the PREVIOUS frame is not yet available. The first sample seeds the
    // temporal history; it is intentionally NOT ready for reprojection.
    // Without this flag OnDisplayPass calls InvalidateHistory(), destroys the
    // seed every frame, and native_motion_valid can never become true.
    frame_has_camera = true;
    if (native_motion_valid) {
        previous.valid = current.valid = true;
        ++native_frames_valid;
    } else {
        previous.valid = current.valid = false;
        ++native_frames_rejected;
    }
}
#endif

void CameraMotion::RecordMotion(vk::CommandBuffer cmdbuf, vk::ImageView depth_view,
                                vk::ImageView motion_view, u32 width, u32 height) {
    bool object_valid = false;
    const vk::ImageView object_view = object_motion && object_motion->Enabled()
        ? object_motion->PrepareRead(cmdbuf, width, height, object_valid) : depth_view;
#ifdef ISS_ANDROID_BIONIC
    const bool object_only = !Ready() && object_valid;
    if (object_only) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::printf("FSR3 ISS: using depth-matched object motion without camera "
                        "(experimental, uncovered pixels use zero vectors)\n");
        }
    }
#endif
    PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = current.proj,
        .prev_proj = previous.proj,
        .size = {float(width), float(height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
#ifdef ISS_ANDROID_BIONIC
        .mode = native_motion_valid ? (object_valid ? 3u : 4u) :
                (object_only ? 2u : (object_valid ? 1u : 0u)),
#else
        .mode = object_valid ? 1u : 0u,
#endif
    };
#ifdef ISS_ANDROID_BIONIC
    if (native_motion_valid) {
        // Reuse existing 80-byte push-constant space for the full 4x4
        // current-clip -> previous-clip matrix. No larger Vulkan push range.
        std::memcpy(push.reproject.data(), native_reprojection.data(),
                    12 * sizeof(float));
        std::memcpy(push.proj.data(), native_reprojection.data() + 12,
                    4 * sizeof(float));
        // The previous_jitter slot is unused by the normal FSR motion
        // shader. In native mode it carries the guest's PREV-CURRENT
        // projection jitter delta, converted from NDC into render pixels.
        push.previous_jitter = {
            native_jitter_delta_ndc[0] * (float(width) * 0.5f),
            -native_jitter_delta_ndc[1] * (float(height) * 0.5f)};
    }
#endif
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = motion_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo object_info{.imageView = object_view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 3> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &motion_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &object_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *motion_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *motion_pipeline_layout, 0,
                                writes);
    cmdbuf.pushConstants(*motion_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                         sizeof(push), &push);
    cmdbuf.dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

#ifdef ISS_ANDROID_BIONIC
void CameraMotion::ObserveGBufferBinding(u64 hash, u32 slot, u32 size, const void* bytes) {
    if (!NeedGBufferSamples() || !bytes || size < 24 || size > 65536) return;
    for (u32 i = 0; i < android_binding_sample_count; ++i) {
        const auto& item = android_binding_samples[i];
        if (item.shader_hash == hash && item.size == size) return;
    }
    auto& item = android_binding_samples[android_binding_sample_count++];
    item.shader_hash = hash;
    item.slot = slot;
    item.size = size;
    // A source buffer can be smaller than 128 bytes; never read beyond its size.
    std::memcpy(item.first.data(), bytes,
                std::min<u32>(size, static_cast<u32>(sizeof(item.first))));
}
#endif

void CameraMotion::OnConstants(const float* data) {
#ifdef ISS_ANDROID_BIONIC
    ++android_probe_constants;
#endif
    // Scene constants: far plane 3000, 1/far, and the render size.
    if (data[0] != 3000.0f || data[4] < 64.0f || data[5] < 64.0f ||
        std::abs(data[1] * data[0] - 1.0f) > 1e-3f) {
        return;
    }
#ifdef ISS_ANDROID_BIONIC
    ++android_probe_signature;
#endif
    if (frame_has_camera) {
        return; // the first one of a frame is the main camera
    }
    previous = current;
    std::memcpy(current.view.data(), data + 8, 12 * sizeof(float));
    std::memcpy(current.inv_view.data(), data + 180, 12 * sizeof(float));
    current.proj = {data[52], data[57], data[62], data[63]};
    current.valid = current.proj[0] != 0.0f && current.proj[1] != 0.0f;
    const std::array<u32, 2> size{u32(data[4]), u32(data[5])};
    if (size != render_size) {
        std::printf("Camera motion: scene render size %ux%u\n", size[0], size[1]);
        render_size = size;
    }
    frame_has_camera = true;
}

void CameraMotion::OnGBufferPass(VideoCore::ImageId depth) {
#ifdef ISS_ANDROID_BIONIC
    ++android_probe_gbuffer;
#endif
    depth_id = depth;
}

void CameraMotion::OnDisplayPass(VideoCore::ImageId frame) {
#ifdef ISS_ANDROID_BIONIC
    if (for_upscaler) {
        // Report the ordered submission snapshot, not a newer CPU publication.
        const TlgIssCameraSnapshot sample = native_submission_camera;
        const u64 sequence = sample.sequence;
        if (sequence && sequence != android_camera_last_sequence) {
            const u64 update_delta = sequence - android_camera_last_sequence;
            android_camera_last_sequence = sequence;
            ++android_camera_frame_updates;
            const bool finite = std::isfinite(sample.near_plane) &&
                std::isfinite(sample.far_plane) &&
                std::isfinite(sample.vertical_fov) && std::isfinite(sample.aspect);
            const bool plausible = finite && sample.near_plane > 0 &&
                sample.far_plane > sample.near_plane && sample.vertical_fov > 0.1f &&
                sample.vertical_fov < 3.14f && sample.aspect > 0.2f &&
                sample.aspect < 8.0f;
            const bool print_sample = android_camera_log_count < 3 ||
                (android_probe_frames == 179 && android_camera_log_count < 9);
            if (print_sample) {
                ++android_camera_log_count;
                std::printf("FSR3 ISS camera capture: seq=%llu delta=%llu object=0x%llx "
                            "depth=%u valid_scalars=%u near=%.9g far=%.9g "
                            "vfov=%.9g aspect=%.9g tan_h=%.9g tan_v=%.9g\n",
                            static_cast<unsigned long long>(sequence),
                            static_cast<unsigned long long>(update_delta),
                            static_cast<unsigned long long>(sample.guest_object),
                            unsigned(bool(depth_id)), unsigned(plausible),
                            sample.near_plane, sample.far_plane, sample.vertical_fov,
                            sample.aspect, sample.tangent_horizontal,
                            sample.tangent_vertical);
                const auto dump_matrix = [](const char* name, const float* matrix) {
                    for (u32 row = 0; row < 4; ++row) {
                        std::printf("FSR3 ISS camera %s row%u = %.9g, %.9g, %.9g, %.9g\n",
                                    name, row, matrix[row * 4], matrix[row * 4 + 1],
                                    matrix[row * 4 + 2], matrix[row * 4 + 3]);
                    }
                };
                dump_matrix("projection", sample.projection);
                dump_matrix("view_transform", sample.view_transform);
                dump_matrix("composite", sample.composite);
                dump_matrix("inverse", sample.inverse);
            }
        }
        if (android_probe_frames == 179) {
            std::printf("FSR3 ISS camera capture: %u frame-boundary updates / 180 frames, "
                        "last_sequence=%llu samples_logged=%u\n",
                        android_camera_frame_updates,
                        static_cast<unsigned long long>(android_camera_last_sequence),
                        android_camera_log_count);
            std::printf("FSR3 ISS native reprojection: frames_valid=%u rejected=%u "
                        "current_native=%u repeated=%u missing=%u matrix=%u jitter=%u "
                        "seed=%u camera_switch=%u sequence_gap=%u transform=%u\n",
                        native_frames_valid, native_frames_rejected,
                        unsigned(native_motion_valid), native_frames_repeated,
                        native_reject_missing, native_reject_matrix, native_reject_jitter,
                        native_reject_seed, native_reject_switch, native_reject_gap,
                        native_reject_transform);
            native_frames_valid = native_frames_rejected = 0;
            native_frames_repeated = native_reject_missing = native_reject_matrix = 0;
            native_reject_jitter = native_reject_seed = native_reject_switch = 0;
            native_reject_gap = native_reject_transform = 0;
            android_camera_frame_updates = 0;
        }

        if (Ready()) ++android_probe_ready;
        if (++android_probe_frames >= 180) {
            std::printf("FSR3 camera: frames=%u gbuffer=%u constants864=%u "
                        "signature3000=%u ready=%u\n",
                        android_probe_frames, android_probe_gbuffer,
                        android_probe_constants, android_probe_signature, android_probe_ready);
            android_probe_frames = android_probe_gbuffer = android_probe_constants = 0;
            android_probe_signature = android_probe_ready = 0;
            if (!android_binding_samples_done && android_binding_sample_count) {
                // One bounded, game-specific trace after the first observed G-buffer.
                // Unlike Bloodborne's 864-byte constants, ISS has no matching
                // signature; record actual bound lengths and a few copied floats.
                std::printf("FSR3 ISS binding layouts: %u distinct shader buffers\n",
                            android_binding_sample_count);
                for (u32 i = 0; i < android_binding_sample_count; ++i) {
                    const auto& v = android_binding_samples[i];
                    std::printf("FSR3 ISS CB: shader=%016llx slot=%u bytes=%u "
                                "f0..15=%g,%g,%g,%g | %g,%g,%g,%g | "
                                "%g,%g,%g,%g | %g,%g,%g,%g "
                                "f16..23=%g,%g,%g,%g,%g,%g,%g,%g\n",
                                static_cast<unsigned long long>(v.shader_hash), v.slot,
                                v.size, v.first[0], v.first[1], v.first[2], v.first[3],
                                v.first[4], v.first[5], v.first[6], v.first[7],
                                v.first[8], v.first[9], v.first[10], v.first[11],
                                v.first[12], v.first[13], v.first[14], v.first[15],
                                v.first[16], v.first[17], v.first[18], v.first[19],
                                v.first[20], v.first[21], v.first[22], v.first[23]);
                    std::printf("FSR3 ISS CB matrices: shader=%016llx slot=%u bytes=%u "
                                "f24..31=%g,%g,%g,%g,%g,%g,%g,%g "
                                "f48..63=%g,%g,%g,%g | %g,%g,%g,%g | "
                                "%g,%g,%g,%g | %g,%g,%g,%g\\n",
                                static_cast<unsigned long long>(v.shader_hash), v.slot, v.size,
                                v.first[24], v.first[25], v.first[26], v.first[27],
                                v.first[28], v.first[29], v.first[30], v.first[31],
                                v.first[48], v.first[49], v.first[50], v.first[51],
                                v.first[52], v.first[53], v.first[54], v.first[55],
                                v.first[56], v.first[57], v.first[58], v.first[59],
                                v.first[60], v.first[61], v.first[62], v.first[63]);
                }
                android_binding_samples_done = true;
            }
        }
    }
#endif
    if (debug_overlay && frame && depth_id && current.valid && previous.valid) {
        Overlay(frame);
    }
    if (!frame_has_camera) InvalidateHistory();
    frame_has_camera = false;
    depth_id = {};
#ifdef ISS_ANDROID_BIONIC
    native_frame_prepared = false;
#endif
}

void CameraMotion::Overlay(VideoCore::ImageId frame) {
    auto& depth = texture_cache.GetImage(depth_id);
    auto& color = texture_cache.GetImage(frame);
    const auto depth_format = depth.info.pixel_format;
    if ((depth_format != vk::Format::eD32Sfloat && depth_format != vk::Format::eD32SfloatS8Uint) ||
        color.info.pixel_format != vk::Format::eR8G8B8A8Unorm ||
        !(color.usage_flags & vk::ImageUsageFlagBits::eStorage) ||
        color.info.size.width != depth.info.size.width ||
        color.info.size.height != depth.info.size.height) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("Camera motion: overlay skipped (depth %s, frame %s %ux%u)\n",
                        vk::to_string(depth_format).c_str(),
                        vk::to_string(color.info.pixel_format).c_str(), color.info.size.width,
                        color.info.size.height);
        }
        return;
    }
    const auto device = instance.GetDevice();
    const auto depth_view = Check(device.createImageView({
        .image = vk::Image(depth.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = depth_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    const auto color_view = Check(device.createImageView({
        .image = vk::Image(color.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&color, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
    runtime.FlushBarriers();

    const PushConstants push{
        .reproject = Multiply(previous.view, current.inv_view),
        .proj = current.proj,
        .prev_proj = previous.proj,
        .size = {float(color.info.size.width), float(color.info.size.height)},
        .jitter = jitter,
        .previous_jitter = previous_jitter,
        .mode = TlgToggle::Disabled(1u << 20)   ? 1u
                : TlgToggle::Disabled(1u << 21) ? 2u
                : TlgToggle::Disabled(1u << 22) ? 3u
                : TlgToggle::Disabled(1u << 23) ? 4u
                                               : 0u,
    };
    static u32 log_counter = 0;
    if (++log_counter % 200 == 0) {
        const auto& m = push.reproject;
        std::printf("Camera motion: proj %g %g %g %g prev %g %g %g %g\n"
                    "  view  %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f | %8.4f %8.4f %8.4f %9.3f\n"
                    "  reproj %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f | %8.4f %8.4f %8.4f %9.4f\n"
                    "  depth %s %ux%u, frame %ux%u\n",
                    push.proj[0], push.proj[1], push.proj[2], push.proj[3], push.prev_proj[0],
                    push.prev_proj[1], push.prev_proj[2], push.prev_proj[3], current.view[0],
                    current.view[1], current.view[2], current.view[3], current.view[4],
                    current.view[5], current.view[6], current.view[7], current.view[8],
                    current.view[9], current.view[10], current.view[11], m[0], m[1], m[2], m[3],
                    m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                    vk::to_string(depth_format).c_str(), depth.info.size.width,
                    depth.info.size.height, color.info.size.width, color.info.size.height);
    }
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo color_info{.imageView = color_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const auto& prev_frame = *frames[frame_index ^ 1];
    const auto& next_frame = *frames[frame_index];
    frame_index ^= 1;
    const vk::DescriptorBufferInfo prev_info{prev_frame.Handle(), 0, prev_frame.SizeBytes()};
    const vk::DescriptorBufferInfo next_info{next_frame.Handle(), 0, next_frame.SizeBytes()};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &color_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &prev_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &next_info},
    }};
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *overlay_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    // The previous frame buffer was written by the last dispatch.
    const vk::MemoryBarrier2 frame_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &frame_barrier});
    cmdbuf.dispatch((color.info.size.width + 7) / 8, (color.info.size.height + 7) / 8, 1);

    scheduler.DeferOperation([device, depth_view, color_view] {
        device.destroyImageView(depth_view);
        device.destroyImageView(color_view);
    });
}

} // namespace Vulkan
