// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
namespace VideoCore::HybridReadbackPolicy {
inline unsigned RequestedQueueCount(unsigned available, bool hybrid) {
#ifdef ISS_ANDROID_BIONIC
    const char* p = std::getenv("TLG_READBACK_QUEUE");
    return hybrid && available >= 2 && (!p || p[0] != '0') ? 2u : 1u;
#else
    (void)available; (void)hybrid;
    return 1;
#endif
}
inline bool PartialStaging() {
#ifdef ISS_ANDROID_BIONIC
    static const bool enabled = [] { const char* p = std::getenv("TLG_HYBRID_STAGE_PARTIAL"); return !p || p[0] != '0'; }();
    return enabled;
#else
    return false;
#endif
}
inline bool RequestPages() {
#ifdef ISS_ANDROID_BIONIC
    static const bool enabled = [] { const char* p = std::getenv("TLG_HYBRID_REQUEST_PAGES"); return !p || p[0] != '0'; }();
    return enabled;
#else
    return false;
#endif
}
} // namespace VideoCore::HybridReadbackPolicy
