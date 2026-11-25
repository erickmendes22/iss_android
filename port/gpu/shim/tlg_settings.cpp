// SPDX-License-Identifier: GPL-2.0-or-later
#include <cctype>
#include <chrono>
#include "iss_performance_state.h"
#include "tlg_settings.h"
#include "tlg_orbs.h"
#include "tlg_graphics.h"
#include "tlg_view.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace TlgSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("TLG_CONFIG");
    return env && env[0] ? env : "tlg.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    static const char* const overlay_keys[] = {"overlay_fps", "overlay_frametime", "overlay_upscaler",
                                               "overlay_gpu", "overlay_cpu", "overlay_ram", "overlay_vram"};
    for (unsigned item = 0; item < 7; ++item) {
        if (key == overlay_keys[item]) {
            const unsigned bit = 1u << item;
            v.overlay_items = i ? (v.overlay_items | bit) : (v.overlay_items & ~bit);
            return;
        }
    }
    if (key == "overlay_scale") {
        v.overlay_scale = std::clamp(i, 50, 300);
        return;
    }
    if (key == "overlay_corner") {
        v.overlay_corner = i >= 0 && i <= 3 ? i : 1;
        return;
    }
    if (key == "overlay_opacity") {
        v.overlay_opacity = std::clamp(i, 0, 100);
        return;
    }
    if (key == "overlay_layout") {
        v.overlay_layout = i == 1 ? 1 : 0;
        return;
    }
    if (key == "menu_tab") {
        v.menu_tab = i >= 0 && i <= 3 ? i : 0;
        return;
    }
    const char* cheat_keys[] = {"cheat_health", "cheat_magic", "cheat_item", "cheat_rage", "cheat_orbs",
                                "cheat_very_hard", "cheat_double_jumps"};
    for (unsigned cheat = 0; cheat < TLG_CHEAT_COUNT; ++cheat) {
        if (key == cheat_keys[cheat]) {
            v.cheats[cheat] = i != 0;
            return;
        }
    }
    if (key.size() == 4 && key[0] == 'b' && key[1] == 'p' && std::isdigit((unsigned char)key[2]) &&
        std::isdigit((unsigned char)key[3])) {
        const int n = (key[2] - '0') * 10 + (key[3] - '0');
        if (n >= 1 && n <= kBackportPrefs && !value.empty()) {
            v.backport_pref[n - 1] = i != 0;
        }
        return;
    }
    for (int e = 0; e < kExtraPrefs; ++e) {
        if (key == kExtraKeys[e]) {
            v.extra_pref[e] = value.empty() ? -1 : i != 0;
            return;
        }
    }
    if (key == "key_map") {
        v.key_map = value;
        return;
    }
    if (key == "lod_bands") {
        // Six comma-separated distances; missing or bad entries keep the current value.
        const char* p = value.c_str();
        for (auto& band : v.lod_bands) {
            char* end = nullptr;
            const float b = std::strtof(p, &end);
            if (end == p) break;
            band = TlgView::ClampBand(b);
            p = *end == ',' ? end + 1 : end;
        }
        return;
    }
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "mouse_enabled") {
        v.mouse_enabled = i != 0;
    } else if (key == "mouse_invert_y") {
        v.mouse_invert_y = i != 0;
    } else if (key == "mouse_sensitivity") {
        v.mouse_sensitivity = Clamp(value.empty() ? 1.0f : f, 0.05f, 10.0f);
    } else if (key == "mouse_smoothing") {
        v.mouse_smoothing = Clamp(f, 0.0f, 0.95f);
    } else if (key == "mouse_accel") {
        v.mouse_accel = Clamp(f, 0.0f, 2.0f);
    } else if (key == "mouse_deadzone") {
        v.mouse_deadzone = Clamp(f, 0.0f, 0.5f);
    } else if (key == "sensor_aim") {
        v.sensor_aim = std::clamp(i, 25, 200);
    } else if (key == "graffiti_guide_seen") {
        v.graffiti_guide_seen = i != 0;
    } else if (key == "display_mode") {
        v.display_mode = TlgGraphics::ParseDisplayMode(value);
    } else if (key == "vsync") {
        v.vsync = i != 0;
    } else if (key == "fps_limit") {
        v.fps_limit = TlgGraphics::ParseFpsLimit(i);
    } else if (key == "async_shaders") {
        v.async_shaders = i != 0;
    } else if (key == "fsr1") {
        v.fsr1 = i != 0;
    } else if (key == "rcas") {
        v.rcas = i != 0;
    } else if (key == "rcas_strength") {
        v.rcas_strength = TlgGraphics::ParseRcasStrength(value.empty() ? -1 : i);
    } else if (key == "frames_queued") {
        v.frames_queued = TlgGraphics::ParseFramesQueued(value.empty() ? -1 : i);
    } else if (key == "render_resolution") {
        v.render_resolution = TlgGraphics::ParseResolution(value);
    } else if (key == "engine_fps") {
        v.engine_fps = TlgGraphics::ParseEngineFps(i);
    } else if (key == "deferred_readback") {
        v.deferred_readback = i != 0;
    } else if (key == "stale_readback") {
        v.stale_readback = i != 0;
    } else if (key == "restart_unconfirmed") {
        v.restart_unconfirmed = i != 0;
    } else if (key == "enemy_health_bar") {
        v.enemy_health_bar = i != 0;
    } else if (key == "red_orb_multiplier") {
        v.red_orb_multiplier = TlgOrbs::ClampMultiplier(f);
    } else if (key == "damage_dealt") {
        v.damage_dealt = TlgOrbs::ClampMultiplier(f);
    } else if (key == "damage_taken") {
        v.damage_taken = TlgOrbs::ClampMultiplier(f);
    } else if (key == "green_orb_multiplier") {
        v.green_orb_multiplier = TlgOrbs::ClampMultiplier(f);
    } else if (key == "blue_orb_multiplier") {
        v.blue_orb_multiplier = TlgOrbs::ClampMultiplier(f);
    } else if (key == "gold_orb_multiplier") {
        v.gold_orb_multiplier = TlgOrbs::ClampMultiplier(f);
    } else if (key == "lod_scale") {
        v.lod_scale = TlgView::ClampLodScale(f);
    } else if (key == "region_lod") {
        v.region_lod = TlgView::ClampRegionLod(f);
    } else if (key == "region_fade") {
        v.region_fade = TlgView::ClampRegionFade(f);
    } else if (key == "near_clip") {
        v.near_clip = TlgView::ClampNearClip(f);
    } else if (key == "far_clip") {
        v.far_clip = TlgView::ClampFarClip(f);
    } else if (key == "mip_lod_bias") {
        v.mip_lod_bias = TlgView::ClampMipLodBias(f);
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void PollAndroidPerformance() {
#ifdef ISS_ANDROID_BIONIC
    static const char* path=std::getenv("ISS_PERFORMANCE_STATE_FILE");
    if(!path || !*path)return;
    static auto last=std::chrono::steady_clock::time_point{};
    const auto now=std::chrono::steady_clock::now();
    if(now-last<std::chrono::milliseconds(250))return;
    last=now;
    IssPerformance::State state;
    if(IssPerformance::Read(path,state)) {
        auto& v=Get();
        if(v.fps_limit!=state.fps || v.vsync!=state.vsync)
            std::printf("Android performance: limit=%d FPS, vsync=%d\n",state.fps,int(state.vsync));
        v.fps_limit=state.fps;v.vsync=state.vsync;v.auto_quality=state.auto_quality;
    }
#endif
}

void Load() {
    auto& v = Get();
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[1024]; // key_map can be long
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("TLG_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"TLG_FSR_SHARPNESS", "sharpness"},        {"TLG_FSR_SHARPEN", "sharpen"},
        {"TLG_JITTER", "jitter"},
        {"TLG_REACTIVE", "reactive"},              {"TLG_REACTIVE_SCALE", "reactive_scale"},
        {"TLG_REACTIVE_THRESHOLD", "reactive_threshold"}, {"TLG_REACTIVE_MAX", "reactive_max"},
        {"TLG_UPSCALE_PRESET", "preset"},            {"TLG_OBJECT_MOTION", "object_motion"},
        {"TLG_ASYNC_SHADERS", "async_shaders"},      {"TLG_DEFERRED_READBACK", "deferred_readback"},
        {"TLG_STALE_READBACK", "stale_readback"},     {"TLG_FSR1", "fsr1"},
        {"TLG_RCAS", "rcas"},                        {"TLG_FRAMES_AHEAD", "frames_queued"},
        {"TLG_SHOW_FPS", "show_fps"},           {"TLG_OUTPUT_RES", "output_res"},
        {"TLG_SENSOR_AIM", "sensor_aim"},
        {"TLG_MOUSE", "mouse_enabled"},           {"TLG_MOUSE_SENS", "mouse_sensitivity"},
        {"TLG_MOUSE_INVERT_Y", "mouse_invert_y"}, {"TLG_MOUSE_SMOOTHING", "mouse_smoothing"},
        {"TLG_MOUSE_ACCEL", "mouse_accel"},       {"TLG_MOUSE_DEADZONE", "mouse_deadzone"},
        {"TLG_LOD_SCALE", "lod_scale"},           {"TLG_LOD_BANDS", "lod_bands"},
        {"TLG_REGION_LOD", "region_lod"},         {"TLG_REGION_FADE", "region_fade"},
        {"TLG_NEAR_CLIP", "near_clip"},           {"TLG_FAR_CLIP", "far_clip"},
        {"TLG_MIP_LOD_BIAS", "mip_lod_bias"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env); value && value[0]) {
            Set(v, key, value);
        }
    }
    // Android launcher uses the game's actual presenter FPS, not the
    // Android/Xvfb bridge refresh rate. Show only FPS for a compact HUD.
    if (const char* android = std::getenv("TLG_ANDROID_FPS_ONLY"); android && *android == '1') {
        v.overlay_items = TlgPerfStats::Fps;
        v.overlay_corner = 1;
        v.overlay_scale = 140;
    }
#ifdef ISS_ANDROID_BIONIC
    // Only the Android launcher's explicit FSR 3.1 choice may activate temporal work.
    // This also prevents stale desktop tlg.ini settings from enabling unsupported providers.
    const char* android_fsr = std::getenv("TLG_UPSCALER");
    const bool fsr3 = android_fsr && std::strcmp(android_fsr,"fsr3")==0;
    v.upscaler = fsr3 ? UpscalerFsr3 : UpscalerOff;
    v.fsr1 = false;
    v.rcas = false;
    v.object_motion = fsr3;
    v.jitter = fsr3;
#endif
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_render_resolution = v.render_resolution;
    v.startup_engine_fps = v.engine_fps;
    const char* applied = std::getenv("TLG_ENGINE_FPS");
    v.applied_engine_fps = applied && applied[0] ? TlgGraphics::ParseEngineFps(std::atoi(applied))
                                                 : v.engine_fps.load();
    v.startup_deferred_readback = v.deferred_readback;
    v.startup_stale_readback = v.stale_readback;
}

