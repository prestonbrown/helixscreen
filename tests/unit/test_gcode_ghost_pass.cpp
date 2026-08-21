// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"
#include "gcode_selection_style.h"

#include <algorithm>
#include <chrono>
#include <glm/glm.hpp>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

// ---------------------------------------------------------------------------
// Characterization tests for the ghost pass.
//
// The ghost had no pixel coverage at all, which is awkward because it is the
// half of the renderer that runs on a BACKGROUND THREAD. Its correctness rests
// on snapshotting every piece of shared state at thread start - transform,
// visibility flags, colour, tool palette, line width, selection, and the
// data-source pointers - and today that rests in turn on a comment block that
// an edit could quietly violate.
//
// These pin what the ghost currently draws, so that a refactor of the segment
// loop can be shown to preserve it rather than argued to. They assert
// properties, not golden bytes, except where determinism itself is the property.
// ---------------------------------------------------------------------------

namespace {

constexpr int kCanvas = 200;
constexpr size_t kBufBytes = static_cast<size_t>(kCanvas) * kCanvas * 4;

/// Two stacked objects, tall enough that a mid-stack current layer leaves a real
/// ghost above it.
ParsedGCodeFile make_two_object_tower(int layer_count) {
    ParsedGCodeFile gcode;
    const int16_t left = gcode.intern_object_name("left_box");
    const int16_t right = gcode.intern_object_name("right_box");

    for (int i = 0; i < layer_count; ++i) {
        Layer layer;
        const float z = 0.2f * static_cast<float>(i + 1);
        layer.z_height = z;

        auto ring = [&](int16_t obj, float x0, float x1) {
            const glm::vec3 c[4] = {{x0, 20.0f, z}, {x1, 20.0f, z}, {x1, 55.0f, z}, {x0, 55.0f, z}};
            for (int k = 0; k < 4; ++k) {
                ToolpathSegment seg;
                seg.start = c[k];
                seg.end = c[(k + 1) % 4];
                seg.is_extrusion = true;
                seg.object_name_index = obj;
                layer.segments.push_back(seg);
                layer.bounding_box.expand(seg.start);
                layer.bounding_box.expand(seg.end);
            }
        };
        ring(left, 15.0f, 45.0f);
        ring(right, 55.0f, 85.0f);

        layer.segment_count_extrusion = 8;
        gcode.layers.push_back(std::move(layer));
        gcode.total_segments += 8;
    }

    gcode.global_bounding_box.expand(glm::vec3(15.0f, 20.0f, 0.2f));
    gcode.global_bounding_box.expand(
        glm::vec3(85.0f, 55.0f, 0.2f * static_cast<float>(layer_count)));
    return gcode;
}

void configure(GCodeLayerRenderer& r, ParsedGCodeFile& gcode, bool ghost) {
    r.set_gcode(&gcode);
    r.set_view_mode(GCodeLayerRenderer::ViewMode::FRONT);
    r.set_ssao_enabled(false);      // isolate the ghost from the shading pass
    r.set_antialias_enabled(false); // and from the AA fringe, now a separate flag
    r.set_extrusion_color(lv_color_hex(0x3060C0));
    r.set_ghost_mode(ghost);
    r.set_canvas_size(kCanvas, kCanvas);
}

/// Pump frames until the solid cache and the background ghost thread have both
/// settled. Bounded by WALL CLOCK, not by an iteration count: the ghost build
/// finishes on another thread, this loop spins far faster than that thread runs,
/// and a fixed 2000-iteration ceiling trips on a loaded machine long before the
/// worker is actually late. Once there is nothing left to draw, yield instead of
/// spinning so the worker gets the CPU.
template <typename Frame> void settle(GCodeLayerRenderer& r, Frame&& frame) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(10);
    long frames = 0;
    long spawns = 0;
    bool was_running = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!r.needs_more_frames() && !r.is_ghost_build_running()) {
            break;
        }
        frame();
        ++frames;
        // render() restarts the ghost build whenever the cache is invalid and no
        // thread is running, so a rising edge here separates "the worker was
        // starved" from "the worker kept being respawned".
        const bool running = r.is_ghost_build_running();
        if (running && !was_running) {
            ++spawns;
        }
        was_running = running;
        if (!r.needs_more_frames() && running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    UNSCOPED_INFO("settle: " << ms << "ms " << frames << " frames " << spawns
                             << " ghost spawn(s) needs_more=" << r.needs_more_frames()
                             << " running=" << r.is_ghost_build_running());
    REQUIRE_FALSE(r.needs_more_frames()); // never settled: harness bug
    REQUIRE_FALSE(r.is_ghost_build_running());
}

