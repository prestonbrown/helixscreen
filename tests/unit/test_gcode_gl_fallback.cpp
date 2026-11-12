// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Regression tests for the 3D→2D defensive fallback decision (issue #966).
//
// The 3D GLES gcode renderer can fault inside the GPU driver on constrained
// boards (Mali-G31 on Allwinner CB1) under memory pressure. The defensive
// guard checks glGetError() once after each draw batch and, on a fatal error,
// abandons GPU rendering for the rest of the session — the viewer degrades to
// the pure-CPU 2D renderer instead of letting the driver crash the process.
//
// A unit test cannot drive a real GL context, so the testable seam is the pure
// decision predicate `gl_draw_error_is_fatal()` that gates the fallback. These
// tests FAIL if that predicate is weakened or removed.

#include "gcode_gl_fallback.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using helix::gcode::gl_draw_error_is_fatal;
using helix::gcode::gl_renderer_is_denylisted;
using helix::gcode::kGlErrInvalidOperation;
using helix::gcode::kGlErrOutOfMemory;

TEST_CASE("gl_draw_error_is_fatal falls back only on errors that leave the driver unsafe",
          "[gcode][gl_fallback]") {
    // GL_OUT_OF_MEMORY is the primary CB1/Mali fault signature, and
    // GL_INVALID_OPERATION means a driver-rejected command stream: drawing on is
    // unsafe. The literal GL ES values guard the header-only constants.
    REQUIRE(kGlErrOutOfMemory == 0x0505u);
    REQUIRE(kGlErrInvalidOperation == 0x0502u);
    REQUIRE(gl_draw_error_is_fatal(0x0505));
    REQUIRE(gl_draw_error_is_fatal(0x0502));

    // A healthy frame (GL_NO_ERROR) keeps 3D, or the viewer thrashes to 2D every
    // frame; INVALID_ENUM, INVALID_VALUE and INVALID_FRAMEBUFFER_OPERATION leave the
    // driver defined and the next frame recovers.
    for (unsigned int err : {0x0000u, 0x0500u, 0x0501u, 0x0506u}) {
        CAPTURE(err);
        REQUIRE_FALSE(gl_draw_error_is_fatal(err));
    }
}

// ---------------------------------------------------------------------------
// Layer 1: proactive GL_RENDERER denylist (issues #966 / #1084 / #1085)
//
// The reactive glGetError() guard above cannot catch an in-driver SIGSEGV:
// control never returns from glDrawArrays, so glGetError() never runs. The
// denylist gates the GPU path BEFORE the first draw, keying off the
// GL_RENDERER string that Panfrost/Mali drivers report. These tests FAIL if
// the predicate is weakened or removed.
// ---------------------------------------------------------------------------

TEST_CASE("gl_renderer_is_denylisted matches known-bad Panfrost GPUs", "[gcode][gl_fallback]") {
    // The exact GL_RENDERER strings from the crash reports.
    REQUIRE(gl_renderer_is_denylisted("Mali-G52 (Panfrost)"));
    REQUIRE(gl_renderer_is_denylisted("Mali-G31 (Panfrost)"));
    // Driver casing varies across Mesa versions — the match must not depend on it.
    REQUIRE(gl_renderer_is_denylisted("PANFROST"));
    REQUIRE(gl_renderer_is_denylisted("panfrost"));
}

TEST_CASE("gl_renderer_is_denylisted allows known-good GPUs", "[gcode][gl_fallback]") {
    // glGetString(GL_RENDERER) can return nullptr on a broken context.
    REQUIRE_FALSE(gl_renderer_is_denylisted(nullptr));
    REQUIRE_FALSE(gl_renderer_is_denylisted(""));
    // Software rasterizer and desktop GPUs render the 3D preview fine.
    REQUIRE_FALSE(gl_renderer_is_denylisted("llvmpipe (LLVM 15.0.0, 128 bits)"));
    REQUIRE_FALSE(gl_renderer_is_denylisted("Mesa Intel(R) UHD Graphics (ADL GT2)"));
    REQUIRE_FALSE(gl_renderer_is_denylisted("Apple M1"));
}

