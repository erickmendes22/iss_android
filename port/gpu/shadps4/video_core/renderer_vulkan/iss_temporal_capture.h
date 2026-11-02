#pragma once
#include <algorithm>
#include <cstdint>

namespace Vulkan {
// Requested captures only. Keep the two full SDK frames, then follow a bounded
// pixel-exact central region through FSR and the guest's final display pass.
class IssTemporalCapture {
public:
    static constexpr int Frames = 120;
    struct Sample { int index = -1; std::uint64_t serial = 0; };
    struct Crop { unsigned x, y, width, height; };
    void Begin() { remaining = Frames; next = 0; }
    Sample Advance() {
        if (!remaining) return {};
        --remaining;
        return {next++, ++serial};
    }
    static Crop Center(unsigned width, unsigned height) {
        const unsigned w = std::min(width, 640u), h = std::min(height, 360u);
        return {(width - w) / 2, (height - h) / 2, w, h};
    }
private:
    int remaining = 0, next = 0;
    std::uint64_t serial = 0;
};
}
