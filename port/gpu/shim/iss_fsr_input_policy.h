// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace IssFsrInput {
struct Image {
    bool available{}, sampled{};
    std::uint32_t format{}, width{}, height{};
};
// Object-only fallback can leave static scenery at zero velocity while the
// camera moves. A stable source must not accidentally authorize that history.
inline bool ResetHistory(bool requested, bool native_camera_ready) {
    return requested || !native_camera_ready;
}
// Only the exact scene input identified by the current frame's AA marker is
// eligible. Different formats/extents (including HDR/G-buffer images) fall back.
inline bool UseOriginal(bool scaled_session, bool fsr3, bool enabled,
                        const Image& original, const Image& composite,
                        std::uint32_t rgba8_unorm) {
    return scaled_session && fsr3 && enabled && original.available && original.sampled &&
           original.format == rgba8_unorm && composite.format == rgba8_unorm &&
           original.width == composite.width && original.height == composite.height &&
           original.width != 0 && original.height != 0;
}
} // namespace IssFsrInput
