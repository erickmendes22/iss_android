// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "../tlggpu.h"

namespace IssSceneCamera {
class Bank {
public:
    void Publish(const TlgIssCameraSnapshot& sample) {
        if (!sample.sequence || !sample.guest_object) return;
        set.sequence = sample.sequence;
        unsigned index = set.count;
        for (unsigned i = 0; i < set.count; ++i) {
            if (set.samples[i].guest_object == sample.guest_object) { index = i; break; }
        }
        if (index == TLG_ISS_CAMERA_CAPACITY) {
            index = 0;
            for (unsigned i = 1; i < set.count; ++i)
                if (set.samples[i].sequence < set.samples[index].sequence) index = i;
        } else if (index == set.count) ++set.count;
        set.samples[index] = sample;
    }
    TlgIssCameraSet Read() const { return set; }
private:
    TlgIssCameraSet set{};
};

// Verified ISS material-instance layout (53 floats / 212 bytes): the first
// nine floats are the object basis, followed by its row-vector clip matrix.
// With uniform instance scale s, clip[0..2] = s * basis * cameraVP[0..2].
// This identity holds for ten independent static instances in capture (3).
inline bool Matches(const TlgIssCameraSnapshot& camera, const float* instance) {
    if (!camera.sequence || !camera.guest_object) return false;
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) {
        double dot = 0;
        for (int k = 0; k < 3; ++k)
            dot += double(instance[row*3+k]) * instance[col*3+k];
        if (!std::isfinite(dot) || std::abs(dot - double(row == col)) > 0.01) return false;
    }
    double product[12]{}, observed[12]{}, cross = 0, square = 0, observed_square = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            const int index = row * 4 + col;
            observed[index] = instance[9 + index];
            for (int k = 0; k < 3; ++k)
                product[index] += double(instance[row * 3 + k]) * camera.composite[k * 4 + col];
            if (!std::isfinite(product[index]) || !std::isfinite(observed[index])) return false;
            cross += product[index] * observed[index];
            square += product[index] * product[index];
            observed_square += observed[index] * observed[index];
        }
    }
    if (square < 1e-12 || observed_square < 1e-12 || cross <= 0) return false;
    const double scale = cross / square;
    double error = 0;
    for (int i = 0; i < 12; ++i) {
        const double difference = observed[i] - scale * product[i];
        error += difference * difference;
    }
    return error <= observed_square * 1e-8; // relative RMS <= 0.0001
}

inline bool Equivalent(const TlgIssCameraSnapshot& a, const TlgIssCameraSnapshot& b) {
    // Equal orientation/projection is insufficient: parallel cameras at
    // different positions must remain ambiguous, never resolved by recency.
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(a.composite[i]) || !std::isfinite(b.composite[i]) ||
            std::abs(double(a.composite[i]) - b.composite[i]) > 1e-5) return false;
    }
    for (int col = 0; col < 3; ++col) {
        double difference = 0;
        for (int k = 0; k < 4; ++k)
            difference += double(a.view_transform[12+k]) * a.inverse[k*4+col] -
                          double(b.view_transform[12+k]) * b.inverse[k*4+col];
        if (!std::isfinite(difference) || std::abs(difference) > 1e-3) return false;
    }
    return true;
}

inline bool Valid(const TlgIssCameraSnapshot& camera) {
    if (!std::isfinite(camera.near_plane) || camera.near_plane <= 0 ||
        !std::isfinite(camera.far_plane) || camera.far_plane <= camera.near_plane ||
        !std::isfinite(camera.vertical_fov) || camera.vertical_fov <= .1f ||
        camera.vertical_fov >= 3.14f || !std::isfinite(camera.aspect) ||
        camera.aspect <= .2f || camera.aspect >= 8.f) return false;
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) {
        double inverse = 0, translated = 0;
        for (int k = 0; k < 4; ++k) {
            inverse += double(camera.composite[row*4+k]) * camera.inverse[k*4+col];
            translated += double(camera.view_transform[row*4+k]) * camera.inverse[k*4+col];
        }
        if (!std::isfinite(inverse) || std::abs(inverse-double(row==col)) > .02 ||
            !std::isfinite(translated) || ((row<3 || col==3) &&
             std::abs(translated-double(row==col)) > .02)) return false;
    }
    return true;
}

inline bool IsInstanceBinding(std::uint64_t shader, unsigned slot, unsigned bytes) {
    return slot == 0 && bytes >= 212 && bytes % 212 == 0 &&
        (shader == 0x40b18d54 || shader == 0x6042bbde || shader == 0x56673dd1);
}

inline bool Select(const TlgIssCameraSet& set, std::uint64_t preferred_object,
                   std::uint64_t shader, unsigned slot, unsigned bytes, const void* data,
                   TlgIssCameraSnapshot& selected) {
    if (!data || !IsInstanceBinding(shader, slot, bytes)) return false;
    std::array<float, 21> instance{};
    std::memcpy(instance.data(), data, sizeof(instance));
    bool found = false;
    for (unsigned i = 0; i < std::min<unsigned>(set.count, TLG_ISS_CAMERA_CAPACITY); ++i) {
        const auto& camera = set.samples[i];
        if (!camera.sequence || camera.sequence > set.sequence ||
            set.sequence - camera.sequence > 64 || !Matches(camera, instance.data()) ||
            !Valid(camera)) continue;
        if (!found) { selected = camera; found = true; continue; }
        if (!Equivalent(selected, camera)) return false;
        if (camera.guest_object == preferred_object ||
            (selected.guest_object != preferred_object && camera.sequence > selected.sequence))
            selected = camera;
    }
    return found;
}
} // namespace IssSceneCamera
