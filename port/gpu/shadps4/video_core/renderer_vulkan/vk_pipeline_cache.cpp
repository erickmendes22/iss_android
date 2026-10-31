// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <string_view>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <ranges>
#include <string>
#include <unordered_set>
#include <condition_variable>
#include <filesystem>
#include <thread>
#include <xxhash.h>
#include "common/backports.h"
#include "tlg_threads.h"
#include "tlg_region.h"

#include "common/elf_info.h"
#include "common/hash.h"
#include "common/thread.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/fragment_contraction.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/recompiler_toggles.h"
#include "shader_recompiler/runtime_info.h"
#include "tlg_settings.h"
#include "tlg_toggles.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "iss_bionic_perf.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_lighting_test.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

struct AsyncShaderCompilation {
    AsyncJob<std::shared_ptr<AsyncShaderCompilation>> job;
    Shader::Info info;
    Shader::RuntimeInfo runtime{};
    std::vector<u32> code, user_data, geometry_code, binary;
    std::unique_ptr<Shader::Pools> pools;
    CompilerPool<Shader::Pools>* pool_cache{};
    std::optional<Shader::IR::Program> ir;
    Shader::StageSpecialization spec;
    Shader::Gcn::FetchShaderSnapshot fetch;
    Shader::Backend::Bindings binding;
    vk::Device device;
    vk::ShaderModule module{};
    size_t permutation{};
    bool initial{};
    ~AsyncShaderCompilation() {
        ir.reset();
        if (pools) {
            pool_cache->Recycle(std::move(pools));
        }
        if (module) {
            device.destroyShaderModule(module);
        }
    }
};

struct AsyncGraphicsCompilation {
    AsyncJob<std::shared_ptr<AsyncGraphicsCompilation>> job;
    PipelineSelection selection;
    std::array<std::unique_ptr<Shader::Info>, MaxShaderStages> infos;
    std::array<std::vector<u32>, MaxShaderStages> user_data;
    std::array<Shader::Gcn::FetchShaderSnapshot, MaxShaderStages> fetch;
    std::array<const Shader::Info*, MaxShaderStages> live_infos;
    GraphicsPipeline::SerializationSupport data;
    std::unique_ptr<GraphicsPipeline> pipeline;
    bool failure_logged = false;
};

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

/// Logs shaders whose float atomic min/max run as integer atomics (no driver support): lets a
/// log tell whether a game depends on that emulation (fork shadPS4-Infamous-FixBuild 32851afe).
static void LogEmulatedFloatAtomics(const Shader::Info& info, const Shader::Profile& profile) {
    if ((info.uses_buffer_atomic_float_min_max && !profile.supports_buffer_fp32_atomic_min_max) ||
        (info.uses_image_atomic_float_min_max && !profile.supports_image_fp32_atomic_min_max)) {
        LOG_INFO(Render_Vulkan, "{} shader {:#x} does float atomic min/max with integer atomics",
                 info.hw_stage, info.pgm_hash);
    }
}

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

