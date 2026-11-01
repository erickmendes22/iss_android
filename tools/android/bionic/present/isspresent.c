// Adapted from Bloodborne-Android 2e472653.
/* libisspresent.so: the app's end of the Bionic game's frame path (tools/android/present/
 * iss_present_proto.h). Listens on a unix socket, receives AHardwareBuffers and frames from the
 * game process and shows them with an ASurfaceControl child of the SurfaceView, scaled to fit.
 *
 * minSdk is 28: ASurfaceControl / ASurfaceTransaction (API 29) are resolved with dlsym; without
 * them frames are dropped (released at once) and start() says so. All state is behind one mutex.
 * The receive thread owns the socket; the display callbacks (any thread) answer RELEASE.
 *
 * Two presentation modes, chosen by the game in HELLO (ISS_PRESENT_FLAG_FIFO; user-selected VSync) and changed by its MODE message while it runs (set_fifo):
 *
 *  - Newest wins ("VSync off", the default): present_frame() applies a transaction per FRAME at once.
 *    SurfaceFlinger latches at most one buffer per refresh, so frames that arrive within one refresh
 *    replace each other: the newest is shown, the others never are (their buffers come back through
 *    on_complete as the "replaced" buffer). No tearing, lowest latency.
 *
 *  - FIFO ("VSync on"): at most ONE transaction is outstanding. A FRAME that arrives while the previous
 *    transaction is outstanding waits in a queue; when that one is done, the next queued frame is
 *    applied. So each frame is on screen for at least one refresh, in order, and with the display
 *    slower than the game the game is paced by the refresh rate: the
 *    queue is bounded (a frame in it is a buffer the game does not get back, and the game holds one
 *    to render, so with `count` buffers the app holds at most count of them: the one on the display,
 *    the outstanding one and count - 2 queued; the array has room for ISS_PRESENT_MAX_BUFFERS).
 *
 * "Done" means LATCHED, not presented: ASurfaceTransaction_setOnComplete fires after the frame has been
 * presented (its present fence), which is already past the latch deadline of the next refresh, so
 * gating the next transaction on it would show one frame every 2nd refresh (30 FPS on 60 Hz).
 * ASurfaceTransaction_setOnCommit (API 31) fires when SurfaceFlinger latches the transaction, early
 * in the refresh before the one that shows it; the next transaction applied right then is latched for
 * the following refresh. So with setOnCommit (resolved with dlsym) the commit callback clears the
 * outstanding transaction and the complete callback only answers RELEASE, as in the other mode. On
 * API 29/30 there is no setOnCommit: on_complete does both, and FIFO pacing may halve the rate there
 * (logged once).
 *
 * Who applies the next queued transaction: the receive thread, never a callback. The callbacks run on
 * a binder thread inside libgui's transaction-callback dispatch; whether applying another transaction
 * (which registers callbacks with the same TransactionCompletedListener) from inside one is safe
 * depends on its locking, which the NDK does not promise. So the callback only marks the transaction
 * done and wakes the receive thread through an eventfd; the receive thread polls the socket and the
 * eventfd, and applies from there (under the mutex like everything else). Cost: one thread wake-up
 * (well under a millisecond, far below the refresh interval).
 *
 * Callback ownership: each transaction has a DoneCtx (on_complete, frees it) and, in FIFO mode with
 * setOnCommit, a CommitCtx of its own (on_commit, frees it): each callback fires exactly once per
 * applied transaction, so each context has one owner and nothing is shared or counted. Both carry the
 * session and the transaction id; a callback of an older session or transaction does not touch
 * S.inflight.
 */
#include "geometry.h"
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "iss_present_proto.h"