/// Drive until BOTH the solid cache and the ghost thread have settled, then take
/// one clean blit. The clear before the final frame matters for the same reason
/// it does in the SSAO tests: the blits composite, and a build that takes a
/// different number of frames would otherwise differ on partial-alpha pixels.
void drive_with_ghost(GCodeLayerRenderer& r, lv_obj_t* canvas, uint8_t* buf) {
    lv_obj_update_layout(canvas);
    auto frame = [&]() {
        lv_layer_t layer;
        lv_area_t clip = {0, 0, kCanvas - 1, kCanvas - 1};
        lv_canvas_init_layer(canvas, &layer);
        r.render(&layer, &clip);
        lv_canvas_finish_layer(canvas, &layer);
        lv_timer_handler_safe();
    };
    for (int i = 0; i < 3; ++i) {
        frame();
    }
    settle(r, frame);
    // One more pass so the finished ghost buffer is copied in and composited.
    frame();
    std::fill(buf, buf + kBufBytes, uint8_t{0});
    frame();
}

std::vector<uint8_t> snapshot(const uint8_t* buf) {
    return std::vector<uint8_t>(buf, buf + kBufBytes);
}

size_t differing_pixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    REQUIRE(a.size() == b.size());
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3]) {
            ++n;
        }
    }
    return n;
}

size_t painted_pixels(const std::vector<uint8_t>& px) {
    size_t n = 0;
    for (size_t i = 3; i < px.size(); i += 4) {
        if (px[i] != 0) {
            ++n;
        }
    }
    return n;
}

/// Count pixels whose colour is close to `rgb`, allowing for the ghost's wash
/// and the renderer's depth shading.
size_t near_hue(const std::vector<uint8_t>& px, uint8_t r, uint8_t g, uint8_t b, int tol) {
    size_t n = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (px[i + 3] == 0) {
            continue;
        }
        // ARGB8888 in memory is B, G, R, A.
        if (std::abs(static_cast<int>(px[i + 2]) - r) <= tol &&
            std::abs(static_cast<int>(px[i + 1]) - g) <= tol &&
            std::abs(static_cast<int>(px[i + 0]) - b) <= tol) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ghost mode draws the layers above the current one",
                 "[layer_renderer][ghost]") {
    auto gcode = make_two_object_tower(30);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer off;
    configure(off, gcode, /*ghost=*/false);
    off.set_current_layer(10);
    drive_with_ghost(off, canvas, buf);
    const auto without = snapshot(buf);

    GCodeLayerRenderer on;
    configure(on, gcode, /*ghost=*/true);
    on.set_current_layer(10);
    drive_with_ghost(on, canvas, buf);
    const auto with = snapshot(buf);

    INFO("painted without ghost: " << painted_pixels(without)
                                   << ", with ghost: " << painted_pixels(with));
    // The ghost adds the unprinted remainder, so it must cover strictly more.
    REQUIRE(painted_pixels(with) > painted_pixels(without));
}