// Lowered user clip planes ride the same emulation path as guest-exported distances, so the
// fragment side arms whenever the hardware vertex stage lowers them, keeping its input locations in
// sync with the shifted vertex outputs.
static bool NeedsClipDistanceEmulation(const AmdGpu::Regs& regs, const Shader::Profile& profile) {
    const bool lowers_user_clip_planes =
        regs.clipper_control.user_clip_plane_enable &&
        !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
    return ((regs.vs_output_control.clip_distance_enable &&
             !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
            lowers_user_clip_planes) &&
           profile.needs_clip_distance_emulation;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        // tlg: RefreshGraphicsStages already skipped the draw when the copy shader has no
        // binary info yet; this only guards against it changing in between (no fatal stop).
        const auto params_vc = AmdGpu::TryGetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc ? params_vc->code : std::span<const u32>{};
        info.hw.gs.vs_copy_hash = params_vc ? params_vc->hash : 0;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        // tlg: BP12 read once when the pipeline is built (the shader cache key has its salt).
        info.hw.fs.depth_before_shader =
            Common::Backports::On(Common::Backports::Id::ShadPS4Pr5264) &&
            regs.depth_shader_control.depth_before_shader;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        info.hw.fs.clip_distance_emulation = NeedsClipDistanceEmulation(regs, profile);
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = sel.cs_regs ? *sel.cs_regs : liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        // tlg: BP11 (shadPS4 #5348)
        info.sw.vs.is_indirect = Common::Backports::On(Common::Backports::Id::FirstInstanceDouble) &&
                                 sel.draw_indirect_params.is_indirect;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

// tlg: BP08 (Mrsuss b972798, 0xCydral bdb9769). The driver's own pipeline cache (the compiled
// ISA behind every vkCreateGraphicsPipelines) is kept between sessions in
// <cache>/driver-<serial>/vk_pipeline_cache_<vendor>_<device>_<pipelineCacheUUID>.vkc. The key
// is in the file name, so a GPU or driver change finds no file and leaves the others alone:
// nothing is ever deleted (the next save of the same key replaces the file). The file is a
// header (magic, version, size, XXH3 of the blob) and the blob; it is written to a temporary
// name and renamed, so a crash leaves the old file or none. Independent of the shader cache
// folder, which is pruned when the recompiler changes; the driver checks the blob again.
namespace {
constexpr u32 DriverCacheMagic = 0x4B435649; // "IVCK"
constexpr u32 DriverCacheVersion = 1;
struct DriverCacheHeader {
    u32 magic;
    u32 version;
    u64 payload_size;
    u64 payload_hash;
};
static_assert(sizeof(DriverCacheHeader) == 24);

std::filesystem::path DriverCacheFile(const Instance& instance) {
    const std::string serial{Common::ElfInfo::Instance().GameSerial()};
    std::string uuid;
    for (const u8 byte : instance.GetPipelineCacheUUID()) {
        uuid += fmt::format("{:02x}", byte);
    }
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
           ("driver-" + (serial.empty() ? std::string{"unknown"} : serial)) /
           fmt::format("vk_pipeline_cache_{:04x}_{:04x}_{}.vkc", instance.GetVendorID(),
                       instance.GetDeviceID(), uuid);
}

/// The saved blob for this GPU and driver, empty when there is none or the file is damaged.
std::vector<u8> LoadDriverCacheBlob(const Instance& instance) {
    const auto path = DriverCacheFile(instance);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        std::printf("GPU: driver pipeline cache: none saved for this GPU/driver\n");
        return {};
    }
    std::vector<u8> file(std::filesystem::file_size(path, ec));
    if (ec || file.size() < sizeof(DriverCacheHeader) + sizeof(VkPipelineCacheHeaderVersionOne)) {
        return {};
    }
    {
        const Common::FS::IOFile io{path, Common::FS::FileAccessMode::Read};
        if (!io.IsOpen() || io.Read(file) != file.size()) {
            return {};
        }
    }
    DriverCacheHeader header;
    std::memcpy(&header, file.data(), sizeof(header));
    const u8* payload = file.data() + sizeof(header);
    if (header.magic != DriverCacheMagic || header.version != DriverCacheVersion ||
        header.payload_size != file.size() - sizeof(header) ||
        XXH3_64bits(payload, header.payload_size) != header.payload_hash) {
        std::printf("GPU: driver pipeline cache ignored (damaged file, kept)\n");
        return {};
    }
    VkPipelineCacheHeaderVersionOne driver;
    std::memcpy(&driver, payload, sizeof(driver));
    const auto uuid = instance.GetPipelineCacheUUID();
    if (driver.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        driver.vendorID != instance.GetVendorID() || driver.deviceID != instance.GetDeviceID() ||
        std::memcmp(driver.pipelineCacheUUID, uuid.data(), VK_UUID_SIZE) != 0) {
        std::printf("GPU: driver pipeline cache ignored (other GPU or driver, kept)\n");
        return {};
    }
    std::printf("GPU: driver pipeline cache loaded (%zu bytes)\n", size_t(header.payload_size));
    return {payload, payload + header.payload_size};
}
} // namespace

std::atomic<u64> g_tlg_driver_pipelines_created{0};

struct PipelineCache::DriverCacheSaver {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    u64 saved_hash = 0; ///< XXH3 of the last blob written (or loaded): unchanged data is not rewritten
    /// g_tlg_driver_pipelines_created at the last save: nothing new means nothing to merge, copy
    /// or hash (a whole-game blob is ~300 MB, and the save ran every minute regardless).
    u64 saved_created = ~0ull;
    bool over_limit_logged = false;
};

bool PipelineCache::SaveDriverCache() {
    // The save is optional (0xCydral 4f16e00): an allocation failure on the saver thread must not
    // end the game; it is skipped and tried again later.
    try {
        return SaveDriverCacheUnchecked();
    } catch (const std::exception& e) {
        LOG_WARNING(Render_Vulkan, "tlg: driver pipeline cache save skipped: {}", e.what());
        return false;
    }
}

bool PipelineCache::SaveDriverCacheUnchecked() {
    // Pipelines the compiler workers built are only merged into pipeline_cache when they finish
    // (FinishCompilations): merge copies of all of them into a scratch cache for the blob.
    const u64 created = g_tlg_driver_pipelines_created.load(std::memory_order_relaxed);
    if (driver_saver && created == driver_saver->saved_created) {
        return true;
    }
    const auto device = instance.GetDevice();
    auto [scratch_result, scratch] = device.createPipelineCacheUnique({});
    if (scratch_result != vk::Result::eSuccess) {
        return false;
    }
    std::vector<vk::PipelineCache> sources{*pipeline_cache};
    for (const auto& cache : worker_pipeline_caches) {
        sources.push_back(*cache);
    }
    if (device.mergePipelineCaches(*scratch, sources) != vk::Result::eSuccess) {
        return false;
    }
    auto [result, blob] = device.getPipelineCacheData(*scratch);
    if (result != vk::Result::eSuccess || blob.size() < sizeof(VkPipelineCacheHeaderVersionOne)) {
        return false;
    }
    // tlg: TLG_DRIVER_CACHE_MAX_MB (default 1024, 0 = no limit): a bigger blob is not written,
    // so the file never grows past it (the saved one keeps loading). Vulkan has no way to evict
    // entries from a pipeline cache; deleting the file starts it again.
    static const u64 max_bytes = [] {
        const char* v = std::getenv("TLG_DRIVER_CACHE_MAX_MB");
        return u64(v && *v ? std::max(0, std::atoi(v)) : 1024) << 20;
    }();
    if (max_bytes && blob.size() > max_bytes) {
        if (driver_saver && !driver_saver->over_limit_logged) {
            driver_saver->over_limit_logged = true;
            std::printf("GPU: driver pipeline cache not saved: %zu MB is over the %llu MB limit "
                        "(TLG_DRIVER_CACHE_MAX_MB)\n",
                        blob.size() >> 20, static_cast<unsigned long long>(max_bytes >> 20));
        }
        if (driver_saver) {
            driver_saver->saved_created = created;
        }
        return false;
    }
    const u64 hash = XXH3_64bits(blob.data(), blob.size());
    if (driver_saver && hash == driver_saver->saved_hash) {
        driver_saver->saved_created = created;
        return true;
    }
    const auto path = DriverCacheFile(instance);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temp = path;
    temp += ".tmp";
    const DriverCacheHeader header{DriverCacheMagic, DriverCacheVersion, blob.size(), hash};
    bool written = false;
    {
        const Common::FS::IOFile io{temp, Common::FS::FileAccessMode::Create};
        written = io.IsOpen() && io.WriteRaw<u8>(&header, sizeof(header)) == sizeof(header) &&
                  io.Write(blob) == blob.size();
    }
    if (written) {
        std::filesystem::rename(temp, path, ec);
    }
    if (!written || ec) {
        std::filesystem::remove(temp, ec);
        LOG_ERROR(Render_Vulkan, "tlg: driver pipeline cache not written to {}", path.string());
        return false;
    }
    if (driver_saver) {
        driver_saver->saved_hash = hash;
        driver_saver->saved_created = created;
    }
    LOG_INFO(Render_Vulkan, "tlg: driver pipeline cache saved ({} bytes)", blob.size());
    return true;
}

void PipelineCache::StartDriverCacheSaver() {
    // The process may end without destructors: the cache is also saved while the game runs.
    driver_saver = new DriverCacheSaver;
    driver_saver->thread = std::thread([this] {
        Common::SetCurrentThreadName("tlg:DriverCacheSave");
        auto* saver = driver_saver;
        std::unique_lock lock{saver->mutex};
        while (!saver->stop) {
            // tlg: TLG_DRIVER_CACHE_SAVE_S (default 60, was 180): a crash loses at most this much of what
            // the driver learned.
            static const int save_seconds = [] {
                const char* v = std::getenv("TLG_DRIVER_CACHE_SAVE_S");
                return v && *v ? std::clamp(std::atoi(v), 10, 3600) : 60;
            }();
            if (saver->cv.wait_for(lock, std::chrono::seconds(save_seconds), [&] { return saver->stop; })) {
                break;
            }
            lock.unlock();
            SaveDriverCache();
            lock.lock();
        }
    });
}

void PipelineCache::StopDriverCacheSaver() {
    if (!driver_saver) {
        return;
    }
    {
        std::scoped_lock lock{driver_saver->mutex};
        driver_saver->stop = true;
    }
    driver_saver->cv.notify_all();
    if (driver_saver->thread.joinable()) {
        driver_saver->thread.join();
    }
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    sel.regs = liverpool ? &liverpool->regs : nullptr;
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_maximal_reconvergence = instance_.IsMaximalReconvergenceSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // tlg: older NVIDIA (Pascal) has no barycentrics; TLG_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("TLG_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
        .dref_broadcast = LightingTest::Current().dref,
        .block1d_as_2d = AmdGpu::Block1DAs2D(),
        .bc_tiny_decode = AmdGpu::BcTinyDecode(),
        .fragment_contraction_policy = Shader::Backend::SPIRV::FragmentContractionEnabled(),
    };
    // tlg: the recompiler switches (ISS_ names arrive as TLG_), one line each on stdout (the run
    // log keeps it; LOG_INFO is filtered out there), for A/B reports.
    Shader::Toggles::LogStates([](const char* name, bool on, const char* what) {
        std::printf("GPU: shader switch %s=%d (%s)\n", name, on ? 1 : 0, what);
    });
    Common::Backports::LogStates();
    std::printf("GPU: host subgroup size %u, LDS barriers %s\n", profile.subgroup_size,
                profile.needs_lds_barriers ? "on" : "off");
    std::printf("GPU: fragment contraction %s (no depth export, at most one color target)\n",
                profile.fragment_contraction_policy ? "on" : "off");

    // tlg: BP08, start from the driver blob saved by an earlier session (same GPU and driver).
    std::vector<u8> driver_blob;
    if (Common::Backports::On(Common::Backports::Id::PipelineCacheBlob)) {
        driver_blob = LoadDriverCacheBlob(instance);
    }
    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique(
        {.initialDataSize = driver_blob.size(),
         .pInitialData = driver_blob.empty() ? nullptr : driver_blob.data()});
    if (cache_result != vk::Result::eSuccess && !driver_blob.empty()) {
        std::printf("GPU: driver rejected the saved pipeline cache (%s), starting empty (file kept)\n",
                    vk::to_string(cache_result).c_str());
        driver_blob.clear();
        auto retry = instance.GetDevice().createPipelineCacheUnique({});
        cache_result = retry.result;
        cache = std::move(retry.value);
    }
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    // tlg: the compiler threads always exist; the in-game menu switches async compilation live
    // (TlgSettings::async_shaders, TLG_ASYNC_SHADERS at start).
    {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
        constexpr bool native_bionic = true;
#else
        constexpr bool native_bionic = false;
#endif
        // GoW3 Bionic's mobile CPU budget: leave more execution time for FEX and
        // command recording during shader misses, while retaining async compilation.
        auto count = native_bionic ? 2u : std::clamp(TlgThreads::Available() / 2, 1u, 6u);
        const auto capture_count = native_bionic ? 1u :
            std::min(2u, std::max(1u, TlgThreads::Available() / 4));
        if (const char* requested = std::getenv("TLG_GPU_COMPILER_WORKERS")) {
            char* end = nullptr;
            const auto parsed = std::strtoul(requested, &end, 10);
            if (end && *end == '\0' && parsed >= 1 && parsed <= 6) {
                count = static_cast<unsigned>(parsed);
            }
        }
        std::printf("GPU: shader workers: %u compile, %u capture (native Bionic=%d)\n",
                    count, capture_count, int(native_bionic));
        for (unsigned i = 0; i < count; ++i) {
            auto [result, worker_cache] = instance.GetDevice().createPipelineCacheUnique({});
            ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create compiler pipeline cache");
            worker_pipeline_caches.push_back(std::move(worker_cache));
        }
        capture_compiler = std::make_unique<AsyncCompiler>(capture_count, 8, [](unsigned index) {
            Common::SetCurrentThreadName(("tlg:ShaderCapture" + std::to_string(index)).c_str());
            Common::SetCurrentThreadPriority(native_bionic ? Common::ThreadPriority::Low
                                                         : Common::ThreadPriority::High);
        });
        compiler = std::make_unique<AsyncCompiler>(count, 64, [](unsigned index) {
            Common::SetCurrentThreadName(("tlg:ShaderCompile" + std::to_string(index)).c_str());
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        });
        LOG_INFO(Render_Vulkan, "tlg: async graphics compilation ready ({} capture, {} compile workers)",
                 capture_count, count);
    }
    // Publish worker caches before the independent saver thread can inspect them.
    if (Common::Backports::On(Common::Backports::Id::PipelineCacheBlob)) {
        StartDriverCacheSaver();
        if (!driver_blob.empty()) {
            driver_saver->saved_hash = XXH3_64bits(driver_blob.data(), driver_blob.size());
            Common::Backports::Hit(Common::Backports::Id::PipelineCacheBlob);
        }
    }
}