#define TAG "IssPresent"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* API 29 entry points, from libandroid.so. */
static struct {
    ASurfaceControl* (*create_from_window)(ANativeWindow*, const char*);
    void (*release)(ASurfaceControl*);
    ASurfaceTransaction* (*txn_create)(void);
    void (*txn_delete)(ASurfaceTransaction*);
    void (*txn_apply)(ASurfaceTransaction*);
    void (*set_buffer)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int);
    void (*set_geometry)(ASurfaceTransaction*, ASurfaceControl*, const ARect*, const ARect*, int32_t);
    void (*set_visibility)(ASurfaceTransaction*, ASurfaceControl*, int8_t);
    void (*set_transparency)(ASurfaceTransaction*, ASurfaceControl*, int8_t);
    void (*set_on_complete)(ASurfaceTransaction*, void*, void (*)(void*, ASurfaceTransactionStats*));
    void (*set_on_commit)(ASurfaceTransaction*, void*, void (*)(void*, ASurfaceTransactionStats*)); /* API 31, optional */
    int (*prev_release_fence)(ASurfaceTransactionStats*, ASurfaceControl*);
    void (*get_controls)(ASurfaceTransactionStats*, ASurfaceControl***, size_t*);
    void (*release_controls)(ASurfaceControl**);
    int64_t (*get_latch_time)(ASurfaceTransactionStats*);
    void (*set_frame_rate)(ASurfaceTransaction*, ASurfaceControl*, float, int8_t); /* API 30, optional */
    int ok;
} api;

static void load_api(void) {
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    *(void**)&api.set_frame_rate = dlsym(lib, "ASurfaceTransaction_setFrameRate");
    *(void**)&api.set_on_commit = dlsym(lib, "ASurfaceTransaction_setOnCommit"); /* optional */
#define SYM(field, name) if (!(*(void**)&api.field = dlsym(lib, name))) return
    SYM(create_from_window, "ASurfaceControl_createFromWindow");
    SYM(release, "ASurfaceControl_release");
    SYM(txn_create, "ASurfaceTransaction_create");
    SYM(txn_delete, "ASurfaceTransaction_delete");
    SYM(txn_apply, "ASurfaceTransaction_apply");
    SYM(set_buffer, "ASurfaceTransaction_setBuffer");
    SYM(set_geometry, "ASurfaceTransaction_setGeometry");
    SYM(set_visibility, "ASurfaceTransaction_setVisibility");
    SYM(set_transparency, "ASurfaceTransaction_setBufferTransparency");
    SYM(set_on_complete, "ASurfaceTransaction_setOnComplete");
    SYM(prev_release_fence, "ASurfaceTransactionStats_getPreviousReleaseFenceFd");
    SYM(get_controls, "ASurfaceTransactionStats_getASurfaceControls");
    SYM(release_controls, "ASurfaceTransactionStats_releaseASurfaceControls");
#undef SYM
    *(void**)&api.get_latch_time = dlsym(lib, "ASurfaceTransactionStats_getLatchTime");
    api.ok = 1;
}

typedef struct {
    uint32_t generation, index, width, height;
    uint64_t frame;
    int fence; /* acquire fence, owned here; -1 none */
} Queued;

static struct {
    pthread_mutex_t lock;
    int started;
    int fullscreen;
    float frame_rate;
    int server_fd;
    char path[108];
    pthread_t thread;
    /* Display. */
    ANativeWindow* window;
    ASurfaceControl* sc;
    int last_set; /* buffer index of the newest transaction, -1 none */
    /* FIFO mode (the connection's HELLO flags): frames wait in queue[] while a transaction is
     * outstanding. inflight is the id (txn_seq) of the newest transaction not yet completed, 0 none;
     * on_complete of an older one (after a surface loss, say) does not clear it. */
    int fifo;
    Queued queue[ISS_PRESENT_MAX_BUFFERS];
    uint32_t queue_head, queue_len;
    uint64_t txn_seq, inflight;
    int wake_fd; /* eventfd: on_complete -> the receive thread (apply the next queued frame) */
    /* Connection (session ids tell the display callbacks which connection they belong to). */
    int conn_fd;
    uint32_t session;
    uint32_t generation;
    uint32_t count;
    AHardwareBuffer* bufs[ISS_PRESENT_MAX_BUFFERS];
    int trace_indices[ISS_PRESENT_MAX_BUFFERS];
    uint64_t trace_frames[ISS_PRESENT_MAX_BUFFERS];
    atomic_ullong frames;
} S = {.lock = PTHREAD_MUTEX_INITIALIZER, .server_fd = -1, .conn_fd = -1, .last_set = -1, .wake_fd = -1};

