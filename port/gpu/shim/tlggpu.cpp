#include "tlg_write_log.h"
// tlg: glue between the C loader and the vendored shadPS4 video core.
#include "tlg_overlay.h"
#include "tlg_png.h"
#include "tlg_settings.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include "tlg_copy.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "tlg_region.h"
#include "tlg_toggles.h"
#include "tlg_freeze.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/backports.h"
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../tlggpu.h"
#include "iss_scene_camera.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/emulator_settings.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
int runtime_memory_write_backing_cpu(uintptr_t address, const void* data, uint64_t size);
int runtime_memory_cpu_write_can_queue(void);
int runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
void runtime_prepare_short_sleep(uint64_t ns);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
uint64_t runtime_read_tsc(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
// runtime.c: exit counters (and the ISA site report), as on the guest's own exit
void runtime_report(void);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const TlgGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        // tlg: read through the unprotected backing view so readback protection (Precise
        // readbacks) cannot fault on the recorder/copy threads; direct read as fallback.
        if (!mapped) std::memset(dest, 0, n);
        else if (!runtime_memory_read_backing(source, dest, n))
            std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // tlg: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !TlgCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    TlgCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    TlgWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, TlgWriteLog::Backing);
    const bool written =
        runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
    // tlg (gow3_pc #331): the write skipped page tracking, so textures over it must reload.
    // Fence and label writes (<= 8 bytes) never hold texels and are left out: each one would send
    // every texture binding through the locked path.
    if (written && rasterizer && size > 8) {
        rasterizer->GetTextureCache().NoteBackingWrite(reinterpret_cast<VAddr>(address), size);
    }
    return written;
}
bool MemoryManager::TryCpuWriteBacking(void* address, const void* data, u64 size) {
    return runtime_memory_write_backing_cpu(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    TlgStats::Timer timer{TlgStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    TlgStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    TlgStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        TlgStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        TlgStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return runtime_read_tsc(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    runtime_prepare_short_sleep(uint64_t{microseconds} * 1000);
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

u32 TlgDisplayRefreshHz() {
    static const u32 hz = [] {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        const u32 rate = mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
        return std::max<u32>(rate, 60);
    }();
    return hz;
}

#ifdef TLG_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (TLG_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("tlg:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

// tlg: what is switched on for this player, written at the top of every log: the tlg.ini keys (the
// launcher's choices and the in-game menu's) and the TLG_/ISS_ variables set in the environment
// (paths are left out: logs are posted in public issues).
static void LogPlayerConfig() {
    std::printf("Config: ---- player configuration ----\n");
    const char* cfg = std::getenv("TLG_CONFIG");
    std::ifstream ini(cfg && *cfg ? cfg : "tlg.ini");
    std::string line;
    int n = 0;
    while (ini && std::getline(ini, line) && n < 300) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#' || line.find('=') == std::string::npos) continue;
        std::printf("Config ini: %s\n", line.c_str());
        ++n;
    }
    if (n == 0) std::printf("Config ini: (empty or missing: every setting at its default)\n");
    std::vector<std::string> vars;
#ifdef _WIN32
    char** envp = _environ;
#else
    extern char** environ;
    char** envp = environ;
#endif
    for (char** e = envp; e && *e; ++e) {
        const std::string entry = *e;
        if (entry.rfind("TLG_", 0) != 0 && entry.rfind("ISS_", 0) != 0) continue;
        const auto eq = entry.find('=');
        if (eq == std::string::npos) continue;
        const std::string value = entry.substr(eq + 1);
        if (value.find('/') != std::string::npos || value.find('\\') != std::string::npos || value.size() > 40) continue;
        vars.push_back(entry);
    }
    std::sort(vars.begin(), vars.end());
    for (const auto& v : vars) std::printf("Config env: %s\n", v.c_str());
    if (vars.empty()) std::printf("Config env: (no TLG_/ISS_ variable set)\n");
    std::printf("Config: ----\n");
}

// tlg: the part of tlggpu_init that the shader precompiler needs without a window or the game:
// the player's settings (the backports chosen there change the shader cache version) and the
// game's identity (some recompiler switches are on by default for this serial).
extern "C" void tlggpu_init_headless(const TlgGpuConfig* config) {
    TlgSettings::Load();
    {
        const auto& settings = TlgSettings::Get();
        for (int i = 0; i < TlgSettings::kBackportPrefs; ++i) {
            const int pref = settings.backport_pref[i].load();
            if (pref >= 0) {
                Common::Backports::ApplyConfig(static_cast<std::size_t>(i), pref != 0);
            }
        }
    }
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir && !std::getenv("TLG_GPU_USER_DIR")) _putenv_s("TLG_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("TLG_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
}

extern "C" int tlggpu_init(const TlgGpuConfig* config) {
    TlgSettings::Load();
    // tlg: the fixes chosen in the launcher or the in-game menu (bpNN in tlg.ini); an environment
    // variable TLG_BPnn / ISS_BPnn still wins.
    {
        const auto& settings = TlgSettings::Get();
        for (int i = 0; i < TlgSettings::kBackportPrefs; ++i) {
            const int pref = settings.backport_pref[i].load();
            if (pref >= 0) {
                Common::Backports::ApplyConfig(static_cast<std::size_t>(i), pref != 0);
            }
        }
    }
    LogPlayerConfig();
#ifdef TLG_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir && !std::getenv("TLG_GPU_USER_DIR")) _putenv_s("TLG_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("TLG_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "God of War III";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("tlg:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        runtime_report();
        std::fflush(stdout);
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void tlggpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t tlggpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int tlggpu_handle_fault(void* ucontext, void* address) {
    // tlg: the freeze report shows threads stuck in a page-protection fault (and how many).
    TlgFreeze::faults_in_progress.fetch_add(1, std::memory_order_relaxed);
    int handled;
    {
        TlgFreeze::WaitScope wait{"GPU page fault handler", address};
        handled = Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
    }
    TlgFreeze::faults_in_progress.fetch_sub(1, std::memory_order_relaxed);
    TlgFreeze::faults_handled.fetch_add(handled, std::memory_order_relaxed);
    return handled;
}

extern "C" void tlggpu_set_wait_hooks(TlgGpuWaitBegin begin, TlgGpuWaitEnd end) {
    TlgFreeze::end_hook.store(end);
    TlgFreeze::begin_hook.store(begin);
}

extern "C" void tlggpu_note_open(const char* guest_path) {
    TlgRegion::NoteOpen(guest_path);
}

extern "C" void tlggpu_frame_counters(TlgGpuFrameCounters* out) {
    out->submitted = TlgFreeze::flips_submitted.load(std::memory_order_relaxed);
    out->shown = TlgFreeze::flips_shown.load(std::memory_order_relaxed);
    out->presented = TlgFreeze::presents.load(std::memory_order_relaxed);
    out->vblanks = TlgFreeze::vblanks.load(std::memory_order_relaxed);
    out->faults_in_progress = TlgFreeze::faults_in_progress.load(std::memory_order_relaxed);
    out->faults_handled = TlgFreeze::faults_handled.load(std::memory_order_relaxed);
}

// FEX pushes a completed camera snapshot here from the guest update routine.
// No address survives the call: the four matrices and scalar fields are copied.
// A submission copies the bounded bank under the same lock. Last publication
// is not ownership: G-buffer material transforms select the actual scene camera.
namespace {
std::mutex iss_camera_mutex;
TlgIssCameraSnapshot iss_camera_latest{};
IssSceneCamera::Bank iss_camera_bank;
}

extern "C" void tlggpu_iss_camera_publish(const TlgIssCameraSnapshot* sample) {
    if (!sample) return;
    std::lock_guard lock{iss_camera_mutex};
    const uint64_t next = iss_camera_latest.sequence + 1;
    iss_camera_latest = *sample;
    iss_camera_latest.sequence = next;
    iss_camera_bank.Publish(iss_camera_latest);
}

extern "C" uint64_t tlggpu_iss_camera_read(TlgIssCameraSnapshot* sample) {
    if (!sample) return 0;
    std::lock_guard lock{iss_camera_mutex};
    *sample = iss_camera_latest;
    return sample->sequence;
}

extern "C" uint64_t tlggpu_iss_camera_read_set(TlgIssCameraSet* samples) {
    if (!samples) return 0;
    std::lock_guard lock{iss_camera_mutex};
    *samples = iss_camera_bank.Read();
    return samples->sequence;
}

extern "C" unsigned tlggpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("tlg:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" int tlggpu_cpu_store(void* dst, const void* bytes, uint64_t size) {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    auto* r = Core::Memory::Instance()->GetRasterizer();
    if (!r || size != 16 || (reinterpret_cast<VAddr>(dst) & 15) ||
        reinterpret_cast<VAddr>(dst) >= (1ULL << 40) ||
        EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Relaxed ||
        EmulatorSettings.IsDirectMemoryAccessEnabled()) return 0;
    if (!r->GetBufferCache().IsRegionGpuModified(reinterpret_cast<VAddr>(dst), size) ||
        r->IsGpuSideThread() || !runtime_memory_cpu_write_can_queue()) return 0;
    return r->GetBufferCache().CpuStoreOverwrite(reinterpret_cast<VAddr>(dst), bytes, size);
#else
    return 0;
#endif
}

extern "C" int tlggpu_cpu_write(void* dst, const void* src, int fill, uint64_t size) {
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    if (rasterizer && EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed) {
        const auto address = reinterpret_cast<VAddr>(dst);
        // The tracker indexes only the guest 40-bit VA space. Validate before querying it.
        if (!size || address >= (1ULL << 40) || size > (1ULL << 40) - address) return 0;
        // Most libc writes are ordinary RAM. Avoid a thread-id syscall for those.
        if (!rasterizer->GetBufferCache().IsRegionGpuModified(address, size)) return 0;
        if (rasterizer->IsGpuSideThread() || !runtime_memory_cpu_write_can_queue()) return 0;
    }
#endif
    return rasterizer && rasterizer->GetBufferCache().CpuWriteMerge(
                             reinterpret_cast<VAddr>(dst), src, fill, size)
               ? 1
               : 0;
}

extern "C" void tlggpu_overlay_notice(const char* text, float seconds) {
    TlgOverlay::Notice(text ? text : "", seconds);
}

extern "C" int tlggpu_overlay_captures_input(void) {
    return TlgOverlay::CapturesInput() ? 1 : 0;
}

extern "C" void tlggpu_input_settings(TlgGpuInputSettings* out) {
    const auto& s = TlgSettings::Get();
    out->mouse_enabled = s.mouse_enabled ? 1 : 0;
    out->invert_y = s.mouse_invert_y ? 1 : 0;
    out->sensitivity = s.mouse_sensitivity;
    out->smoothing = s.mouse_smoothing;
    out->accel = s.mouse_accel;
    out->deadzone = s.mouse_deadzone;
    std::snprintf(out->key_map, sizeof(out->key_map), "%s", s.key_map.c_str());
}

extern "C" void tlggpu_mouse_pad_hud(int active, float x, float y, int touching, int clicking) {
    TlgOverlay::SetMousePad(active != 0, x, y, touching != 0, clicking != 0);
}

extern "C" int tlggpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

static TlgOverlay::SaveRow SaveRowOf(const TlgSaveEntry& entry) {
    TlgOverlay::SaveRow row;
    row.title = entry.title ? entry.title : "";
    row.subtitle = entry.subtitle ? entry.subtitle : "";
    row.detail = entry.detail ? entry.detail : "";
    row.mtime = entry.mtime;
    row.bytes = entry.bytes;
    row.is_new = entry.is_new != 0;
    return row;
}

static TlgPng::Image SavePicture(const char* path) {
    TlgPng::Image picture;
    if (path && !TlgPng::LoadRgba(path, picture)) {
        LOG_WARNING(Lib_SaveDataDialog, "save dialog: no picture from {}", path);
    }
    return picture;
}

extern "C" int tlggpu_save_list_begin(int kind, const TlgSaveEntry* entries, int count, int focus) {
    if (!g_window || count <= 0 || !entries) return 0;
    // Pictures are decoded here, on the game's thread, before the list is shown.
    std::vector<TlgOverlay::SaveRow> rows;
    std::vector<TlgPng::Image> pictures;
    for (int i = 0; i < count; ++i) {
        rows.push_back(SaveRowOf(entries[i]));
        pictures.push_back(SavePicture(entries[i].icon));
    }
    TlgOverlay::BeginSaveList(kind, std::move(rows), std::move(pictures), focus);
    return 1;
}

extern "C" int tlggpu_save_list_poll(void) {
    return TlgOverlay::PollSaveList();
}

extern "C" int tlggpu_save_message_begin(int kind, int code, uint64_t value, const char* text,
                                         int buttons, const TlgSaveEntry* entry) {
    if (!g_window) return 0;
    TlgOverlay::SaveRow row;
    if (entry) row = SaveRowOf(*entry);
    TlgOverlay::BeginSaveMessage(kind, code, value, text ? text : "", buttons, entry ? &row : nullptr,
                                 SavePicture(entry ? entry->icon : nullptr));
    return 1;
}

extern "C" int tlggpu_save_message_poll(void) {
    return TlgOverlay::PollSaveMessage();
}

extern "C" void tlggpu_save_dialog_close(void) {
    TlgOverlay::CloseSaveDialog();
}

extern "C" void tlggpu_save_progress(int kind, int active, int percent, const char* text) {
    if (g_window) TlgOverlay::SaveProgress(kind, active, percent, text);
}

extern "C" float tlggpu_sensor_aim_scale(void) {
    return float(TlgSettings::Get().sensor_aim.load()) / 100.0f;
}

extern "C" void tlggpu_graffiti_guide(int force) {
    if (g_window) TlgOverlay::ShowGraffitiGuide(force != 0);
}

extern "C" void tlggpu_save_toast(int kind) {
    if (g_window) TlgOverlay::SaveToast(kind);
}

extern "C" int tlggpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
