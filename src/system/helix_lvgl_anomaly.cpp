// SPDX-License-Identifier: GPL-3.0-or-later
#include "helix_lvgl_anomaly.h"

#include <cstdio>
#include <string>

// Stack trace support (glibc Linux / macOS only).
// Android NDK and musl libc (Creality K1) don't ship execinfo.h.
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__))
#include <execinfo.h>
#define HELIX_ANOMALY_HAS_BACKTRACE 1
#endif

#include "system/telemetry_manager.h"

namespace {
// Capture the caller's backtrace and format as a comma-separated list of hex PCs.
// The telemetry resolver pipeline (scripts/telemetry-crashes.py) can symbolize
// hex PCs against the binary's symbol cache the same way it resolves crash frames.
// Skip 2 frames: this helper + helix_lvgl_anomaly itself.
std::string capture_backtrace_hex() {
#ifdef HELIX_ANOMALY_HAS_BACKTRACE
    constexpr int MAX_FRAMES = 24;
    constexpr int SKIP_FRAMES = 2;
    void* frames[MAX_FRAMES];
    int n = ::backtrace(frames, MAX_FRAMES);
    std::string out;
    out.reserve(MAX_FRAMES * 16);
    for (int i = SKIP_FRAMES; i < n; ++i) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s0x%lx", i == SKIP_FRAMES ? "" : ",",
                      reinterpret_cast<unsigned long>(frames[i]));
        out += buf;
    }
    return out;
#else
    return std::string{};
#endif
}
} // namespace

extern "C" void helix_lvgl_anomaly(const char* code, const char* context) {
    // Use "display" category — it's on the TelemetryManager allow-list and
    // semantically correct for LVGL/render-layer anomalies. Per-category rate
    // limit (1/5min) applies, so we share the budget with other display errors.
    std::string ctx;
    ctx.reserve(384);
    if (context && *context) {
        ctx = context;
        ctx += " | ";
    }
    // Stable anchor: runtime address of helix_lvgl_anomaly itself.
    // Python-side resolver uses this to compute load_base directly
    // (base = runtime_anchor - file_offset_of_helix_lvgl_anomaly), avoiding
    // the heuristic guesswork that fails on PIE binaries. Backwards-compat:
    // pre-fix bundles lack this field and the resolver falls back to raw hex.
    {
        char anchor_buf[40];
        std::snprintf(anchor_buf, sizeof(anchor_buf), "runtime_anchor=0x%lx | ",
                      reinterpret_cast<unsigned long>(&helix_lvgl_anomaly));
        ctx += anchor_buf;
    }
    ctx += "bt=";
    ctx += capture_backtrace_hex();
    TelemetryManager::instance().record_error("display", code ? code : "unknown_anomaly", ctx);
}
