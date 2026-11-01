// Adapted from Bloodborne-Android 2e472653; Android presentation infrastructure.
/* Wire format of the Bionic game process -> app frame path ("AHB swapchain").
 *
 * The game (libbbgpu.so, gpu/shim/iss_present_client.cpp) renders into N AHardwareBuffers and the app
 * (libbbpresent.so, tools/android/frontend/jni/bbpresent) shows them with an ASurfaceControl. They
 * talk over one AF_UNIX SOCK_SEQPACKET socket; the app listens on the path the game finds in the
 * environment variable ISS_PRESENT_SOCKET (the app's files dir + "/bbpresent.sock"). One message is
 * one datagram of exactly sizeof(iss_present_msg) bytes; a file descriptor travels as one SCM_RIGHTS
 * descriptor of the same datagram. Both sides run on the same device (same endianness).
 *
 *   game -> app  HELLO     version; width/height/format/count = the buffer set that follows
 *                          (generation 1); flags (ISS_PRESENT_FLAG_*, in the field `index` shares):
 *                          how the app is to show the frames at the start (MODE changes it). The app
 *                          answers HELLO (its version, flags = the ones it accepts) or closes the
 *                          socket; a version it does not speak is a close, logged on both sides.
 *   game -> app  MODE      flags (ISS_PRESENT_FLAG_*): the presentation mode from now on, see below.
 *                          Sent when the game's effective mode changes while it runs (Lossless
 *                          Scaling frame generation changes the buffer count); no answer.
 *   game -> app  BUFFER    generation, index, width, height, format. Followed IMMEDIATELY by the
 *                          buffer itself, one more datagram written by
 *                          AHardwareBuffer_sendHandleToUnixSocket (the receiver reads it with
 *                          AHardwareBuffer_recvHandleFromUnixSocket right after the BUFFER message;
 *                          the sender holds a lock over the pair).
 *   game -> app  FRAME     generation, index, frame (counter), width, height; fd = acquire fence (a
 *                          sync file that signals when the game's GPU writes are done), or no fd.
 *                          The buffer is the app's until it sends RELEASE for it.
 *   app  -> game RELEASE   generation, index, frame; fd = release fence (signals when the display is
 *                          done reading), or no fd: the buffer is free at once. Sent for every
 *                          FRAME, also when the app drops the frame (no surface): then the fence is
 *                          the frame's own acquire fence.
 *   game -> app  RESET     generation (new), width/height/format/count: the buffers of the previous
 *                          generation are dropped (the app releases its references; releases still
 *                          to come for them carry the old generation and are ignored by the game).
 *                          BUFFER messages of the new generation follow.
 *   either       BYE       the sender closes the socket after it.
 *
 * Presentation is throttled by RELEASE: the game reuses a buffer only after its RELEASE, so at most
 * count - 1 frames are in the app's hands while the game renders the next one (count = 3: one on the
 * display, one pending).
 *
 * Presentation modes (HELLO flags):
 *   no flag  Newest wins ("VSync off"): every FRAME is applied at once; frames that arrive within
 *            one display refresh replace each other and only the newest is ever shown (the others
 *            are released without having been on screen). No tearing; the lowest latency.
 *   ISS_PRESENT_FLAG_FIFO  ("VSync on"): a FRAME is applied only after the previous one has been
 *            latched by SurfaceFlinger (ASurfaceTransaction_setOnCommit, Android 12+; on 10/11 only
 *            setOnComplete exists, i.e. after it was presented, and the rate may halve), so every
 *            frame is shown for at least one refresh, in order, one per refresh. Frames that
 *            arrive meanwhile wait in the app (at most count of them); the game is held back by the
 *            missing RELEASEs. Bionic game: ISS_VSYNC=1 (gow3.ini vsync=1).
 * Both modes use the same messages and the same RELEASE rules. HELLO sets the mode and MODE changes it
 * (version 3), at a point in the stream between two FRAMEs:
 *   FIFO -> newest wins  the newest frame waiting in the app's queue is applied at once and the older
 *            queued frames are released unseen, each with its own acquire fence as the release fence
 *            (the game's writes are what their buffers still wait for); later frames are applied as
 *            they arrive.
 *   newest wins -> FIFO  later frames queue as above; the first goes on at once.
 * Frame generation that adds frames in the game (Lossless Scaling, libbbgpu/vk_lsfg.cpp) hands the
 * app a burst of frames per game frame and needs FIFO for their even spacing.
 *
 * Version 3 added MODE. The app and the game must be the same version: HELLO with another one is
 * refused and logged on both sides.
 */
