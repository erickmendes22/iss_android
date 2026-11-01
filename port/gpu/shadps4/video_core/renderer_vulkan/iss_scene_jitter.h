// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace Vulkan {
// A newly discovered scene target cannot acquire jitter midway through a
// frame: an earlier depth-only draw may already have used zero displacement.
class IssSceneJitter {
public:
    using Key = std::array<std::uint64_t, 4>; // guest address, width, height, format
    void BeginFrame(bool reset) {
        frame = reset ? Key{} : observed;
        observed = {};
    }
    void Observe(Key key) { observed = key; }
    bool Applies(Key key) const { return key[0] && key == frame; }
private:
    Key frame{}, observed{};
};
}
