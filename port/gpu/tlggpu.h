/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef TLGGPU_H
#define TLGGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} TlgGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void tlggpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int tlggpu_init(const TlgGpuConfig *config);
/* The settings and game identity of tlggpu_init, without a window (shader precompiler). */
void tlggpu_init_headless(const TlgGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t tlggpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int tlggpu_handle_fault(void *ucontext, void *address);
/* Native store overwrite: accepts only pages whose GPU-dirty bytes are entirely replaced. */
int tlggpu_cpu_store(void *destination, const void *bytes, uint64_t size);
/* TLG_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void tlggpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int tlggpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int tlggpu_text_input_poll(char *out_utf8, uint64_t size);
/* The save dialogs (sceSaveDataDialog), drawn over the game. kind: 1 save, 2 load, 3 delete
 * (the dialog's dispType); 0 for the toast of a save the game made without a dialog. */
typedef struct TlgSaveEntry {
    const char *title, *subtitle, *detail; /* the game's text for the save (UTF-8) */
    const char *icon;                      /* PNG path drawn beside it, or NULL */
    int64_t mtime;                         /* when it was saved (0: unknown) */
    uint64_t bytes;                        /* size on disk (0: unknown) */
    int is_new;                            /* the "New save" row */
} TlgSaveEntry;
/* The list. begin: 0 when no window exists. poll: -1 while open, -2 cancelled, else the index. */
int tlggpu_save_list_begin(int kind, const TlgSaveEntry *entries, int count, int focus);
int tlggpu_save_list_poll(void);
/* A message the player answers. code: a system message type (1 no data ... 14 too large), 100 the
 * game's own text, 101 an error code (value). buttons: 0 OK, 1 yes/no, 3 OK/cancel, -1 the
 * code's own. entry: the save it is about, or NULL. begin: 0 when no window exists.
 * poll: -1 while open, 1 yes/OK, 2 no/cancel. */
int tlggpu_save_message_begin(int kind, int code, uint64_t value, const char *text, int buttons,
                              const TlgSaveEntry *entry);
int tlggpu_save_message_poll(void);
/* The game closed its dialog: the list or message goes away unanswered. */
void tlggpu_save_dialog_close(void);
/* "Saving..." panel: active 1 shows it (percent < 0: no bar; text NULL keeps the last), 0 hides
 * it; active 2 shows it briefly, for dialogs the port answers at once. */
void tlggpu_save_progress(int kind, int active, int percent, const char *text);
/* Sensor mode's aiming speed from the menu's Controls tab (1.0 = default). */
float tlggpu_sensor_aim_scale(void);
/* The graffiti guide over the game: the first time only (force 0), or again (force 1). */
void tlggpu_graffiti_guide(int force);
/* "Game saved" in the corner (repeats within a few seconds are dropped). */
void tlggpu_save_toast(int kind);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int tlggpu_overlay_captures_input(void);
/* A short message at the top of the screen (any thread). */
void tlggpu_overlay_notice(const char *text, float seconds);
/* Guest memcpy (src) / memset (src NULL, fill) into GPU-modified memory without a fault or a
 * GPU wait (TLG_WRITE_MERGE=1); 0: the caller writes normally. */
int tlggpu_cpu_write(void *dst, const void *src, int fill, uint64_t size);
/* Freeze report (src/runtime_freeze.c): the GPU library marks its blocking calls (event queue,
 * GPU idle and vblank waits, GPU thread commands, readbacks, page faults) through these hooks. */
typedef void (*TlgGpuWaitBegin)(const char *kind, const void *object, const char *name, uintptr_t frame);
typedef void (*TlgGpuWaitEnd)(void);
void tlggpu_set_wait_hooks(TlgGpuWaitBegin begin, TlgGpuWaitEnd end);
/* Flip requests from the game, flips handed to the presenter, presents done, vblank ticks,
 * page-protection faults being handled now and handled so far. */
typedef struct { uint64_t submitted, shown, presented, vblanks, faults_in_progress, faults_handled; } TlgGpuFrameCounters;
void tlggpu_frame_counters(TlgGpuFrameCounters *out);
/* Camera fields captured at a verified inFAMOUS x86-64 camera-update site.
 * The FEX hook copies these values before the game resumes. Graphics submission
 * copies them again into its owned command context; FSR never reads the live
 * CPU publisher while executing older commands. */
typedef struct {
    uint64_t sequence;
    uint64_t guest_object;
    float projection[16];      /* game camera +0x1c0 */
    float view_transform[16];  /* game camera +0x200 */
    float composite[16];       /* game camera +0x430 */
    float inverse[16];         /* game camera +0x4b0 */
    float near_plane, far_plane, vertical_fov, aspect;
    float tangent_horizontal, tangent_vertical;
} TlgIssCameraSnapshot;
/* Called from guest CPU only; does not retain any guest pointer. */
void tlggpu_iss_camera_publish(const TlgIssCameraSnapshot *sample);
/* Read at graphics submission, before asynchronous execution. Returns sequence
 * (>0), or 0 if absent. The caller must retain its own value, not guest pointers. */
uint64_t tlggpu_iss_camera_read(TlgIssCameraSnapshot *sample);
/* The game opened a file (guest path): feeds the region trace (TLG_REGION_TRACE=1, gpu/shim/tlg_region.h). */
void tlggpu_note_open(const char *guest_path);
/* The mouse, gathered by the window thread while the game has it (SDL relative mode: the window
 * has focus, the port's menu is closed and Esc did not release it; a click takes it again).
 * take: motion in mouse counts and wheel notches since the last call, then cleared. Buttons are
 * bits (1 << (SDL button - 1)): held now, and pressed since the last call (a short click). */
typedef struct {
    float dx, dy;
    uint32_t held, pressed;
    int32_t wheel_up, wheel_down, wheel_left, wheel_right;
    int32_t captured;
} TlgGpuMouse;
void tlggpu_mouse_take(TlgGpuMouse *mouse);
/* Mouse and keyboard settings (tlg.ini; the menu changes the mouse ones live). key_map: the
 * tlg.ini key_map value, the TLG_KEY_MAP syntax (runtime_pad.c), read once at start. */
typedef struct {
    int32_t mouse_enabled, invert_y;
    float sensitivity, smoothing, accel, deadzone;
    char key_map[512];
} TlgGpuInputSettings;
void tlggpu_input_settings(TlgGpuInputSettings *settings);
/* Touchpad mode of the mouse (runtime_pad.c): while active, a small touchpad with the virtual
 * finger is drawn in the corner (x, y: 0..1 over the pad; touching: the game sees the finger;
 * clicking: the pad is pressed). */
void tlggpu_mouse_pad_hud(int active, float x, float y, int touching, int clicking);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned tlggpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