PipelineCache::~PipelineCache() {
    StopWarmUp();
    StopDriverCacheSaver();
    FinishCompilations();
    if (driver_saver) {
        SaveDriverCache();
        delete driver_saver;
        driver_saver = nullptr;
    }
}

// tlg: shader/pipeline compile time on the GPU thread, reported by TLG_FRAME_STATS.
std::atomic<u64> g_tlg_compile_ns;
std::atomic<u32> g_tlg_compiles;
std::atomic<u64> g_tlg_capture_ns, g_tlg_async_ns, g_tlg_compile_wait_ns;
std::atomic<u64> g_tlg_snapshot_ns, g_tlg_guest_wait_ns;
std::atomic<u32> g_tlg_pool_reuses;
std::atomic<u32> g_tlg_pending_draws;
namespace {
struct PhaseTimer {
    std::atomic<u64>& counter;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool active = true;
    void Stop() {
        if (!active) {
            return;
        }
        counter += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start).count());
        active = false;
    }
    ~PhaseTimer() { Stop(); }
};
struct CompileTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CompileTimer() {
        g_tlg_compile_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count());
        ++g_tlg_compiles;
    }
};
} // namespace

bool PipelineCache::QueueShader(Program& program, HwStage stage, SwStage l_stage,
                                 const Shader::ShaderParams& params,
                                 Shader::RuntimeInfo runtime_info,
                                 Shader::Backend::Bindings binding, size_t permutation,
                                 bool initial,
                                 std::optional<Shader::StageSpecialization> specialization) {
    if (!compiler->HasCapacity()) {
        return false;
    }
    PhaseTimer snapshot{g_tlg_snapshot_ns};
    auto build = std::make_shared<AsyncShaderCompilation>();
    build->device = instance.GetDevice();
    build->code.assign(params.code.begin(), params.code.end());
    build->user_data.assign(params.user_data.begin(), params.user_data.end());
    build->info = Shader::Info(stage, l_stage, params);
    build->info.user_data = build->user_data;
    build->runtime = runtime_info;
    if (stage == HwStage::Geometry && !runtime_info.hw.gs.vs_copy.empty()) {
        build->geometry_code.assign(runtime_info.hw.gs.vs_copy.begin(), runtime_info.hw.gs.vs_copy.end());
        build->runtime.hw.gs.vs_copy = build->geometry_code;
    }
    build->binding = binding;
    build->permutation = permutation;
    build->initial = initial;
    if (specialization) {
        build->spec = *specialization;
    }
    build->pool_cache = &compiler_pools;
    snapshot.Stop();
    // tlg: the GPU thread waits until all guest reads and specialization are captured.
    const auto capture_work = [this, build, binding] {
        if (!build->pools) {
            auto [pools, reused] = compiler_pools.Acquire();
            build->pools = std::move(pools);
            if (reused) {
                ++g_tlg_pool_reuses;
            }
        }
        // The key keeps the runtime info as the lookup builds it: translation adjusts it
        // (InjectClipDistanceAttributes adds a fragment input), and a key made after that never
        // matches again, so every draw queued a new permutation (inFAMOUS: 32768 of one shader).
        const Shader::RuntimeInfo lookup_runtime = build->runtime;
        build->ir.emplace(Shader::TranslateProgram(build->code, *build->pools, build->info,
                                                   build->runtime, profile));
        build->spec = Shader::StageSpecialization(build->info, lookup_runtime, profile, binding);
        build->fetch.Capture(build->info);
    };
    // tlg: the capture worker reads guest memory (fetch shader, resource descriptors). A page
    // the GPU wrote is protected under precise readbacks, and its readback needs this (the GPU
    // command) thread, which is waiting here: the worker gives up on such a fault
    // (runtime_fault_recover) and the capture runs again on this thread, where the readback
    // works. Seen in Gravity Rush 2 at the first draws.
    auto faulted = std::make_shared<std::atomic<bool>>(false);
    AsyncJob<bool> capture_job;
    if (!capture_job.Start(*capture_compiler, [capture_work, faulted] {
        PhaseTimer capture{g_tlg_capture_ns};
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            runtime_fault_recover = nullptr;
            faulted->store(true, std::memory_order_release);
            return false;
        }
        runtime_fault_recover = &recover;
        try {
            capture_work();
        } catch (...) {
            runtime_fault_recover = nullptr; // `recover` dies with this frame
            throw;
        }
        runtime_fault_recover = nullptr;
        return true;
    })) {
        return false;
    }
    {
        PhaseTimer waiting{g_tlg_guest_wait_ns};
        capture_job.Wait();
    }
    const auto mark_failed = [&](const std::string& error) {
        if (initial) {
            program.compilation_failed = true;
        } else {
            specialization->info = &program.info;
            program.failed_permutations.push_back(std::move(*specialization));
        }
        LOG_ERROR(Render_Vulkan, "tlg: shader {:#x}/{} capture failed: {}", params.hash,
                  permutation, error);
    };
    if (faulted->load(std::memory_order_acquire)) {
        PhaseTimer capture{g_tlg_capture_ns};
        build->ir.reset();
        try {
            capture_work();
        } catch (const std::exception& e) {
            mark_failed(e.what());
            return false;
        }
    } else if (!capture_job.Poll()) {
        mark_failed(capture_job.Error());
        return false;
    }
    const bool queued = build->job.Start(*compiler, [this, build] {
        PhaseTimer compile{g_tlg_async_ns};
        Shader::Gcn::ScopedFetchShaderSnapshot snapshot{build->fetch};
        auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, build->runtime, *build->ir,
                                                    build->binding);
        DumpShader(build->code, build->info.pgm_hash, build->info.hw_stage, build->permutation, "bin");
        LogEmulatedFloatAtomics(build->info, profile);
        DumpShader(spv, build->info.pgm_hash, build->info.hw_stage, build->permutation, "spv");
        auto patch = GetShaderPatch(build->info.pgm_hash, build->info.hw_stage,
                                    build->permutation, "spv");
        build->module = CompileSPV(patch && EmulatorSettings.IsPatchShaders() ? *patch : spv,
                                    instance.GetDevice());
        build->binary = std::move(spv);
        build->ir.reset();
        compiler_pools.Recycle(std::move(build->pools));
        return build;
    }, true);
    if (queued) {
        program.pending = std::move(build);
        pending_programs.push_back(&program);
    }
    return queued;
}

