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

#include "../catch_amalgamated.hpp"

using helix::gcode::gl_draw_error_is_fatal;
using helix::gcode::GL_ERR_INVALID_OPERATION;
using helix::gcode::GL_ERR_OUT_OF_MEMORY;
using helix::gcode::gl_renderer_is_denylisted;

TEST_CASE("gl_draw_error_is_fatal triggers fallback on out-of-memory", "[gcode][gl_fallback]") {
    // GL_OUT_OF_MEMORY is the primary CB1/Mali fault signature — must fall back.
    REQUIRE(gl_draw_error_is_fatal(GL_ERR_OUT_OF_MEMORY));
    REQUIRE(gl_draw_error_is_fatal(0x0505)); // literal value guards against renumbering
}

TEST_CASE("gl_draw_error_is_fatal triggers fallback on invalid-operation", "[gcode][gl_fallback]") {
    // GL_INVALID_OPERATION means the command stream is in a driver-rejected
    // state; continuing to draw is unsafe.
    REQUIRE(gl_draw_error_is_fatal(GL_ERR_INVALID_OPERATION));
    REQUIRE(gl_draw_error_is_fatal(0x0502));
}

TEST_CASE("gl_draw_error_is_fatal does NOT fall back on GL_NO_ERROR", "[gcode][gl_fallback]") {
    // The common case — a healthy frame — must keep rendering in 3D. If this
    // returned true the viewer would thrash to 2D on every frame.
    REQUIRE_FALSE(gl_draw_error_is_fatal(0x0000)); // GL_NO_ERROR
}

TEST_CASE("gl_draw_error_is_fatal ignores non-fatal recoverable errors", "[gcode][gl_fallback]") {
    // These errors indicate a programming mistake in the call, but the driver
    // stays in a defined state and the next frame can recover — they must NOT
    // permanently disable GPU rendering.
    REQUIRE_FALSE(gl_draw_error_is_fatal(0x0500)); // GL_INVALID_ENUM
    REQUIRE_FALSE(gl_draw_error_is_fatal(0x0501)); // GL_INVALID_VALUE
    REQUIRE_FALSE(gl_draw_error_is_fatal(0x0506)); // GL_INVALID_FRAMEBUFFER_OPERATION
}

TEST_CASE("gl_draw_error_is_fatal constants match OpenGL ES values", "[gcode][gl_fallback]") {
    // The predicate is header-only (no GL dependency) so it is testable on
    // builds without ENABLE_GLES_3D. Pin the constants to the real GL ES
    // numbers so a typo can never silently disable the guard.
    REQUIRE(GL_ERR_OUT_OF_MEMORY == 0x0505u);
    REQUIRE(GL_ERR_INVALID_OPERATION == 0x0502u);
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
}

TEST_CASE("gl_renderer_is_denylisted is case-insensitive", "[gcode][gl_fallback]") {
    // Driver casing varies across Mesa versions — the match must not depend on it.
    REQUIRE(gl_renderer_is_denylisted("PANFROST"));
    REQUIRE(gl_renderer_is_denylisted("panfrost"));
}

TEST_CASE("gl_renderer_is_denylisted rejects null and empty", "[gcode][gl_fallback]") {
    // glGetString(GL_RENDERER) can return nullptr on a broken context.
    REQUIRE_FALSE(gl_renderer_is_denylisted(nullptr));
    REQUIRE_FALSE(gl_renderer_is_denylisted(""));
}

TEST_CASE("gl_renderer_is_denylisted allows known-good GPUs", "[gcode][gl_fallback]") {
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
