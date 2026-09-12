// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace helix {
namespace gcode {

// GL error codes that warrant a permanent fall-back to the pure-CPU 2D
// renderer. These mirror the OpenGL ES values so this predicate stays
// header-only and free of any GL dependency (so it is unit-testable on
// targets built without ENABLE_GLES_3D).
//
// GL_OUT_OF_MEMORY (0x0505): the driver could not allocate memory for the
//   draw. On constrained Mali/Panfrost boards (e.g. Allwinner CB1) this can
//   fault inside the driver rather than returning cleanly, so the only safe
//   reaction is to stop issuing 3D draws entirely.
// GL_INVALID_OPERATION (0x0502): the command stream is in a state the driver
//   rejects. Continuing to draw risks undefined behaviour in the driver.
constexpr unsigned int GL_ERR_OUT_OF_MEMORY = 0x0505;
constexpr unsigned int GL_ERR_INVALID_OPERATION = 0x0502;

/// Decide whether a GL error observed after a draw batch should trigger a
/// permanent, session-sticky fall-back from the 3D GLES renderer to the
/// pure-CPU 2D renderer.
///
/// Pure function (no GL state, no side effects) so the fall-back decision is
/// unit-testable without a live GL context.
///
/// @param gl_error  the value returned by glGetError() (0 == GL_NO_ERROR)
/// @return true if the error is fatal enough to abandon GPU rendering
inline bool gl_draw_error_is_fatal(unsigned int gl_error) {
    return gl_error == GL_ERR_OUT_OF_MEMORY || gl_error == GL_ERR_INVALID_OPERATION;
}

/// Case-insensitive substring search of a GL_RENDERER string against a list of
/// needles. Both renderer predicates below key off the same driver-supplied
/// string, so they share one matcher rather than each lowering it themselves.
///
/// @param renderer  the value returned by glGetString(GL_RENDERER) (may be null)
/// @param needles   lowercase substrings to look for
/// @param count     number of entries in @p needles
/// @return true if any needle occurs in @p renderer
inline bool gl_renderer_matches(const char* renderer, const char* const* needles, size_t count) {
    if (renderer == nullptr || renderer[0] == '\0') {
        return false;
    }

    std::string lowered(renderer);
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    for (size_t i = 0; i < count; ++i) {
        if (lowered.find(needles[i]) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Decide, BEFORE the first GPU draw, whether a GPU is known to hard-fault in
/// the 3D gcode preview and must be excluded from the GLES path entirely.
///
/// The reactive glGetError() guard above cannot help here: on constrained
/// Mali/Panfrost SBCs (BTT CB1 Mali-G31, CB2/RK3566 Mali-G52) the driver
/// SIGSEGVs *inside* glDrawArrays (issues #966 / #1084 / #1085) — control never
/// returns, so glGetError() never runs. The only safe reaction is to never
/// issue the draw. We key off the GL_RENDERER string the driver reports at
/// context creation and skip the GPU path proactively when it matches a
/// known-bad substring.
///
/// The seed list is a single entry, "panfrost" (the open Mesa driver common to
/// the crashing boards). It is deliberately easy to extend from field
/// GL_RENDERER logs — the renderer logs the exact string at startup so new
/// bad GPUs can be added here. GPUs the denylist misses are still covered by
/// the crash-loop breaker (a persistent guard file promoted to a block after
/// one real hard-fault), so this list does not need to be exhaustive.
///
/// Pure function (no GL state, no side effects) so the decision is unit-testable
/// without a live GL context.
///
/// @param renderer  the value returned by glGetString(GL_RENDERER) (may be null)
/// @return true if the renderer matches a known-bad substring (case-insensitive)
inline bool gl_renderer_is_denylisted(const char* renderer) {
    static const char* const DENYLIST[] = {"panfrost"};
    return gl_renderer_matches(renderer, DENYLIST, sizeof(DENYLIST) / sizeof(DENYLIST[0]));
}

/// Decide whether a GL_RENDERER string names a software rasterizer rather than
/// a GPU.
///
/// Mesa answers with one of these when it cannot reach the hardware — a stale
/// or mismatched userspace, a missing kernel driver, a render node the process
/// cannot open. The context creates successfully and every GL call works, so
/// nothing fails; the drawing simply happens on the CPU.
///
/// For a presentation path that exists to move work OFF the CPU, that outcome
/// is worse than no GPU at all: it pays context setup, buffer handoff and a
/// copy to spend more CPU than it saves. Callers choosing between a GPU path
/// and a CPU path must treat a software renderer as a refusal.
///
/// This asks a different question from gl_renderer_is_denylisted() above.
/// That one names hardware whose driver faults during 3D draws; this one names
/// the absence of hardware. A renderer can be neither, either, or both:
/// Panfrost is denylisted for 3D draws yet presents through EGL correctly.
///
/// Pure function (no GL state, no side effects) so the decision is unit-testable
/// without a live GL context.
///
/// @param renderer  the value returned by glGetString(GL_RENDERER) (may be null)
/// @return true if the renderer is a software rasterizer (case-insensitive)
inline bool gl_renderer_is_software(const char* renderer) {
    // "swrast" also covers Mesa's "kms_swrast", which is the one that appears
    // on an SBC whose GPU kernel driver is missing but whose KMS node works.
    static const char* const SOFTWARE[] = {"llvmpipe", "softpipe", "swrast", "swiftshader"};
    return gl_renderer_matches(renderer, SOFTWARE, sizeof(SOFTWARE) / sizeof(SOFTWARE[0]));
}

} // namespace gcode
} // namespace helix