void PipelineCache::PublishShaders(bool wait) {
    for (size_t i = 0; i < pending_programs.size();) {
        auto* program = pending_programs[i];
        const auto hash = program->info.pgm_hash;
        const auto retire = [&] {
            pending_programs[i] = pending_programs.back();
            pending_programs.pop_back();
        };
        auto build = program->pending;
        if (wait) {
            build->job.Wait();
        }
        auto result = build->job.Poll();
        if (!result) {
            if (build->job.Failed()) {
                if (build->initial) {
                    program->compilation_failed = true;
                } else {
                    build->spec.info = &program->info;
                    program->failed_permutations.push_back(std::move(build->spec));
                }
                program->pending.reset();
                LOG_ERROR(Render_Vulkan, "tlg: shader {:#x}/{} compilation failed: {}", hash,
                          build->permutation, build->job.Error());
                retire();
            } else {
                ++i;
            }
            continue;
        }
        const auto perm_hash = HashCombine(hash, build->permutation);
        Vulkan::SetObjectName(instance.GetDevice(), build->module,
                             GetShaderName(build->info.hw_stage, hash, build->permutation));
        RegisterShaderBinary(std::move(build->binary), hash, build->permutation);
        {
            std::unique_lock lock{programs_mutex};
            if (build->initial) {
                program->info = std::move(build->info);
                // user_data is refreshed on the next lookup; no pointer into a completed job survives.
                program->info.user_data = {};
                program->info_template = std::make_unique<Shader::Info>(program->info);
            }
            build->spec.info = &program->info;
            RegisterShaderMeta(program->info, build->spec.fetch_shader_data, build->spec,
                               perm_hash, build->permutation);
            program->InsertPermut(build->module, std::move(build->spec), build->permutation);
            build->module = nullptr;
            program->pending.reset();
        }
        retire();
    }
}

void PipelineCache::PublishGraphics(bool wait) {
    for (auto it = pending_graphics.begin(); it != pending_graphics.end();) {
        auto build = it.value();
        if (wait) {
            build->job.Wait();
        }
        auto result = build->job.Poll();
        if (!result) {
            if (build->job.Failed() && !build->failure_logged) {
                LOG_ERROR(Render_Vulkan, "tlg: pipeline {:#x} compilation failed: {}",
                          std::hash<GraphicsPipelineKey>{}(it.key()), build->job.Error());
                build->failure_logged = true;
            }
            ++it;
            continue;
        }
        const auto key = it.key();
        build->pipeline->PublishStages(build->live_infos);
        RegisterPipelineData(key, std::hash<GraphicsPipelineKey>{}(key), build->data);
        graphics_pipelines.emplace(key, std::move(build->pipeline));
        ++num_new_pipelines;
        it = pending_graphics.erase(it);
    }
}

void PipelineCache::FinishCompilations() {
    if (compiler) {
        capture_compiler->Stop();
        compiler->Stop();
        PublishShaders(true);
        PublishGraphics(true);
        std::vector<vk::PipelineCache> sources;
        for (const auto& cache : worker_pipeline_caches) {
            sources.push_back(*cache);
        }
        const auto result = instance.GetDevice().mergePipelineCaches(*pipeline_cache, sources);
        if (result != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "tlg: compiler cache merge failed: {}", vk::to_string(result));
        }
    }
}

bool PipelineCache::AsyncCompilation() const {
    return compiler && TlgSettings::Get().async_shaders.load(std::memory_order_relaxed);
}

void PipelineCache::SeedWorkerCaches() {
    if (!compiler || worker_caches_seeded) {
        return;
    }
    for (const auto& cache : worker_pipeline_caches) {
        const auto result = instance.GetDevice().mergePipelineCaches(*cache, *pipeline_cache);
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed to seed compiler pipeline cache");
    }
    worker_caches_seeded = true;
}

void PipelineCache::ReportCompilerStats() {
    if (compiler) {
        std::printf("Compiler stats: capture queue %zu, compile queue %zu; queue waits: capture "
                    "%.1f ms, compile %.1f ms; pools retained %zu, reused %u\n",
                    capture_compiler->Depth(), compiler->Depth(),
                    capture_compiler->QueueWaitNs() / 1e6, compiler->QueueWaitNs() / 1e6,
                    compiler_pools.Retained(), g_tlg_pool_reuses.exchange(0));
    }
}

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    return RefreshGraphicsKey(worker_sel) && !worker_sel.worker->failed;
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto found = AmdGpu::TryGetParams(*pgm);
        if (!found || found->hash != stage.hash) {
            return nullptr;
        }
        const auto& params = *found;
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    const auto it = graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() ? it->second.get() : nullptr;
}


// tlg: BP07 (bbport 10baf9d). The async path above skips every draw whose pipeline or shader is
// still being compiled. With TLG_BP07=1 only draws of a pass drawn every frame in a busy frame,
// with geometry of a few triangles or more and direct, may go without; the rest (small and
// full-screen passes, indirect draws, frames with fewer than 300 draws, targets drawn to for
// fewer than 8 frames in a row) compile now, shaders included, on the GPU thread.
namespace {
enum class Bp07Reason : u32 { Skippable, Small, Indirect, LightFrame, NoStreak, Count };

struct Bp07State {
    struct TargetStreak {
        u64 last_frame = ~0ull;
        u32 frames = 0;
    };
    std::unordered_map<u64, TargetStreak> streaks;
    u64 frame_seen = ~0ull;
    u64 draws_this_frame = 0, last_frame_draws = 0;
    u64 last_target = 0, last_target_frame = ~0ull;
    // Per reason: draws that compiled now / draws that went without (the async path skipped).
    std::array<u64, size_t(Bp07Reason::Count)> sync{}, skipped{};
    // The last pass of each kind, for the log: target address, indices.
    std::array<u64, size_t(Bp07Reason::Count)> last_target_of{};
    std::array<u32, size_t(Bp07Reason::Count)> last_indices_of{};
    std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();
    Bp07Reason current = Bp07Reason::Skippable;
};
Bp07State bp07; // GPU thread only (pipeline selection)

u64 Bp07DrawTarget(const AmdGpu::Regs& regs) {
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
            if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) {
                return regs.color_buffers[cb].Address();
            }
        }
    }
    return regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthAddress() : 0;
}

/// Counts the draw and classifies it; the result stays in bp07.current.
Bp07Reason Bp07Classify(const AmdGpu::Regs& regs, bool indirect) {
    auto& st = bp07;
    const u64 frame = TlgStats::gpu_frames.load(std::memory_order_relaxed);
    if (frame != st.frame_seen) {
        st.last_frame_draws = st.draws_this_frame;
        st.draws_this_frame = 0;
        st.frame_seen = frame;
    }
    ++st.draws_this_frame;
    const u64 target = Bp07DrawTarget(regs);
    if (target != 0 && !(target == st.last_target && frame == st.last_target_frame)) {
        st.last_target = target;
        st.last_target_frame = frame;
        auto& streak = st.streaks[target];
        if (streak.last_frame != frame) {
            streak.frames = streak.last_frame + 1 == frame ? streak.frames + 1 : 1;
            streak.last_frame = frame;
        }
    }
    constexpr u64 BusyFrameDraws = 300;
    constexpr u32 FullScreenIndices = 12;
    constexpr u32 PerFrameStreak = 8;
    Bp07Reason reason = Bp07Reason::Skippable;
    if (st.last_frame_draws < BusyFrameDraws) {
        reason = Bp07Reason::LightFrame;
    } else if (indirect) {
        reason = Bp07Reason::Indirect;
    } else if (regs.num_indices <= FullScreenIndices) {
        reason = Bp07Reason::Small;
    } else {
        const auto streak = st.streaks.find(target);
        if (target == 0 || streak == st.streaks.end() || streak->second.frames < PerFrameStreak) {
            reason = Bp07Reason::NoStreak;
        }
    }
    const size_t i = size_t(reason);
    st.last_target_of[i] = target;
    st.last_indices_of[i] = indirect ? 0u : regs.num_indices;
    st.current = reason;
    return reason;
}