#ifndef ISS_PRESENT_PROTO_H
#define ISS_PRESENT_PROTO_H

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define ISS_PRESENT_MAGIC 0x52504242u /* "BBPR" */
#define ISS_PRESENT_VERSION 3u
/* Optional FRAME diagnostic tag in otherwise unused format/count fields. */
#define ISS_PRESENT_TRACE_MAGIC 0x54524345u
#define ISS_PRESENT_MAX_BUFFERS 8u

/* HELLO flags. */
#define ISS_PRESENT_FLAG_FIFO 0x1u /* show every frame for at least one refresh, in order (VSync) */
#define ISS_PRESENT_FLAGS_KNOWN ISS_PRESENT_FLAG_FIFO

enum iss_present_type {
    ISS_PRESENT_HELLO = 1,
    ISS_PRESENT_BUFFER = 2,
    ISS_PRESENT_FRAME = 3,
    ISS_PRESENT_RELEASE = 4,
    ISS_PRESENT_RESET = 5,
    ISS_PRESENT_BYE = 6,
    ISS_PRESENT_MODE = 7,
};

typedef struct iss_present_msg {
    uint32_t magic;      /* ISS_PRESENT_MAGIC */
    uint16_t version;    /* ISS_PRESENT_VERSION */
    uint16_t type;       /* enum iss_present_type */
    uint32_t generation; /* buffer set, starts at 1, +1 on every RESET */
    union {
        uint32_t index; /* buffer number, < count (BUFFER, FRAME, RELEASE) */
        uint32_t flags; /* ISS_PRESENT_FLAG_* (HELLO, the app's answer, MODE) */
    };
    uint32_t width;      /* pixels */
    uint32_t height;
    uint32_t format;     /* AHARDWAREBUFFER_FORMAT_* */
    uint32_t count;      /* buffers in the set (HELLO, RESET) */
    uint64_t frame;      /* frame counter, from 1 */
} iss_present_msg;

static inline int iss_present_trace_index(const iss_present_msg* msg) {
    return msg->type == ISS_PRESENT_FRAME && msg->format == ISS_PRESENT_TRACE_MAGIC &&
           msg->count >= 1 && msg->count <= 120 ? (int)msg->count - 1 : -1;
}

_Static_assert(sizeof(iss_present_msg) == 40, "iss_present_msg is part of the wire format");

static inline iss_present_msg iss_present_make(uint16_t type) {
    iss_present_msg m;
    memset(&m, 0, sizeof m);
    m.magic = ISS_PRESENT_MAGIC;
    m.version = ISS_PRESENT_VERSION;
    m.type = type;
    return m;
}

/* Sends one message, with fd (>= 0) as SCM_RIGHTS. 0 on success, -errno on failure. The caller keeps
 * ownership of fd. */
static inline int iss_present_send(int sock, const iss_present_msg* msg, int fd) {
    struct iovec iov = {(void*)msg, sizeof *msg};
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } control;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        memset(control.buf, 0, sizeof control.buf);
        mh.msg_control = control.buf;
        mh.msg_controllen = sizeof control.buf;
        struct cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    for (;;) {
        ssize_t n = sendmsg(sock, &mh, MSG_NOSIGNAL);
        if (n == (ssize_t)sizeof *msg) return 0;
        if (n < 0 && errno == EINTR) continue;
        return n < 0 ? -errno : -EMSGSIZE;
    }
}

/* Receives one message (flags: 0 or MSG_DONTWAIT). *fd is the received descriptor or -1 (the caller
 * owns and closes it). Returns 0 on success, -EAGAIN when nothing is there (MSG_DONTWAIT), -ECONNRESET
 * when the peer closed, -EPROTO for a message that is not ours, other -errno otherwise. */
static inline int iss_present_recv(int sock, iss_present_msg* msg, int* fd, int flags) {
    struct iovec iov = {msg, sizeof *msg};
    union {
        char buf[CMSG_SPACE(sizeof(int) * 4)];
        struct cmsghdr align;
    } control;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = control.buf;
    mh.msg_controllen = sizeof control.buf;
    *fd = -1;
    ssize_t n;
    do {
        n = recvmsg(sock, &mh, flags | MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    int err = n < 0 ? -errno : 0;
    /* Take every descriptor that came, keep the first, close the rest. */
    for (struct cmsghdr* c = (n >= 0 && mh.msg_controllen) ? CMSG_FIRSTHDR(&mh) : 0; c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int got;
            memcpy(&got, CMSG_DATA(c) + i * sizeof(int), sizeof got);
            if (*fd < 0 && !err) {
                *fd = got;
            } else {

                close(got);
            }
        }
    }
    if (err) return err;
    if (n == 0) return -ECONNRESET;
    if (n != (ssize_t)sizeof *msg || msg->magic != ISS_PRESENT_MAGIC || (mh.msg_flags & MSG_TRUNC)) {
        if (*fd >= 0) {

            close(*fd);
            *fd = -1;
        }
        return -EPROTO;
    }
    return 0;
}

#endif