/* Requested capture only; S.lock serializes apply/complete file writes. */
static int64_t trace_clock_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static void trace_event(int index, const char* format, ...) {
    if (index < 0 || index >= 120) return;
    const char* slash = strrchr(S.path, '/'); if (!slash) return;
    char path[512];
    snprintf(path, sizeof(path), "%.*s/fsr-capture/f%03d_android_info.txt", (int)(slash-S.path), S.path, index);
    FILE* f = fopen(path, "ab"); if (!f) return;
    va_list args; va_start(args, format); vfprintf(f, format, args); va_end(args);
    fputc('\n', f); fclose(f);
}

/* ---- lock held for everything below unless noted ---- */

static void send_release(uint32_t generation, uint32_t index, uint64_t frame, int fence) {
    iss_present_msg m = iss_present_make(ISS_PRESENT_RELEASE);
    m.generation = generation;
    m.index = index;
    m.frame = frame;
    if (S.conn_fd >= 0) {
        int r = iss_present_send(S.conn_fd, &m, fence);
        if (r != 0) LOGE("release send failed: %d", r);
    }
    if (fence >= 0) close(fence);
}

/* Empties the FIFO queue. release: answer RELEASE for each frame with its own acquire fence (the
 * game's writes are what its buffer still waits for), as for a frame dropped for want of a display;
 * else the buffers are gone (RESET, new HELLO, connection closed): only the fences are closed. */
static void clear_queue(int release) {
    while (S.queue_len) {
        Queued* q = &S.queue[S.queue_head];
        if (release) send_release(q->generation, q->index, q->frame, q->fence);
        else if (q->fence >= 0) close(q->fence);
        S.queue_head = (S.queue_head + 1) % ISS_PRESENT_MAX_BUFFERS;
        S.queue_len--;
    }
    S.queue_head = 0;
}

static void drop_buffers(void) {
    clear_queue(0);
    S.inflight = 0;
    for (uint32_t i = 0; i < ISS_PRESENT_MAX_BUFFERS; ++i) {
        if (S.bufs[i]) AHardwareBuffer_release(S.bufs[i]);
        S.bufs[i] = NULL;
    }
    S.last_set = -1;
}

static void drop_surface_control(void) {
    /* No display: queued frames are dropped like any frame that arrives without one. A transaction
     * still outstanding on the old control is forgotten (its on_complete changes nothing). */
    clear_queue(1);
    S.inflight = 0;
    if (S.sc) {
        /* The newest buffer is on the display until the control goes: its release has no fence. */
        if (S.last_set >= 0 && S.conn_fd >= 0)
            send_release(S.generation, (uint32_t)S.last_set, 0, -1);
        S.last_set = -1;
        api.release(S.sc);
        S.sc = NULL;
    }
}

static void apply_frame_rate(void) {
    if(!api.ok || !api.set_frame_rate || !S.sc)return;
    ASurfaceTransaction* t=api.txn_create();
    api.set_frame_rate(t,S.sc,S.frame_rate,0); /* DEFAULT compatibility: game, not fixed-rate video */
    api.txn_apply(t);api.txn_delete(t);
    LOGI("display: child surface requests %.1f Hz",S.frame_rate);
}

static void make_surface_control(void) {
    if (!api.ok || !S.window || S.sc) return;
    S.sc = api.create_from_window(S.window, "isspresent");
    if (!S.sc) LOGE("ASurfaceControl_createFromWindow failed");
    else { LOGI("surface control created"); apply_frame_rate(); }
}