void Bp07Tally(bool compiled_now) {
    auto& st = bp07;
    const size_t i = size_t(st.current);
    ++(compiled_now ? st.sync : st.skipped)[i];
    if (compiled_now) {
        Common::Backports::Hit(Common::Backports::Id::AsyncSkippable);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - st.last_log < std::chrono::seconds(5)) {
        return;
    }
    st.last_log = now;
    static constexpr const char* names[] = {"skippable", "small", "indirect", "light-frame",
                                            "no-streak"};
    std::string line;
    for (size_t r = 0; r < size_t(Bp07Reason::Count); ++r) {
        if (st.sync[r] == 0 && st.skipped[r] == 0) {
            continue;
        }
        line += fmt::format(" {}: {} compiled now, {} went without (last target {:#x}, {} indices);",
                            names[r], st.sync[r], st.skipped[r], st.last_target_of[r],
                            st.last_indices_of[r]);
    }
    if (!line.empty()) {
        LOG_INFO(Render_Vulkan, "BP07 async draws (total):{}", line);
    }
}
} // namespace

// tlg: TLG_ASYNC_WAIT_MS=N (default 0 = off), TLG_ASYNC_WAIT_FRAME_MS=M (default 8). A draw whose
// shader or pipeline is still compiling in the background is skipped. With N > 0 it first waits
// for the compile up to N ms, and the waits of one GPU frame add up to at most M ms: a short
// compile then costs a few milliseconds instead of a missing ground or effect, a long one is
// skipped as before, so the frame time stays bounded. The log shows the tally every 5 s.
// A new shader is the usual reason for a skipped draw (the pipeline is only queued once the
// shader is translated), so GetProgram waits too, not only GetGraphicsPipeline.
namespace {
struct AsyncWaitBudget {
    std::chrono::microseconds per_draw{0}, per_frame{8000};
    u64 frame = ~0ull;
    std::chrono::nanoseconds used{0};
    u64 waited = 0, ready = 0, timed_out = 0;
    std::chrono::nanoseconds total{0};
    std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();

    AsyncWaitBudget() {
        if (const char* v = std::getenv("TLG_ASYNC_WAIT_MS"); v && *v) {
            per_draw = std::chrono::milliseconds(std::clamp(std::atoi(v), 0, 100));
        }
        if (const char* v = std::getenv("TLG_ASYNC_WAIT_FRAME_MS"); v && *v) {
            per_frame = std::chrono::milliseconds(std::clamp(std::atoi(v), 0, 100));
        }
    }
    bool Enabled() const { return per_draw.count() > 0 && per_frame.count() > 0; }

    /// How long this draw may wait now (zero: none left in this frame).
    std::chrono::nanoseconds Allowance() {
        const u64 now_frame = TlgStats::gpu_frames.load(std::memory_order_relaxed);
        if (now_frame != frame) {
            frame = now_frame;
            used = std::chrono::nanoseconds{0};
        }
        const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(per_frame) - used;
        return left.count() <= 0 ? std::chrono::nanoseconds{0}
                                 : std::min<std::chrono::nanoseconds>(left, per_draw);
    }

    void Tally(std::chrono::nanoseconds spent, bool finished) {
        used += spent;
        total += spent;
        ++waited;
        ++(finished ? ready : timed_out);
        const auto now = std::chrono::steady_clock::now();
        if (now - last_log < std::chrono::seconds(5)) {
            return;
        }
        last_log = now;
        // A warning, not info: the log keeps only warnings and above.
        LOG_WARNING(Render_Vulkan,
                    "Async wait (total): {} waits for a compiling shader or pipeline, {:.1f} ms in all, "
                    "{} finished in time, {} gave up at the limit and the draw was skipped "
                    "({} ms per wait, {} ms per frame)",
                    waited, double(total.count()) / 1e6, ready, timed_out, per_draw.count() / 1000,
                    per_frame.count() / 1000);
    }
};
AsyncWaitBudget async_wait; // GPU thread only (pipeline selection)
bool async_wait_set_by_env = std::getenv("TLG_ASYNC_WAIT_MS") != nullptr;
} // namespace

void PipelineCache::UseDefaultPipelineWait() {
    if (!async_wait_set_by_env) {
        async_wait.per_draw = std::chrono::milliseconds(4);
        std::printf("GPU: a draw waits up to 4 ms for its pipeline (precompiled cache)\n");
    }
}

namespace {
// Match the working TryPreparedPipeline validation policy, not PM4 registers
// alone. This is opt-in until scene-by-scene A/B and Android driver testing.
bool FastGraphicsKeyEnabled() {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    static const bool enabled = [] {
        const char* env = std::getenv("ISS_FAST_DRAW_KEY");
        return env && env[0] == '1';
    }();
    return enabled;
#else
    return false;
#endif
}
} // namespace