TEST_CASE_METHOD(LVGLTestFixture, "the ghost pass is deterministic", "[layer_renderer][ghost]") {
    // The property any refactor of the segment loop has to preserve, and the
    // reason it can be checked at all despite the work happening on another
    // thread: the SCHEDULE is nondeterministic, the OUTPUT must not be.
    auto gcode = make_two_object_tower(30);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer a;
    configure(a, gcode, true);
    a.set_current_layer(12);
    drive_with_ghost(a, canvas, buf);
    const auto first = snapshot(buf);

    GCodeLayerRenderer b;
    configure(b, gcode, true);
    b.set_current_layer(12);
    drive_with_ghost(b, canvas, buf);
    const auto second = snapshot(buf);

    CHECK(differing_pixels(first, second) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "the ghost is washed toward white, not just dimmed",
                 "[layer_renderer][ghost]") {
    // wash_to_white() is the ghost's defining treatment: unprinted geometry
    // reads as pale rather than dark, so it sits behind the solid without
    // being mistaken for it. A refactor that dropped the wash and merely
    // lowered opacity would still look "faded" in a screenshot.
    auto gcode = make_two_object_tower(30);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    // Everything is ghost: nothing has printed yet.
    GCodeLayerRenderer r;
    configure(r, gcode, true);
    r.set_current_layer(0);
    drive_with_ghost(r, canvas, buf);
    const auto px = snapshot(buf);

    REQUIRE(painted_pixels(px) > 0);

    // The filament is a saturated blue (0x3060C0): r=48, b=192, so the source
    // channel RATIO is 48/192 = 0.25.
    //
    // The ratio is the discriminator, and picking it took a mutation to find.
    // The first version of this test asserted that the blue/red GAP narrowed and
    // that red stayed off the floor - and both of those are satisfied by simply
    // DIMMING, because dimming scales every channel equally. Deleting
    // wash_to_white() left the test green. Washing toward white by 15% takes red
    // to 48 + 207*0.15 = 79 and blue to 192 + 63*0.15 = 201, a ratio of 0.39;
    // dimming leaves it at 0.25 no matter how dark it gets.
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0, n = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (px[i + 3] == 0) {
            continue;
        }
        sum_b += px[i];
        sum_g += px[i + 1];
        sum_r += px[i + 2];
        ++n;
    }
    REQUIRE(n > 0);
    const double avg_r = static_cast<double>(sum_r) / static_cast<double>(n);
    const double avg_g = static_cast<double>(sum_g) / static_cast<double>(n);
    const double avg_b = static_cast<double>(sum_b) / static_cast<double>(n);
    REQUIRE(avg_b > 1.0); // else the ratio below is meaningless
    const double ratio = avg_r / avg_b;
    INFO("ghost average rgb = " << avg_r << ", " << avg_g << ", " << avg_b
                                << "  r/b ratio = " << ratio << " (source 0.25)");

    // Comfortably above the source ratio, and unreachable by any amount of
    // uniform dimming.
    CHECK(ratio > 0.30);
}

TEST_CASE_METHOD(LVGLTestFixture, "an excluded object is recoloured in the ghost too",
                 "[layer_renderer][ghost]") {
    // Exclusion has to be visible on unprinted geometry: that is exactly the
    // geometry a user is deciding whether to cancel.
    auto gcode = make_two_object_tower(30);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer plain;
    configure(plain, gcode, true);
    plain.set_current_layer(0);
    drive_with_ghost(plain, canvas, buf);
    const auto before = snapshot(buf);

    GCodeLayerRenderer excluded;
    configure(excluded, gcode, true);
    excluded.set_current_layer(0);
    excluded.set_excluded_objects({"left_box"});
    drive_with_ghost(excluded, canvas, buf);
    const auto after = snapshot(buf);

    const uint8_t ex_r = (selection::kExcludedColor >> 16) & 0xFF;
    const uint8_t ex_g = (selection::kExcludedColor >> 8) & 0xFF;
    const uint8_t ex_b = selection::kExcludedColor & 0xFF;

    const size_t hue_before = near_hue(before, ex_r, ex_g, ex_b, 60);
    const size_t hue_after = near_hue(after, ex_r, ex_g, ex_b, 60);
    INFO("exclusion-hue pixels before=" << hue_before << " after=" << hue_after);
    CHECK(hue_after > hue_before);
}