typedef struct {
    uint32_t session, generation, index; /* the buffer the transaction replaced; index == ~0: none */
    uint64_t frame;
    uint64_t seq; /* the transaction's id (S.txn_seq), compared with S.inflight */
    int trace_index;
    uint64_t trace_frame;
    int gates;    /* FIFO without a commit callback: this on_complete ends the outstanding state */
} DoneCtx;

typedef struct {
    uint32_t session;
    uint64_t seq;
} CommitCtx;

static void wake_receiver(void) {
    uint64_t one = 1;
    if (S.wake_fd >= 0 && write(S.wake_fd, &one, sizeof one) < 0 && errno != EAGAIN)
        LOGE("wake failed: %s", strerror(errno));
}

static void on_complete(void* ctx_, ASurfaceTransactionStats* stats) {
    DoneCtx* ctx = ctx_;
    int fence = -1;
    ASurfaceControl** controls = NULL;
    size_t n = 0;
    api.get_controls(stats, &controls, &n);
    if (n > 0) fence = api.prev_release_fence(stats, controls[0]);
    if (controls) api.release_controls(controls);
    const int64_t latch = ctx->trace_index >= 0 && api.get_latch_time ? api.get_latch_time(stats) : -1;
    pthread_mutex_lock(&S.lock);
    if (ctx->trace_index >= 0 && ctx->session == S.session)
        trace_event(ctx->trace_index, "event=complete frame=%llu txn=%llu latch_ns=%lld completed_ns=%lld previous_buffer=%u release_fence=%d",
                    (unsigned long long)ctx->trace_frame, (unsigned long long)ctx->seq,
                    (long long)latch, (long long)trace_clock_ns(), ctx->index, fence >= 0);
    if (ctx->index != ~0u && ctx->session == S.session && S.conn_fd >= 0) {
        send_release(ctx->generation, ctx->index, ctx->frame, fence);
    } else if (fence >= 0) {
        close(fence);
    }
    /* FIFO: this transaction is on the display, the next queued frame may go (applied by the
     * receive thread, see the top of the file). */
    if (ctx->gates && S.fifo && ctx->session == S.session && ctx->seq == S.inflight) {
        S.inflight = 0;
        if (S.queue_len) wake_receiver();
    }
    pthread_mutex_unlock(&S.lock);
    free(ctx);
}

/* FIFO, API 31: SurfaceFlinger latched the transaction; the next queued frame may go. */
static void on_commit(void* ctx_, ASurfaceTransactionStats* stats) {
    (void)stats;
    CommitCtx* ctx = ctx_;
    pthread_mutex_lock(&S.lock);
    if (S.fifo && ctx->session == S.session && ctx->seq == S.inflight) {
        S.inflight = 0;
        if (S.queue_len) wake_receiver();
    }
    pthread_mutex_unlock(&S.lock);
    free(ctx);
}

/* Hands one frame to SurfaceFlinger: a transaction with the buffer, the fit-to-window geometry and
 * the acquire fence (taken). Needs S.sc and the API; the frame's buffer is checked by the callers. */