bool GraphicsNeedRestart() {
    const auto& v = Get();
    return v.render_resolution != v.startup_render_resolution ||
           v.engine_fps != v.startup_engine_fps ||
           v.deferred_readback != v.startup_deferred_readback ||
           v.stale_readback != v.startup_stale_readback;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "tlg_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("TLG_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    const auto& v = Get();
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# tlg settings (in-game menu: Insert / R3+L2)\n"
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    std::fprintf(file, "output_res=%dx%d\n", OutputWidths[v.output_res], OutputHeights[v.output_res]);
    std::fprintf(file, "red_orb_multiplier=%.3f\n", v.red_orb_multiplier.load());
    std::fprintf(file, "damage_dealt=%.3f\n", v.damage_dealt.load());
    std::fprintf(file, "damage_taken=%.3f\n", v.damage_taken.load());
    std::fprintf(file, "green_orb_multiplier=%.3f\n", v.green_orb_multiplier.load());
    std::fprintf(file, "blue_orb_multiplier=%.3f\n", v.blue_orb_multiplier.load());
    std::fprintf(file, "gold_orb_multiplier=%.3f\n", v.gold_orb_multiplier.load());
    std::fprintf(file, "cheat_health=%d\ncheat_magic=%d\ncheat_item=%d\ncheat_rage=%d\ncheat_orbs=%d\n"
                       "cheat_very_hard=%d\ncheat_double_jumps=%d\n",
                 int(v.cheats[0].load()), int(v.cheats[1].load()), int(v.cheats[2].load()),
                 int(v.cheats[3].load()), int(v.cheats[4].load()),
                 int(v.cheats[5].load()), int(v.cheats[6].load()));
    std::fprintf(file, "enemy_health_bar=%d\n", int(v.enemy_health_bar.load()));
    std::fprintf(file,
                 "overlay_fps=%d\noverlay_frametime=%d\noverlay_upscaler=%d\noverlay_gpu=%d\n"
                 "overlay_cpu=%d\noverlay_ram=%d\noverlay_vram=%d\noverlay_scale=%d\noverlay_corner=%d\n"
                 "overlay_opacity=%d\noverlay_layout=%d\nmenu_tab=%d\n",
                 int(v.overlay_items >> 0 & 1), int(v.overlay_items >> 1 & 1), int(v.overlay_items >> 2 & 1),
                 int(v.overlay_items >> 3 & 1), int(v.overlay_items >> 4 & 1), int(v.overlay_items >> 5 & 1),
                 int(v.overlay_items >> 6 & 1), v.overlay_scale.load(), v.overlay_corner.load(),
                 v.overlay_opacity.load(), v.overlay_layout.load(), v.menu_tab.load());
    std::fprintf(file,
                 "display_mode=%s\nvsync=%d\nfps_limit=%d\nasync_shaders=%d\n"
                 "fsr1=%d\nrcas=%d\nrcas_strength=%d\nframes_queued=%d\n"
                 "render_resolution=%s\nengine_fps=%d\ndeferred_readback=%d\nstale_readback=%d\n"
                 "restart_unconfirmed=%d\n",
                 TlgGraphics::DisplayModeKeys[std::clamp(v.display_mode.load(), 0, 2)].data(),
                 int(v.vsync.load()), v.fps_limit.load(), int(v.async_shaders.load()),
                 int(v.fsr1.load()), int(v.rcas.load()), v.rcas_strength.load(), v.frames_queued.load(),
                 TlgGraphics::Resolutions[std::clamp(v.render_resolution.load(), 0, 5)].data(),
                 v.engine_fps.load(), int(v.deferred_readback.load()), int(v.stale_readback.load()),
                 int(v.restart_unconfirmed.load()));
    std::fprintf(file, "sensor_aim=%d\ngraffiti_guide_seen=%d\n", v.sensor_aim.load(),
                 int(v.graffiti_guide_seen.load()));
    std::fprintf(file,
                 "lod_scale=%.3f\nregion_lod=%.3f\nregion_fade=%.3f\nnear_clip=%.3f\nfar_clip=%.0f\n"
                 "mip_lod_bias=%.3f\nlod_bands=%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n",
                 v.lod_scale.load(), v.region_lod.load(), v.region_fade.load(), v.near_clip.load(),
                 v.far_clip.load(), v.mip_lod_bias.load(), v.lod_bands[0].load(), v.lod_bands[1].load(),
                 v.lod_bands[2].load(), v.lod_bands[3].load(), v.lod_bands[4].load(), v.lod_bands[5].load());
    // Fixes: applied on the next start by the launcher (TLG_BPnn / TLG_<NAME>); only the ones
    // the player or the launcher chose are written.
    for (int n = 0; n < kBackportPrefs; ++n) {
        if (const int p = v.backport_pref[n]; p >= 0) std::fprintf(file, "bp%02d=%d\n", n + 1, p);
    }
    for (int e = 0; e < kExtraPrefs; ++e) {
        if (const int p = v.extra_pref[e]; p >= 0) std::fprintf(file, "%s=%d\n", kExtraKeys[e], p);
    }
    std::fprintf(file,
                 "mouse_enabled=%d\nmouse_sensitivity=%.2f\nmouse_invert_y=%d\nmouse_smoothing=%.2f\n"
                 "mouse_accel=%.2f\nmouse_deadzone=%.2f\nkey_map=%s\n",
                 int(v.mouse_enabled.load()), v.mouse_sensitivity.load(), int(v.mouse_invert_y.load()),
                 v.mouse_smoothing.load(), v.mouse_accel.load(), v.mouse_deadzone.load(), v.key_map.c_str());
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    std::fclose(file);
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace TlgSettings