TEST_CASE_METHOD(LVGLTestFixture, "selecting an object marks it in the ghost",
                 "[layer_renderer][ghost]") {
    // The ghost is what is on screen for most of a print, so a selection cue
    // that skipped it would be a cue you cannot see while printing.
    auto gcode = make_two_object_tower(30);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer plain;
    configure(plain, gcode, true);
    plain.set_current_layer(0);
    drive_with_ghost(plain, canvas, buf);
    const auto before = snapshot(buf);

    GCodeLayerRenderer selected;
    configure(selected, gcode, true);
    selected.set_current_layer(0);
    selected.set_highlighted_objects({"left_box"});
    drive_with_ghost(selected, canvas, buf);
    const auto after = snapshot(buf);

    INFO("pixels changed by selecting in the ghost: " << differing_pixels(before, after));
    CHECK(differing_pixels(before, after) > 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "the ghost worker uses the selection as of its spawn",
                 "[layer_renderer][ghost]") {
    // The worker takes SelectionState BY VALUE at std::thread construction
    // rather than reading the member, so the main thread can rebuild its index
    // map mid-render without tearing what the worker sees. This does not prove
    // the absence of a race - only a sanitizer run does that - but it does pin
    // the observable half: a selection set before the build must be reflected,
    // and each rebuild must pick up the current one rather than a stale copy.
    auto gcode = make_two_object_tower(30);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer r;
    configure(r, gcode, true);
    r.set_current_layer(0);
    r.set_excluded_objects({"left_box"});
    drive_with_ghost(r, canvas, buf);
    const auto left_excluded = snapshot(buf);

    // Swap which object is excluded. This invalidates the ghost and respawns the
    // worker, which must see the NEW set.
    r.set_excluded_objects({"right_box"});
    drive_with_ghost(r, canvas, buf);
    const auto right_excluded = snapshot(buf);

    INFO("pixels differing between the two exclusions: " << differing_pixels(left_excluded,
                                                                             right_excluded));
    CHECK(differing_pixels(left_excluded, right_excluded) > 0);

    // And going back reproduces the first image exactly, which a stale captured
    // copy would not.
    r.set_excluded_objects({"left_box"});
    drive_with_ghost(r, canvas, buf);
    CHECK(differing_pixels(left_excluded, snapshot(buf)) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "main-thread setters during a ghost build do not race the worker",
                 "[layer_renderer][ghost][threading]") {
    // This test exists to be run under ThreadSanitizer. Green here proves very
    // little on its own; the point is that it drives the interleaving that the
    // snapshot was introduced to make safe.
    //
    // The worker used to capture color_extrusion_, tool_palette_ and the whole
    // transform ITSELF, on the worker thread, under a comment reading "capture
    // ALL shared state at thread start". That window is precisely when the main
    // thread is free to be writing, and set_extrusion_color(), set_scale(),
    // set_offset(), set_content_offset_y() and set_canvas_size() all wrote into
    // it without joining first. set_tool_color_palette() was the one that had
    // been hardened, and its comment names the hazard.
    //
    // capture_ghost_snapshot() now runs on the spawning thread and the worker
    // reads nothing but its own copy, so the writes below cannot be observed by
    // it at all.
    auto gcode = make_two_object_tower(60);
    lv_obj_t* canvas = lv_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    static uint8_t buf[kBufBytes];
    lv_canvas_set_buffer(canvas, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_ARGB8888);

    GCodeLayerRenderer r;
    configure(r, gcode, /*ghost=*/true);
    r.set_current_layer(0);
    lv_obj_update_layout(canvas);

    auto frame = [&]() {
        lv_layer_t layer;
        lv_area_t clip = {0, 0, kCanvas - 1, kCanvas - 1};
        lv_canvas_init_layer(canvas, &layer);
        r.render(&layer, &clip);
        lv_canvas_finish_layer(canvas, &layer);
        lv_timer_handler_safe();
    };

    // Get past warm-up so the ghost worker is actually spawned.
    for (int i = 0; i < 4; ++i) {
        frame();
    }

    // Hammer the setters that used to be unsynchronized, for a FIXED number of
    // rounds rather than "until the build finishes". Several of them invalidate
    // the ghost, so a wait-for-completion loop here never converges: each round
    // respawns the worker it is waiting on. The first version of this test did
    // exactly that, spun to its guard, and passed anyway when run alongside
    // other cases purely on timing.
    constexpr int kRounds = 150;
    for (int i = 1; i <= kRounds; ++i) {
        r.set_extrusion_color(lv_color_hex(i & 1 ? 0x3060C0 : 0xC06030));
        r.set_scale(1.0f + static_cast<float>(i % 7) * 0.01f);
        r.set_offset(static_cast<float>(i % 5), static_cast<float>(i % 3));
        r.set_content_offset_y(static_cast<float>(i % 3) * 0.05f);
        frame();
    }

    // Stop mutating, then let both the cache and the worker settle.
    r.set_extrusion_color(lv_color_hex(0x3060C0));
    r.set_scale(1.0f);
    r.set_offset(0.0f, 0.0f);
    r.set_content_offset_y(0.0f);

    settle(r, frame);
    frame();
    std::fill(buf, buf + kBufBytes, uint8_t{0});
    frame();
    CHECK(painted_pixels(snapshot(buf)) > 0);
}