TEST_CASE("gl_renderer_is_denylisted does not match bare Mali without Panfrost",
          "[gcode][gl_fallback]") {
    // A bare "Mali-G52" (proprietary blob driver, not Panfrost) is intentionally
    // NOT denylisted — some Mali blobs render fine, and over-blocking would
    // needlessly force 2D. The crash-loop breaker (Layer 2) is the backstop for
    // any GPU the denylist misses: it promotes to a persistent block after one
    // real hard-fault, so we don't have to predict every bad string here.
    REQUIRE_FALSE(gl_renderer_is_denylisted("Mali-G52"));
}

// ---------------------------------------------------------------------------
// Software-renderer refusal: the EGL presentation rung must not accept llvmpipe
//
// A software rasterizer creates a context and answers every GL call, so the
// only signal that the GPU was never reached is the renderer string. Accepting
// it costs more CPU than the software path it replaces, which is the opposite
// of why the rung exists. These tests FAIL if the predicate stops recognising
// a software renderer, or starts rejecting real hardware.
// ---------------------------------------------------------------------------

TEST_CASE("Software renderers are refused", "[gcode][gl_fallback][egl]") {
    using helix::gcode::gl_renderer_is_software;

    SECTION("Mesa software rasterizers") {
        REQUIRE(gl_renderer_is_software("llvmpipe (LLVM 15.0.6, 128 bits)"));
        REQUIRE(gl_renderer_is_software("softpipe"));
        REQUIRE(gl_renderer_is_software("swrast"));
        REQUIRE(gl_renderer_is_software("kms_swrast"));
    }

    SECTION("case-insensitive") {
        REQUIRE(gl_renderer_is_software("LLVMpipe"));
        REQUIRE(gl_renderer_is_software("KMS_SWRAST"));
    }

    SECTION("real GPUs are accepted") {
        REQUIRE_FALSE(gl_renderer_is_software("V3D 7.1.7"));
        REQUIRE_FALSE(gl_renderer_is_software("Mali-G31 (Panfrost)"));
        REQUIRE_FALSE(gl_renderer_is_software("Mali-G52 (Panfrost)"));
        REQUIRE_FALSE(gl_renderer_is_software("AMD Radeon Graphics (radeonsi)"));
        REQUIRE_FALSE(gl_renderer_is_software("VC4 V3D 2.1"));
    }

    SECTION("absent renderer string is not a software claim") {
        // A null string means the query failed, which the caller reports as a
        // failed probe on its own. Guessing "software" here would conflate the
        // two and hide a broken context behind a renderer verdict.
        REQUIRE_FALSE(gl_renderer_is_software(nullptr));
        REQUIRE_FALSE(gl_renderer_is_software(""));
    }
}

TEST_CASE("Software refusal and the 3D draw denylist are independent",
          "[gcode][gl_fallback][egl]") {
    using helix::gcode::gl_renderer_is_denylisted;
    using helix::gcode::gl_renderer_is_software;

    // Panfrost faults inside glDrawArrays but presents through EGL correctly:
    // the measured CB1 gain is real. Folding these two questions into one
    // predicate would cost that board the presentation rung.
    REQUIRE(gl_renderer_is_denylisted("Mali-G31 (Panfrost)"));
    REQUIRE_FALSE(gl_renderer_is_software("Mali-G31 (Panfrost)"));

    // And the inverse: llvmpipe is safe to draw 3D on, but must never be
    // chosen for presentation.
    REQUIRE_FALSE(gl_renderer_is_denylisted("llvmpipe (LLVM 15.0.6, 128 bits)"));
    REQUIRE(gl_renderer_is_software("llvmpipe (LLVM 15.0.6, 128 bits)"));
}