static void show_frame(uint32_t generation, uint32_t index, uint64_t frame, uint32_t width, uint32_t height,
                       int acquire_fd) {
    int w = ANativeWindow_getWidth(S.window), h = ANativeWindow_getHeight(S.window);
    if (w <= 0 || h <= 0) { w = (int)width; h = (int)height; }
    int dw, dh;
    gow3_display_size(w, h, width, height, S.fullscreen, &dw, &dh);
    ARect src = {0, 0, (int32_t)width, (int32_t)height};
    ARect dst = {(w - dw) / 2, (h - dh) / 2, (w - dw) / 2 + dw, (h - dh) / 2 + dh};
    const int trace_index = S.trace_frames[index] == frame ? S.trace_indices[index] : -1;
    DoneCtx* ctx = malloc(sizeof *ctx);
    if (!ctx) {
        if (trace_index >= 0) trace_event(trace_index, "event=drop frame=%llu reason=context_allocation time_ns=%lld",
            (unsigned long long)frame, (long long)trace_clock_ns());
        send_release(generation, index, frame, acquire_fd); return;
    }
    ctx->session = S.session;
    ctx->generation = S.generation;
    ctx->index = S.last_set >= 0 ? (uint32_t)S.last_set : ~0u; /* replaced by this transaction */
    ctx->frame = 0;
    ctx->seq = ++S.txn_seq;
    ctx->gates = 0;
    ctx->trace_index = trace_index;
    ctx->trace_frame = frame;
    if (ctx->trace_index >= 0) trace_event(ctx->trace_index, "event=apply frame=%llu txn=%llu generation=%u buffer=%u applied_ns=%lld source=%d,%d,%d,%d destination=%d,%d,%d,%d window=%d,%d fifo=%d acquire_fence=%d",
                (unsigned long long)frame, (unsigned long long)ctx->seq, generation, index,
                (long long)trace_clock_ns(), src.left, src.top, src.right, src.bottom,
                dst.left, dst.top, dst.right, dst.bottom, w, h, S.fifo, acquire_fd >= 0);
    CommitCtx* commit = NULL;
    if (S.fifo) {
        if (api.set_on_commit && (commit = malloc(sizeof *commit))) {
            commit->session = ctx->session;
            commit->seq = ctx->seq;
        } else {
            ctx->gates = 1; /* no commit callback: the complete one gates (API 29/30, or no memory) */
        }
    }
    ASurfaceTransaction* t = api.txn_create();
    api.set_buffer(t, S.sc, S.bufs[index], acquire_fd); /* takes the fd */
    api.set_geometry(t, S.sc, &src, &dst, 0);
    api.set_transparency(t, S.sc, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
    api.set_visibility(t, S.sc, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    api.set_on_complete(t, ctx, on_complete);
    if (commit) api.set_on_commit(t, commit, on_commit);
    api.txn_apply(t);
    api.txn_delete(t);
    S.last_set = (int)index;
    if (S.fifo) S.inflight = ctx->seq;
}

/* FIFO: applies the oldest queued frame when no transaction is outstanding. Called from the receive
 * thread only (a new frame, or the wake-up from on_complete). */
static void pump_queue(void) {
    while (S.fifo && S.queue_len && !S.inflight) {
        Queued q = S.queue[S.queue_head];
        S.queue_head = (S.queue_head + 1) % ISS_PRESENT_MAX_BUFFERS;
        S.queue_len--;
        if (!S.sc || !api.ok || q.generation != S.generation || q.index >= S.count || !S.bufs[q.index]) {
            send_release(q.generation, q.index, q.frame, q.fence); /* cannot be shown: give it back */
            continue;
        }
        show_frame(q.generation, q.index, q.frame, q.width, q.height, q.fence); /* sets S.inflight: the loop ends */
    }
}

/* Warns once that FIFO may run at half rate (API 29/30: no commit callback). */
static void warn_fifo_without_commit(void) {
    static int warned;
    if (!warned && !api.set_on_commit) {
        warned = 1;
        LOGI("no ASurfaceTransaction_setOnCommit (needs Android 12): FIFO waits for the frame to be "
             "presented before the next, which may halve the frame rate");
    }
}

/* The game's MODE message: FIFO <-> newest wins, between two frames. Lock held.
 *  - FIFO -> newest wins: the newest queued frame is applied now (if it can be shown), every older
 *    queued frame is released unseen with its own acquire fence, as for any frame the display does
 *    not take. The transaction still outstanding stays (its callbacks only answer RELEASE now: they
 *    clear S.inflight only in FIFO mode, and S.inflight is 0 from here).
 *  - newest wins -> FIFO: nothing is queued yet and nothing outstanding counts, so the next frame is
 *    applied at once and the ones after it queue behind its commit. */
static void set_fifo(int fifo) {
    fifo = fifo != 0;
    if (fifo == S.fifo) return;
    S.fifo = fifo;
    S.inflight = 0;
    if (!fifo) {
        Queued newest = {0};
        int have = 0;
        while (S.queue_len) {
            Queued q = S.queue[S.queue_head];
            S.queue_head = (S.queue_head + 1) % ISS_PRESENT_MAX_BUFFERS;
            S.queue_len--;
            if (have) send_release(newest.generation, newest.index, newest.frame, newest.fence); /* replaced unseen */
            newest = q;
            have = 1;
        }
        S.queue_head = 0;
        if (have) {
            if (!S.sc || !api.ok || newest.generation != S.generation || newest.index >= S.count ||
                !S.bufs[newest.index])
                send_release(newest.generation, newest.index, newest.frame, newest.fence);
            else
                show_frame(newest.generation, newest.index, newest.frame, newest.width, newest.height, newest.fence);
        }
    } else {
        warn_fifo_without_commit();
    }
    LOGI("presentation: %s", fifo ? "FIFO (VSync on)" : "newest frame (VSync off)");
}

static void present_frame(const iss_present_msg* m, int acquire_fd) {
    const int trace_index = iss_present_trace_index(m);
    if (m->generation != S.generation || m->index >= S.count || !S.bufs[m->index]) {
        if (trace_index >= 0) trace_event(trace_index, "event=drop frame=%llu reason=invalid_buffer time_ns=%lld",
                    (unsigned long long)m->frame, (long long)trace_clock_ns());
        if (acquire_fd >= 0) close(acquire_fd);
        return; /* a frame of a set we do not have: nothing to show, nothing to release */
    }
    S.trace_indices[m->index] = trace_index;
    S.trace_frames[m->index] = m->frame;
    if (trace_index >= 0) trace_event(trace_index, "event=receive frame=%llu generation=%u buffer=%u received_ns=%lld",
                (unsigned long long)m->frame, m->generation, m->index, (long long)trace_clock_ns());
    atomic_fetch_add(&S.frames, 1);
    if (!S.sc || !api.ok) {
        /* No display now (surface lost, app paused): the frame is dropped; its own acquire fence
         * is the release (the game's writes are what the buffer still waits for). */
        if (trace_index >= 0) trace_event(trace_index, "event=drop frame=%llu reason=no_surface time_ns=%lld",
                    (unsigned long long)m->frame, (long long)trace_clock_ns());
        send_release(m->generation, m->index, m->frame, acquire_fd);
        return;
    }
    if (!S.fifo) {
        show_frame(m->generation, m->index, m->frame, m->width, m->height, acquire_fd);
        return;
    }
    if (S.queue_len == ISS_PRESENT_MAX_BUFFERS) {
        /* Cannot happen with a game that waits for RELEASE (it holds at most count <= MAX_BUFFERS
         * buffers); a broken one loses its oldest queued frame instead of growing the queue. */
        LOGE("frame queue full: dropping the oldest");
        Queued* old = &S.queue[S.queue_head];
        send_release(old->generation, old->index, old->frame, old->fence);
        S.queue_head = (S.queue_head + 1) % ISS_PRESENT_MAX_BUFFERS;
        S.queue_len--;
    }
    uint32_t tail = (S.queue_head + S.queue_len) % ISS_PRESENT_MAX_BUFFERS;
    S.queue[tail].generation = m->generation;
    S.queue[tail].index = m->index;
    S.queue[tail].width = m->width;
    S.queue[tail].height = m->height;
    S.queue[tail].frame = m->frame;
    S.queue[tail].fence = acquire_fd;
    S.queue_len++;
    pump_queue();
}

static void close_connection(void) {
    if (S.conn_fd >= 0) { close(S.conn_fd); S.conn_fd = -1; }
    S.session++;
    S.last_set = -1;
    drop_buffers(); /* also the FIFO queue (no connection to release to) and the outstanding id */
    S.count = 0;
    S.fifo = 0;
    /* The control stays: the next game connection shows on the same surface. */
}

/* Serves one connection until it ends. Called without the lock. */
static void serve(int fd) {
    pthread_mutex_lock(&S.lock);
    S.conn_fd = fd;
    S.session++;
    S.generation = 0;
    S.count = 0;
    S.fifo = 0;
    S.inflight = 0;
    pthread_mutex_unlock(&S.lock);
    LOGI("game connected");
    for (;;) {
        /* Block until the game sends something, or on_complete asks for the next queued frame. */
        struct pollfd pfds[2] = {{fd, POLLIN, 0}, {S.wake_fd, POLLIN, 0}};
        if (poll(pfds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            LOGE("poll failed: %s", strerror(errno));
            break;
        }
        if (pfds[1].revents & POLLIN) {
            uint64_t n;
            if (read(S.wake_fd, &n, sizeof n) < 0 && errno != EAGAIN) LOGE("wake read failed: %s", strerror(errno));
            pthread_mutex_lock(&S.lock);
            pump_queue();
            pthread_mutex_unlock(&S.lock);
        }
        if (!(pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        iss_present_msg m;
        int fence = -1;
        int r = iss_present_recv(fd, &m, &fence, 0); /* no lock: data (or the close) is there */
        if (r != 0) {
            if (r != -ECONNRESET) LOGE("receive failed: %d", r);
            break;
        }
        pthread_mutex_lock(&S.lock);
        int stop = 0;
        switch (m.type) {
        case ISS_PRESENT_HELLO: {
            if (m.version != ISS_PRESENT_VERSION || m.count == 0 || m.count > ISS_PRESENT_MAX_BUFFERS) {
                LOGE("bad HELLO: version %u (ours %u) count %u: the game and the app are not the same build",
                     m.version, ISS_PRESENT_VERSION, m.count);
                stop = 1;
                break;
            }
            drop_buffers(); /* clears the FIFO queue too */
            S.generation = m.generation;
            S.count = m.count;
            S.fifo = (m.flags & ISS_PRESENT_FLAG_FIFO) != 0;
            LOGI("presentation: %s", S.fifo ? "FIFO (VSync on)" : "newest frame (VSync off)");
            if (S.fifo) warn_fifo_without_commit();
            iss_present_msg a = iss_present_make(ISS_PRESENT_HELLO);
            a.generation = m.generation;
            a.flags = m.flags & ISS_PRESENT_FLAGS_KNOWN; /* what the app will do */
            stop = iss_present_send(fd, &a, -1) != 0;
            break;
        }
        case ISS_PRESENT_RESET:
            drop_buffers(); /* clears the FIFO queue too: its frames belong to the old set */
            S.generation = m.generation;
            S.count = m.count <= ISS_PRESENT_MAX_BUFFERS ? m.count : 0;
            break;
        case ISS_PRESENT_BUFFER: {
            AHardwareBuffer* b = NULL;
            /* The buffer is the very next datagram. */
            pthread_mutex_unlock(&S.lock);
            int rr = AHardwareBuffer_recvHandleFromUnixSocket(fd, &b);
            pthread_mutex_lock(&S.lock);
            if (rr != 0 || !b) { LOGE("receiving buffer %u failed: %d", m.index, rr); stop = 1; break; }
            if (m.generation == S.generation && m.index < S.count) {
                if (S.bufs[m.index]) AHardwareBuffer_release(S.bufs[m.index]);
                S.bufs[m.index] = b;
            } else {
                AHardwareBuffer_release(b);
            }
            break;
        }
        case ISS_PRESENT_MODE:
            set_fifo((m.flags & ISS_PRESENT_FLAG_FIFO) != 0);
            break;
        case ISS_PRESENT_FRAME:
            present_frame(&m, fence);
            fence = -1;
            break;
        case ISS_PRESENT_BYE:
            stop = 1;
            break;
        default:
            break;
        }
        pthread_mutex_unlock(&S.lock);
        if (fence >= 0) close(fence);
        if (stop) break;
    }
    pthread_mutex_lock(&S.lock);
    close_connection();
    pthread_mutex_unlock(&S.lock);
    LOGI("game disconnected");
}

static void* server_thread(void* unused) {
    (void)unused;
    for (;;) {
        int fd = accept4(S.server_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return NULL; /* closed by shutdown */
        }
        serve(fd);
    }
}

/* ---- JNI ---- */

JNIEXPORT void JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeSetFullscreen(
        JNIEnv* env, jclass cls, jboolean enabled) {
    (void)env; (void)cls;
    pthread_mutex_lock(&S.lock);
    S.fullscreen = enabled != 0;
    pthread_mutex_unlock(&S.lock);
}


JNIEXPORT void JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeSetFrameRate(JNIEnv* env,jclass cls,jfloat rate) {
    (void)env;(void)cls;
    if(!(rate>=0.f && rate<=1000.f))return;
    pthread_mutex_lock(&S.lock);
    if(S.frame_rate!=rate){S.frame_rate=rate;apply_frame_rate();}
    pthread_mutex_unlock(&S.lock);
}

JNIEXPORT jstring JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeStart(JNIEnv* env, jclass cls,
        jstring jpath) {
    (void)cls;
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    char msg[160];
    pthread_mutex_lock(&S.lock);
    if (!S.started) {
        load_api();
        if (S.wake_fd < 0) /* kept for the life of the process: the receive thread may still poll it */
            S.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (S.wake_fd < 0) {
            snprintf(msg, sizeof msg, "eventfd failed: %s", strerror(errno));
            goto fail;
        }
        if (strlen(path) >= sizeof S.path) {
            snprintf(msg, sizeof msg, "socket path too long");
            goto fail;
        }
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        strcpy(a.sun_path, path);
        unlink(path);
        if (fd < 0 || bind(fd, (struct sockaddr*)&a, sizeof a) != 0 || listen(fd, 1) != 0) {
            snprintf(msg, sizeof msg, "cannot listen on %s: %s", path, strerror(errno));
            if (fd >= 0) close(fd);
            goto fail;
        }
        chmod(path, 0600);
        strcpy(S.path, path);
        S.server_fd = fd;
        S.started = 1;
        pthread_create(&S.thread, NULL, server_thread, NULL);
        LOGI("listening on %s, ASurfaceControl %s", path, api.ok ? "available" : "NOT available (Android 10+)");
    }
    pthread_mutex_unlock(&S.lock);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return (*env)->NewStringUTF(env, "present running");
fail:
    pthread_mutex_unlock(&S.lock);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return (*env)->NewStringUTF(env, msg);
}

JNIEXPORT void JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeSetSurface(JNIEnv* env, jclass cls,
        jobject surface) {
    (void)cls;
    pthread_mutex_lock(&S.lock);
    drop_surface_control();
    if (S.window) { ANativeWindow_release(S.window); S.window = NULL; }
    if (surface) {
        S.window = ANativeWindow_fromSurface(env, surface);
        make_surface_control();
    }
    pthread_mutex_unlock(&S.lock);
}

JNIEXPORT jlong JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeFrames(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    return (jlong)atomic_load(&S.frames);
}

JNIEXPORT void JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeShutdown(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    pthread_mutex_lock(&S.lock);
    drop_surface_control();
    if (S.window) { ANativeWindow_release(S.window); S.window = NULL; }
    if (S.started) {
        shutdown(S.server_fd, SHUT_RDWR);
        close(S.server_fd);
        S.server_fd = -1;
        unlink(S.path);
        if (S.conn_fd >= 0) shutdown(S.conn_fd, SHUT_RDWR);
        S.started = 0;
    }
    pthread_mutex_unlock(&S.lock);
}