const GraphicsPipeline* PipelineCache::TryFastGraphicsPipeline(
    const DrawIndirectParams params, const PreparedDraw* prepared) {
    if (!FastGraphicsKeyEnabled() || prepared || params.is_indirect ||
        EmulatorSettings.IsShaderCollect() || FrameCapture::Active() || !fast_graphics.valid ||
        fast_graphics.tick != scheduler.CurrentTick() ||
        fast_graphics.frame != TlgStats::gpu_frames.load(std::memory_order_relaxed) ||
        fast_graphics.checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // The existing precompiler/prepared-draw path checks all shader-program
    // identities AND live flattened guest user data before trusting a cached
    // pipeline. Repeat that validation here: shader constants, fetch descriptors
    // and buffer sizes can change without a new PM4 shader program address.
    const auto& regs = liverpool->regs;
    for (const auto& remembered : fast_graphics.stages) {
        if (!remembered.info) continue;
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(remembered.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) return nullptr;
        const auto found = AmdGpu::TryGetParams(*pgm);
        if (!found || found->hash != remembered.program_hash) return nullptr;
        auto& info = *const_cast<Shader::Info*>(remembered.info);
        info.pgm_base = found->Base();
        info.user_data = found->user_data;
        info.RefreshFlatBuf();
        if (info.pgm_base != remembered.program_base ||
            info.flattened_ud_buf.size() != remembered.flat.size() ||
            std::memcmp(info.flattened_ud_buf.data(), remembered.flat.data(),
                        remembered.flat.size() * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    TlgRegion::NotePipeline('g', fast_graphics.key_hash, true);
    if (IssBionicPerf::Enabled()) {
        IssBionicPerf::fast_draw_key_hits.fetch_add(1, std::memory_order_relaxed);
    }
    return fast_graphics.pipeline;
}

const GraphicsPipeline* PipelineCache::RememberFastGraphicsPipeline(
    const GraphicsPipeline* pipeline, const DrawIndirectParams params,
    const PreparedDraw* prepared) {
    fast_graphics.valid = false;
    if (!FastGraphicsKeyEnabled() || prepared || params.is_indirect || !pipeline ||
        EmulatorSettings.IsShaderCollect() || FrameCapture::Active()) {
        return pipeline;
    }
    FastGraphicsState next{};
    next.pipeline = pipeline;
    next.tick = scheduler.CurrentTick();
    next.frame = TlgStats::gpu_frames.load(std::memory_order_relaxed);
    next.checksum = liverpool->gfx_reg_checksum;
    next.key_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        const auto* info = sel.infos[i];
        if (!info) continue;
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(info->hw_stage));
        if (!pgm || !pgm->Address<u32*>()) return pipeline;
        const auto found = AmdGpu::TryGetParams(*pgm);
        if (!found) return pipeline;
        auto& stage = next.stages[i];
        stage.info = info;
        stage.hw_stage = info->hw_stage;
        stage.program_hash = found->hash;
        stage.program_base = found->Base();
        stage.flat.assign(info->flattened_ud_buf.begin(), info->flattened_ud_buf.end());
    }
    next.valid = true;
    fast_graphics = std::move(next);
    return pipeline;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared) {
    SeedWorkerCaches();
    PublishShaders();
    PublishGraphics();
    PumpWarmUp();
    used_prepared = nullptr;
    // tlg: BP07, classified per draw (also those a prepared pipeline serves, for the frame's draws).
    const bool bp07 = Common::Backports::On(Common::Backports::Id::AsyncSkippable);
    sel.sync_compile = false;
    if (bp07) {
        sel.sync_compile = Bp07Classify(liverpool->regs, params.is_indirect) != Bp07Reason::Skippable;
    }
    if (prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            TlgRegion::NotePipeline('g', std::hash<GraphicsPipelineKey>{}(prepared->key), true);
            return pipeline;
        }
    }
    // The Bionic build uses no speculative draw-preparation workers by default.
    // Fast Draw checks the exact shader data before bypassing pipeline selection.
    if (const auto* ready = TryFastGraphicsPipeline(params, prepared)) {
        return ready;
    }
    sel.draw_indirect_params = params;
    if (!RefreshGraphicsKey(sel)) {
        if (sel.compilation_pending) {
            ++g_tlg_pending_draws;
            if (bp07 && !sel.sync_compile) {
                Bp07Tally(false); // a draw classified skippable
            }
        }
        return nullptr;
    }
    // tlg: region trace (TLG_REGION_TRACE=1): the first request of each pipeline.
    TlgRegion::NotePipeline('g', std::hash<GraphicsPipelineKey>{}(sel.graphics_key),
                            graphics_pipelines.contains(sel.graphics_key));
    if (Precompile::CaptureEnabled() && !graphics_pipelines.contains(sel.graphics_key)) {
        Precompile::CaptureGraphics(sel);
    }
    if (AsyncCompilation() && !sel.regs->stage_enable.hs_en && !EmulatorSettings.IsShaderCollect()) {
        const auto found = graphics_pipelines.find(sel.graphics_key);
        if (found != graphics_pipelines.end()) {
            return RememberFastGraphicsPipeline(found->second.get(), params, prepared);
        }
        if (sel.sync_compile) {
            // tlg: BP07: this draw does not go without. Wait for the worker building the
            // pipeline, else fall through to the compile below on this thread.
            Bp07Tally(true);
            if (const auto pending = pending_graphics.find(sel.graphics_key);
                pending != pending_graphics.end()) {
                PhaseTimer waiting{g_tlg_compile_wait_ns};
                pending.value()->job.Wait();
                waiting.Stop();
                PublishGraphics();
                const auto ready = graphics_pipelines.find(sel.graphics_key);
                if (ready != graphics_pipelines.end()) {
                    return RememberFastGraphicsPipeline(ready->second.get(), params, prepared);
                }
            }
            goto compile_now;
        }
        if (!pending_graphics.contains(sel.graphics_key) && compiler->HasCapacity()) {
            PhaseTimer capture{g_tlg_snapshot_ns};
            auto build = std::make_shared<AsyncGraphicsCompilation>();
            build->selection = sel;
            build->selection.regs = nullptr;
            build->live_infos = sel.infos;
            for (u32 i = 0; i < MaxShaderStages; ++i) {
                if (sel.infos[i]) {
                    build->infos[i] = std::make_unique<Shader::Info>(*sel.infos[i]);
                    build->user_data[i].assign(sel.infos[i]->user_data.begin(), sel.infos[i]->user_data.end());
                    build->infos[i]->user_data = build->user_data[i];
                    build->selection.infos[i] = build->infos[i].get();
                    build->fetch[i].Capture(*build->infos[i]);
                }
            }
            const bool queued = build->job.Start(*compiler, [this, build] {
                // Only the vertex stage uses fetch-shader guest pointers during pipeline construction.
                Shader::Gcn::ScopedFetchShaderSnapshot snapshot{build->fetch[u32(SwStage::Vertex)]};
                PhaseTimer compile{g_tlg_async_ns};
                auto& s = build->selection;
                build->pipeline = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, s.graphics_key,
                    *worker_pipeline_caches[AsyncCompiler::WorkerIndex()],
                    s.infos, s.runtime_infos, s.fetch_shader, s.modules, build->data, false);
                return build;
            });
            if (queued) {
                pending_graphics.emplace(sel.graphics_key, std::move(build));
            }
        }
        // tlg: TLG_ASYNC_WAIT_MS: give the background compile a few milliseconds before the draw goes.
        if (async_wait.Enabled()) {
            if (const auto building = pending_graphics.find(sel.graphics_key);
                building != pending_graphics.end()) {
                if (const auto allowance = async_wait.Allowance(); allowance.count() > 0) {
                    const auto t0 = std::chrono::steady_clock::now();
                    const bool finished = building.value()->job.WaitFor(allowance);
                    async_wait.Tally(std::chrono::steady_clock::now() - t0, finished);
                    if (finished) {
                        PublishGraphics();
                        if (const auto done = graphics_pipelines.find(sel.graphics_key);
                            done != graphics_pipelines.end()) {
                            return RememberFastGraphicsPipeline(done->second.get(), params, prepared);
                        }
                    }
                }
            }
        }
        ++g_tlg_pending_draws;
        if (bp07) {
            Bp07Tally(false);
        }
        return nullptr;
    }
compile_now:
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        CompileTimer timer;

        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
            sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, false);

        RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        sel.fetch_shader.reset();
    }
    return RememberFastGraphicsPipeline(it->second.get(), params, prepared);
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    SeedWorkerCaches();
    PublishShaders();
    PublishGraphics();
    PumpWarmUp();
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    TlgRegion::NotePipeline('c', std::hash<ComputePipelineKey>{}(compute_key),
                            compute_pipelines.contains(compute_key));
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new && Precompile::CaptureEnabled()) {
        Precompile::CaptureCompute(sel, liverpool->GetCsRegs());
    }
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        CompileTimer timer;

        ComputePipeline::SerializationSupport sdata{};
        // tlg: built on the compiler threads when the driver hasn't cached it (the dispatch
        // binds it once built, vk_compute_pipeline.cpp). Not while collecting shaders: their
        // pipelines are erased on a reload, with a build perhaps still running.
        const ComputeBuilder builder{compiler.get(), worker_pipeline_caches};
        const bool async = compiler && !EmulatorSettings.IsShaderCollect();
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *sel.infos[0],
                                                       sel.modules[0], sdata, false,
                                                       async ? &builder : nullptr);
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    } else if (it->second) {
        it.value()->SwapInOptimized();
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    // tlg: a depth/stencil buffer that cannot affect the draw is not attached: on the PC a
    // smaller one would crop a full-screen pass (eltutz, vk_depth_stencil_state.h).
    const bool db_enabled = GetEffectiveDepthStencilState(regs).needs_attachment;

    key.z_format = db_enabled && regs.depth_buffer.DepthValid()
                       ? regs.depth_buffer.z_info.format
                       : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = db_enabled && regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // TLG_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("TLG_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("TLG_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    sel.fetch_shader = std::nullopt;
    sel.compilation_pending = false;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto found = AmdGpu::TryGetParams(*pgm);
        // tlg: a geometry stage also needs its VS copy shader (BuildRuntimeInfo reads it).
        const bool copy_missing = stage_in == HwStage::Geometry &&
                                  (!regs.vs_program.Address<u32*>() ||
                                   !AmdGpu::TryGetParams(regs.vs_program));
        if (!found || copy_missing) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }
        const auto& params = *found;
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (!sel.infos[stage_out_idx]) {
            sel.compilation_pending = true;
            return false;
        }
        if (fetch_shader_) {
            sel.fetch_shader = std::move(fetch_shader_);
        }
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);
    if (sel.compilation_pending) {
        return false;
    }

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    if (!fs_info) {
        // tlg: with no pixel shader, GraphicsPipeline adds the discard fragment shader from this
        // runtime info. It used to keep the previous draw's, so a pipeline got the discard shader
        // (or lost it) depending on the draw before it (NVIDIA clip distance emulation).
        auto& fs_runtime = sel.runtime_infos[static_cast<u32>(SwStage::Fragment)];
        fs_runtime = {};
        fs_runtime.hw.fs.clip_distance_emulation = NeedsClipDistanceEmulation(regs, profile);
    }
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        if (sel.compilation_pending) {
            return false;
        }
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && sel.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    sel.cs_regs = nullptr;
    return RefreshComputeKey(sel, compute_key);
}

