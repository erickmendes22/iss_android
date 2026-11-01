// Adapted from Bloodborne-Android 2e472653; Android presentation infrastructure.
// gow3: see iss_present_client.h.
#ifdef __ANDROID__
#include "iss_present_client.h"

#include <android/hardware_buffer.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "iss_present_proto.h"

namespace IssPresent {

namespace {
const char* SocketPath() {
    const char* path = std::getenv("ISS_PRESENT_SOCKET");
    return path && *path ? path : nullptr;
}
} // namespace

Client::~Client() {
    Disconnect();
}

bool Client::Configured() {
    return SocketPath() != nullptr;
}

bool Client::VsyncRequested() {
    const char* value = std::getenv("ISS_VSYNC");
    return value && std::strtol(value, nullptr, 10) != 0;
}

bool Client::Connect(u32 width, u32 height, u32 format, u32 count, bool fifo) {
    const char* path = SocketPath();
    if (!path) {
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (std::strlen(path) >= sizeof addr.sun_path) {
        std::printf("Present: ISS_PRESENT_SOCKET is too long for a socket path: %s\n", path);
        return false;
    }
    std::strcpy(addr.sun_path, path);
    // The app creates the socket when it starts the display, normally before the game: wait a
    // while all the same (30 s) rather than give up on a slow start.
    int sock = -1;
    int err = 0;
    for (int attempt = 0; attempt < 300; ++attempt) {
        sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (sock < 0) {
            std::printf("Present: socket: %s\n", std::strerror(errno));
            return false;
        }
        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
            err = 0;
            break;
        }
        err = errno;
        close(sock);
        sock = -1;
        if (attempt == 0) {
            std::printf("Present: waiting for the app at %s (%s)\n", path, std::strerror(err));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (sock < 0) {
        std::printf("Present: cannot connect to %s: %s\n", path, std::strerror(err));
        return false;
    }
    fd = sock;
    hello_count = count;
    iss_present_msg hello = iss_present_make(ISS_PRESENT_HELLO);
    hello.generation = 1;
    hello.width = width;
    hello.height = height;
    hello.format = format;
    hello.count = count;
    hello.flags = fifo ? ISS_PRESENT_FLAG_FIFO : 0;
    if (!SendRaw(&hello, -1)) {
        Disconnect();
        return false;
    }
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, 10000) <= 0) {
        std::printf("Present: the app did not answer HELLO within 10 s\n");
        Disconnect();
        return false;
    }
    iss_present_msg answer;
    int answer_fd = -1;
    const int result = iss_present_recv(fd, &answer, &answer_fd, 0);
    if (answer_fd >= 0) {
        close(answer_fd);
    }
    if (result != 0 || answer.type != ISS_PRESENT_HELLO || answer.version != ISS_PRESENT_VERSION) {
        std::printf("Present: bad answer to HELLO (result %d, type %u, version %u, ours %u)\n",
                    result, result == 0 ? unsigned(answer.type) : 0u,
                    result == 0 ? unsigned(answer.version) : 0u, ISS_PRESENT_VERSION);
        Disconnect();
        return false;
    }
    if ((answer.flags & ISS_PRESENT_FLAGS_KNOWN) != hello.flags) {
        // Same protocol version, so the app knows the flag: it only answers what it will do.
        std::printf("Present: the app does not show frames the way asked (flags %#x, answered %#x)\n",
                    hello.flags, answer.flags);
    }
    fifo_mode = fifo;
    std::printf("Present: connected to the app (protocol %u), %u buffers %ux%u, %s\n", answer.version,
                count, width, height,
                (answer.flags & ISS_PRESENT_FLAG_FIFO) ? "VSync on (every frame shown for a refresh)"
                                                      : "VSync off (newest frame shown)");
    return true;
}

bool Client::SendRaw(const void* msg, int fence_fd) {
    const int result = iss_present_send(fd, static_cast<const iss_present_msg*>(msg), fence_fd);
    if (result != 0) {
        std::printf("Present: send failed: %s\n", std::strerror(-result));
        return false;
    }
    return true;
}

bool Client::SetFifo(bool fifo) {
    if (fifo == fifo_mode) {
        return true;
    }
    fifo_mode = fifo;
    if (fd < 0) {
        return false;
    }
    iss_present_msg mode = iss_present_make(ISS_PRESENT_MODE);
    mode.flags = fifo ? ISS_PRESENT_FLAG_FIFO : 0;
    std::scoped_lock lock{send_mutex};
    if (!SendRaw(&mode, -1)) {
        return false;
    }
    std::printf("Present: %s\n", fifo ? "VSync on (every frame shown for a refresh)"
                                       : "VSync off (newest frame shown)");
    std::fflush(stdout);
    return true;
}

bool Client::BeginGeneration(u32 generation, u32 width, u32 height, u32 format, u32 count) {
    if (fd < 0) {
        return false;
    }
    if (generation == 1) {
        return true; // announced by the HELLO of Connect
    }
    iss_present_msg reset = iss_present_make(ISS_PRESENT_RESET);
    reset.generation = generation;
    reset.width = width;
    reset.height = height;
    reset.format = format;
    reset.count = count;
    std::scoped_lock lock{send_mutex};
    return SendRaw(&reset, -1);
}

bool Client::SendBuffer(u32 generation, u32 index, u32 width, u32 height, u32 format,
                        AHardwareBuffer* buffer) {
    if (fd < 0) {
        return false;
    }
    iss_present_msg msg = iss_present_make(ISS_PRESENT_BUFFER);
    msg.generation = generation;
    msg.index = index;
    msg.width = width;
    msg.height = height;
    msg.format = format;
    std::scoped_lock lock{send_mutex};
    if (!SendRaw(&msg, -1)) {
        return false;
    }
    const int result = AHardwareBuffer_sendHandleToUnixSocket(buffer, fd);
    if (result != 0) {
        std::printf("Present: sending buffer %u failed: %s\n", index, std::strerror(-result));
        return false;
    }
    return true;
}

bool Client::SendFrame(u32 generation, u32 index, u64 frame, u32 width, u32 height,
                       int acquire_fd, int trace_index) {
    if (fd < 0) {
        return false;
    }
    iss_present_msg msg = iss_present_make(ISS_PRESENT_FRAME);
    msg.generation = generation;
    msg.index = index;
    msg.frame = frame;
    if (trace_index >= 0 && trace_index < 120) {
        msg.format = ISS_PRESENT_TRACE_MAGIC; msg.count = u32(trace_index + 1);
    }
    msg.width = width;
    msg.height = height;
    std::scoped_lock lock{send_mutex};
    return SendRaw(&msg, acquire_fd);
}

int Client::WaitRelease(Release& out, int timeout_ms) {
    if (fd < 0) {
        return -1;
    }
    for (;;) {
        pollfd pfd{fd, POLLIN, 0};
        const int ready = poll(&pfd, 1, timeout_ms);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready == 0) {
            return 0;
        }
        if (ready < 0) {
            std::printf("Present: poll: %s\n", std::strerror(errno));
            return -1;
        }
        iss_present_msg msg;
        int received_fd = -1;
        const int result = iss_present_recv(fd, &msg, &received_fd, MSG_DONTWAIT);
        if (result == -EAGAIN) {
            continue;
        }
        if (result != 0) {
            if (result != -ECONNRESET) {
                std::printf("Present: receive failed: %s\n", std::strerror(-result));
            }
            return -1;
        }
        if (msg.type == ISS_PRESENT_BYE) {
            if (received_fd >= 0) {
                close(received_fd);
            }
            return -1;
        }
        if (msg.type != ISS_PRESENT_RELEASE) {
            if (received_fd >= 0) {
                close(received_fd);
            }
            continue; // not for us; the protocol has no other app -> game messages
        }
        out.generation = msg.generation;
        out.index = msg.index;
        out.frame = msg.frame;
        out.fence_fd = received_fd;
        return 1;
    }
}

void Client::Disconnect() {
    if (fd >= 0) {
        iss_present_msg bye = iss_present_make(ISS_PRESENT_BYE);
        iss_present_send(fd, &bye, -1);
        close(fd);
        fd = -1;
    }
}

} // namespace IssPresent
#endif