// ---------------------------------------------------------------------------
// GPU triangle budget: slow render drivers cap geometry by triangle count
//
// Memory is not the only ceiling on 3D geometry. VideoCore IV (Pi 0-3) draws
// ~4M triangles/s, so a memory-approved tier whose frames take multiple
// seconds trips the kernel's GPU hang check and wedges the GPU until reboot.
// The budget is keyed off the kernel driver behind the DRM render node, not
// the GL_RENDERER string, because the tier is chosen before any GL context
// exists. These tests FAIL if the vc4 cap or its exclusions stop working.
// ---------------------------------------------------------------------------

TEST_CASE("gpu_triangle_budget caps vc4 render drivers", "[gcode][gl_fallback]") {
    using helix::gcode::gpu_triangle_budget;
    using helix::gcode::kVc4TriangleBudget;

    // "vc4-drm" is the kernel driver symlink target on a Pi 3B render node.
    REQUIRE(gpu_triangle_budget("vc4-drm") == kVc4TriangleBudget);
    REQUIRE(gpu_triangle_budget("VC4-DRM") == kVc4TriangleBudget);
    REQUIRE(gpu_triangle_budget("vc4") == kVc4TriangleBudget);
    REQUIRE(kVc4TriangleBudget == 1'000'000);
}

TEST_CASE("gpu_triangle_budget leaves every other driver uncapped", "[gcode][gl_fallback]") {
    using helix::gcode::gpu_triangle_budget;

    // v3d (Pi 4/5) and panfrost draw far faster than vc4; 0 means no cap.
    REQUIRE(gpu_triangle_budget("v3d") == 0);
    REQUIRE(gpu_triangle_budget("panfrost") == 0);
    REQUIRE(gpu_triangle_budget("") == 0);
    REQUIRE(gpu_triangle_budget(nullptr) == 0);
}

// ---------------------------------------------------------------------------
// Init-failure fallback: "no GL at all" must reach the viewer (issue #1555)
//
// The denylist and fatal-draw-error layers above only fire once a GL context
// exists. When the context itself cannot be created — a --test run under
// SDL_VIDEODRIVER=dummy has no GL, and headless/embedded boards can lack EGL
// devices — render() returns "GPU not available" every frame without drawing
// and without any signal the viewer can poll. The viewer's only fallback
// trigger is render_failed(), so an init failure must surface there too, or
// the pane stays blank for a whole print with the 2D CPU renderer idle.
//
// The test binary forces SDL's dummy video driver process-wide (static
// initializer in tests/helix_test_fixture.cpp), and SDL2 self-initializes
// video inside SDL_CreateWindow — so on a desktop with working GL the window
// would otherwise succeed and this test would spuriously fail. The dummy
// driver has no OpenGL, which is the same wall a --test run of the app hits.
// Escape hatch: SDL_VIDEODRIVER already set to something else wins (the
// fixture forces with overwrite=0), so a developer who exports x11 on a GL
// box gets a red here that is environmental, not a code bug.
// ---------------------------------------------------------------------------

#ifdef ENABLE_GLES_3D
#include "gcode_camera.h"
#include "gcode_gles_renderer.h"
#include "gcode_parser.h"

#include <SDL.h>

TEST_CASE("GLES init failure surfaces through render_failed", "[gcode][gl_fallback][1555]") {
    const Uint32 video_before = SDL_WasInit(SDL_INIT_VIDEO);
    helix::gcode::GCodeGLESRenderer renderer;
    const helix::gcode::ParsedGCodeFile empty_gcode;
    const helix::gcode::GCodeCamera camera;

    // render() attempts GL init first and returns before touching the layer
    // or geometry when init fails, so null layer/coords are safe here.
    renderer.render(nullptr, empty_gcode, camera, nullptr);

    REQUIRE(renderer.render_failed());
    // A failed init leaves SDL video as it found it, or later tests get an OS clipboard.
    CHECK(SDL_WasInit(SDL_INIT_VIDEO) == video_before);
}

#endif // ENABLE_GLES_3D