bool PipelineCache::RefreshComputeKey(PipelineSelection& selection, ComputePipelineKey& key) {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = selection.cs_regs ? *selection.cs_regs : liverpool->GetCsRegs();
    const auto cs_found = AmdGpu::TryGetParams(cs_pgm);
    if (!cs_found) {
        selection.infos[0] = nullptr;
        return false;
    }
    const auto& cs_params = *cs_found;
    std::tie(selection.infos[0], selection.modules[0], selection.fetch_shader, key.value) =
        GetProgram(selection, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return selection.infos[0] != nullptr;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    CompileTimer timer;

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    LogEmulatedFloatAtomics(info, profile);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

static std::string DescribeSpecDiff(const Shader::StageSpecialization& a, const Shader::StageSpecialization& b) {
    std::string diff;
    if (!a.Valid() || !b.Valid()) diff = fmt::format("valid {}/{}", a.Valid(), b.Valid());
    if (a.bitset != b.bitset) diff += diff.empty() ? "bitset" : ", bitset";
    auto note = [&](const char* what) { diff += diff.empty() ? what : std::string(", ") + what; };
    if (a.vs_attribs != b.vs_attribs) note("vs_attribs");
    if (a.fetch_shader_data != b.fetch_shader_data) note("fetch_shader");
    if (a.fmasks != b.fmasks) note("fmasks");
    if (a.start != b.start) note("start_bindings");
    if (a.buffers != b.buffers) note("buffers");
    if (a.images != b.images) note("images");
    if (a.samplers != b.samplers) note("samplers");
    const auto& ra = a.runtime_info;
    const auto& rb = b.runtime_info;
    if (!(ra.props == rb.props)) note("props");
    if (!ra.HasSameSwInfo(rb.sw)) note("sw_info");
    if (!ra.HasSameHwInfo(rb.hw)) {
        if (ra.hw_stage == Shader::HwStage::Fragment) {
            const auto& fa = ra.hw.fs;
            const auto& fb = rb.hw.fs;
            for (size_t i = 0; i < fa.color_buffers.size(); ++i) {
                if (!(fa.color_buffers[i] == fb.color_buffers[i])) {
                    diff += fmt::format("{}fs.color_buffers[{}] fmt {}/{} -> {}/{} export {} -> {}",
                                        diff.empty() ? "" : ", ", i,
                                        u32(fa.color_buffers[i].data_format), u32(fa.color_buffers[i].num_format),
                                        u32(fb.color_buffers[i].data_format), u32(fb.color_buffers[i].num_format),
                                        u32(fa.color_buffers[i].export_format), u32(fb.color_buffers[i].export_format));
                }
            }
            if (!(fa.en_flags == fb.en_flags) || !(fa.addr_flags == fb.addr_flags)) note("fs.input_flags");
            if (fa.num_inputs != fb.num_inputs) note("fs.num_inputs");
            if (!std::ranges::equal(fa.inputs.begin(), fa.inputs.begin() + fa.num_inputs, fb.inputs.begin(),
                                    fb.inputs.begin() + fb.num_inputs)) note("fs.inputs");
            if (fa.z_export_format != fb.z_export_format) note("fs.z_export");
            if (fa.num_samples != fb.num_samples) note("fs.num_samples");
            if (fa.mrtz_mask != fb.mrtz_mask) note("fs.mrtz_mask");
            if (fa.dual_source_blending != fb.dual_source_blending) note("fs.dual_source");
            if (fa.clip_distance_emulation != fb.clip_distance_emulation) note("fs.clip_distance");
            if (fa.front_face_all_bits != fb.front_face_all_bits) note("fs.front_face");
            if (fa.motion_vectors != fb.motion_vectors) note("fs.motion_vectors");
        } else {
            note("hw_info");
        }
    }
    return diff.empty() ? "nothing?" : diff;
}

void Program::LogPermutationGrowth() const {
    const size_t n = modules.size();
    if (n < 16 || (n & (n - 1))) {
        return;
    }
    LOG_WARNING(Render_Vulkan, "tlg: shader {:#x} (hw stage {}) has {} permutations; last two differ in: {}",
                info.pgm_hash, static_cast<int>(info.hw_stage), n,
                DescribeSpecDiff(modules[n - 2].spec, modules[n - 1].spec));
}

bool Program::PendingInitial() const {
    return pending && pending->initial;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (sel.worker) {
        // tlg: draw-preparation worker: look up only, with the worker's own Info copy.
        auto& worker = *sel.worker;
        std::shared_lock lk{programs_mutex};
        const auto found_program = program_cache.find(params.hash);
        if (found_program == program_cache.end() || !found_program->second->info_template) {
            worker.failed = true;
            return {};
        }
        const Program* program = found_program->second.get();
        auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
        auto& info = it_info->second;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // The walk of resource tables and the fetch shader parse read guest memory through
        // pointers in the registers; ahead of the GPU thread that memory may already be
        // reused. A fault returns here (runtime_fault_recover) and the draw is left to the GPU
        // thread. A jump out of the specialization leaks its partial allocations (rare).
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            worker.failed = true;
            return {};
        }
        runtime_fault_recover = &recover;
        info.RefreshFlatBuf();
        auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
        runtime_fault_recover = nullptr;
        const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
        if (it == program->modules.end()) {
            worker.failed = true;
            return {};
        }
        info.AddBindings(binding);
        const size_t perm_idx = std::distance(program->modules.begin(), it);
        worker.stages.push_back(
            {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf});
        return std::make_tuple(&info, it->module, it->spec.fetch_shader_data,
                               HashCombine(params.hash, perm_idx));
    }

    // tlg: TLG_ASYNC_WAIT_MS: before the draw goes for lack of its shader, give the background
    // translation a few milliseconds. True when the shader is ready and the lookup should run again.
    const auto wait_for_shader = [this](const Program& program) {
        if (!async_wait.Enabled() || !program.pending) {
            return false;
        }
        const auto allowance = async_wait.Allowance();
        if (allowance.count() <= 0) {
            return false;
        }
        const auto started = std::chrono::steady_clock::now();
        const bool finished = program.pending->job.WaitFor(allowance);
        async_wait.Tally(std::chrono::steady_clock::now() - started, finished);
        if (!finished) {
            return false;
        }
        PublishShaders();
        return !program.pending; // published; else the lookup would find it pending again
    };

    auto it_pgm = program_cache.find(params.hash); // this thread is the only writer
    if (it_pgm == program_cache.end() && LoadProgramFromCache(params.hash)) {
        it_pgm = program_cache.find(params.hash);
    }
    const bool compile_async = AsyncCompilation() && hw_stage != HwStage::Compute &&
                               !sel.regs->stage_enable.hs_en &&
                               !EmulatorSettings.IsShaderCollect() &&
                               !sel.sync_compile; // tlg: BP07 draws compile their shaders now
    if (!compile_async && it_pgm != program_cache.end() && it_pgm->second->pending) {
        {
            PhaseTimer waiting{g_tlg_compile_wait_ns};
            it_pgm->second->pending->job.Wait();
        }
        PublishShaders();
    }
    if (!compile_async && it_pgm != program_cache.end() && it_pgm->second->modules.empty() &&
        !it_pgm->second->compilation_failed) {
        std::unique_lock lock{programs_mutex};
        program_cache.erase(it_pgm);
        it_pgm = program_cache.end();
    }
    if (it_pgm == program_cache.end()) {
        auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
        const size_t first_idx = NewPermutationIndex(*new_program, params.hash);
        if (compile_async) {
            QueueShader(*new_program, hw_stage, sw_stage, params, runtime_info, binding, first_idx,
                        true);
            const Program* queued = new_program.get();
            {
                std::unique_lock lock{programs_mutex};
                program_cache.emplace(params.hash, std::move(new_program));
            }
            if (wait_for_shader(*queued)) {
                return GetProgram(sel, hw_stage, sw_stage, params, binding);
            }
            return {};
        }
        auto start = binding;
        auto compile_runtime = runtime_info; // translation adjusts it; the key keeps the original
        vk::ShaderModule module;
        try {
            module = CompileModule(new_program->info, compile_runtime, params.code, first_idx,
                                   binding);
        } catch (const Shader::UnsupportedShaderError& e) {
            // tlg: kept in the cache as failed, so its draws are skipped without retranslating.
            LOG_ERROR(Render_Vulkan, "tlg: shader {:#x} refused: {}", params.hash, e.what());
            new_program->compilation_failed = true;
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
            return {};
        }
        auto spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, first_idx);

        RegisterShaderMeta(new_program->info, spec.fetch_shader_data, spec, perm_hash, first_idx);
        new_program->InsertPermut(module, std::move(spec), first_idx);
        new_program->info_template = std::make_unique<Shader::Info>(new_program->info);
        Program* program = new_program.get();
        {
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
        }
        return std::make_tuple(&program->info, module,
                               program->modules[first_idx].spec.fetch_shader_data, perm_hash);
    }

    auto& program = it_pgm.value();
    if (program->compilation_failed || (program->modules.empty() && program->pending)) {
        if (!program->compilation_failed && wait_for_shader(*program)) {
            return GetProgram(sel, hw_stage, sw_stage, params, binding);
        }
        return {};
    }
    if (program->modules.empty() && compile_async) {
        QueueShader(*program, hw_stage, sw_stage, params, runtime_info, binding,
                    NewPermutationIndex(*program, params.hash), true);
        if (wait_for_shader(*program)) {
            return GetProgram(sel, hw_stage, sw_stage, params, binding);
        }
        return {};
    }
    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        program->info_template = std::make_unique<Shader::Info>(program->info);
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();

    // tlg (i3-pc 0120115): the specialization depends only on the flattened user data (which holds
    // every register, so the fetch shader address too), the runtime info, the first bindings and,
    // for a vertex shader, the vertex buffer descriptors its fetch shader reads from guest memory.
    // Tessellation stages also read constants from memory: never reused. TLG_VARIANT_REUSE=1
    // turns this on; =check also builds the specialization on every hit and logs a mismatch.
    static const int reuse_mode = [] {
        const char* value = std::getenv("TLG_VARIANT_REUSE");
        return !value ? 0 : (std::string_view{value} == "check" ? 2 : (value[0] == '1' ? 1 : 0));
    }();
    static std::atomic<u64> reuse_hits{0}, reuse_misses{0}, reuse_mismatches{0};
    auto& last = program->last_match;
    const bool reusable = reuse_mode != 0 && !TlgToggle::Disabled(TlgToggle::VariantReuse) &&
                          sw_stage != SwStage::TessellationControl &&
                          sw_stage != SwStage::TessellationEval;
    const auto vs_sharps_match = [&](bool store) {
        const auto& fetch = program->modules[last.perm_idx].spec.fetch_shader_data;
        if (!fetch) {
            if (store) {
                last.vs_sharps.clear();
            }
            return store || last.vs_sharps.empty();
        }
        if (store) {
            last.vs_sharps.clear();
        } else if (last.vs_sharps.size() != fetch->attributes.size()) {
            return false;
        }
        for (size_t i = 0; i < fetch->attributes.size(); ++i) {
            const auto sharp = fetch->attributes[i].GetSharp(info);
            std::array<u32, 4> words;
            static_assert(sizeof(sharp) == sizeof(words));
            std::memcpy(words.data(), &sharp, sizeof(words));
            if (store) {
                last.vs_sharps.push_back(words);
            } else if (last.vs_sharps[i] != words) {
                return false;
            }
        }
        return true;
    };
    if (reusable && last.perm_idx < program->modules.size() && last.start == binding &&
        last.runtime_info == runtime_info && last.flat_buf == info.flattened_ud_buf &&
        vs_sharps_match(false)) {
        auto& hit = program->modules[last.perm_idx];
        if (reuse_mode == 2) {
            // The lookup below compares stored == fresh (the operator is not symmetric).
            const auto check = Shader::StageSpecialization(info, runtime_info, profile, binding);
            if (!(hit.spec == check)) {
                if (reuse_mismatches.fetch_add(1) < 16) {
                    LOG_ERROR(Render_Vulkan,
                              "tlg: variant reuse of shader {:#x} perm {} differs from a fresh "
                              "specialization: {}",
                              params.hash, last.perm_idx, DescribeSpecDiff(check, hit.spec));
                }
            }
        }
        ++reuse_hits;
        program->last_used = last.perm_idx;
        info.AddBindings(binding);
        return std::make_tuple(&program->info, hit.module, hit.spec.fetch_shader_data,
                               HashCombine(params.hash, last.perm_idx));
    }
    if (reusable && ((++reuse_misses) & 0xffff) == 0) {
        LOG_INFO(Render_Vulkan, "tlg: variant reuse: {} hits, {} misses, {} mismatches",
                 reuse_hits.load(), reuse_misses.load(), reuse_mismatches.load());
    }
    const auto start_binding = binding;
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    auto& spec = sel.specialization_scratch[static_cast<u32>(sw_stage)];
    spec.Refresh(info, runtime_info, profile, binding);
#else
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
#endif

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    // tlg: consecutive draws of a program almost always use the same permutation.
    auto it = program->last_used < program->modules.size() &&
                      program->modules[program->last_used].spec == spec
                  ? program->modules.begin() + program->last_used
                  : std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it != program->modules.end()) {
        program->last_used = std::distance(program->modules.begin(), it);
    }
    if (it == program->modules.end() && !program->modules.empty()) {
        const size_t n = program->modules.size();
        if (n >= 16 && !(n & (n - 1)) && !program->pending) {
            LOG_WARNING(Render_Vulkan, "tlg: shader {:#x} lookup miss with {} permutations; vs last: {}",
                        params.hash, n, DescribeSpecDiff(spec, program->modules.back().spec));
        }
    }
    if (it == program->modules.end()) {
        if (program->pending || std::ranges::find(program->failed_permutations, spec) !=
                                    program->failed_permutations.end()) {
            return {};
        }
        perm_idx = NewPermutationIndex(*program, params.hash);
        perm_hash = HashCombine(params.hash, perm_idx);
        if (compile_async) {
            QueueShader(*program, hw_stage, sw_stage, params, runtime_info, binding, perm_idx, false,
                        std::move(spec));
            return {};
        }
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        try {
            module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);
        } catch (const Shader::UnsupportedShaderError& e) {
            LOG_ERROR(Render_Vulkan, "tlg: shader {:#x}/{} refused: {}", params.hash, perm_idx,
                      e.what());
            spec.info = &program->info;
            program->failed_permutations.push_back(std::move(spec));
            return {};
        }

        RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        std::unique_lock lk{programs_mutex};
        program->InsertPermut(module, std::move(spec), perm_idx);
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    if (reusable && perm_idx < program->modules.size()) {
        last.perm_idx = perm_idx;
        last.flat_buf = info.flattened_ud_buf;
        last.runtime_info = runtime_info;
        last.start = start_binding;
        vs_sharps_match(true);
    }
    return std::make_tuple(&program->info, module,
                           program->modules[perm_idx].spec.fetch_shader_data, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    PublishShaders(true);
    PublishGraphics(true);
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                fast_graphics.valid = false;
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
