// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/motion_history.h"

using namespace Vulkan::Motion;

int main() {
    const Draw mesh{.shader=1, .geometry=2, .indices=3, .topology=4,
                    .index_count=6, .instances=1, .vertices={0,4}};
    History legacy(128);
    legacy.NextFrame();
    const auto object_a=legacy.Prepare(mesh);
    const auto object_b=legacy.Prepare(mesh);
    legacy.NextFrame();
    // B remains visible, A is culled. The old algorithm loads A, not B.
    const auto wrong_b=legacy.Prepare(mesh);
    assert(wrong_b.load==object_a.store && wrong_b.load!=object_b.store);

    History strict(128,true);
    strict.NextFrame();
    assert(!strict.Prepare(mesh).load);
    assert(!strict.Prepare(mesh).load);
    strict.NextFrame();
    // No previous instance identity is available: retain camera reprojection.
    const auto visible_b=strict.Prepare(mesh);
    assert(visible_b.store && !visible_b.load);
    strict.NextFrame();
    // A stable unique instance recovers history immediately, at any velocity.
    const auto unique_b=strict.Prepare(mesh);
    assert(unique_b.load==visible_b.store);
    // A second current occurrence must never borrow the first one's history.
    assert(!strict.Prepare(mesh).load);
    strict.NextFrame();
    assert(!strict.Prepare(mesh).load);
    strict.NextFrame();
    const auto unique=strict.Prepare(mesh);
    assert(unique.load);
    Draw changed=mesh;
    changed.topology=99;
    assert(!strict.Prepare(changed).load);
    strict.NextFrame();
    strict.NextFrame();
    assert(!strict.Prepare(mesh).load);
    std::puts("PASS: swapped shared-mesh instances cannot borrow object history; unique draws recover");
}
