// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
namespace IssFsrDiagnostics {
// Opt-in developer instrumentation; never part of camera/history selection.
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ISS_FSR3_DIAGNOSTICS");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
inline bool CaptureEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ISS_FSR3_CAPTURE");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
} // namespace IssFsrDiagnostics
