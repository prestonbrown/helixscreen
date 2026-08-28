// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_gles_selection_palette.cpp
 * @brief The 3D path's two selection-colour decisions, pinned together.
 *
 * GCodeGLESRenderer composes two things when it stamps the white silhouette into
 * its frame: the outline colour, which comes from ui_xml/gcode_tokens.xml via
 * reset_colors(), and the byte order of the buffer it writes into, which is the
 * GL_RGBA layout glReadPixels hands back rather than the ARGB8888 every other
 * caller of stroke_selection_rim() uses.
 *
 * They have to be tested together because each one hides the other. A palette
 * that is never resolved sits on the struct default of white, and white is the
 * one colour for which the channel order does not matter; a swapped channel
 * order is likewise invisible until the token stops being grey. So the case
 * below overrides the token with a colour that has three distinct channel
 * values, and asserts on the bytes that come out.
 *
 * A unit test cannot drive a real GL context, so what it cannot cover is the
 * literal glReadPixels format argument at the call site — the same limitation
 * test_gcode_gl_fallback.cpp documents. What it does cover is everything
 * downstream of the readback: the palette actually reaching the renderer, and
 * the rim landing the right bytes for the layout that readback produces.
 *
 * On a build without ENABLE_GLES_3D (macOS, and the constrained printer targets)
 * the GLES header preprocesses to nothing and this file contributes no cases.
 */

#include "../test_fixtures.h"
#include "gcode_gles_renderer.h"
#include "gcode_raster.h"
#include "gcode_selection_style.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <cstdint>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

#ifdef ENABLE_GLES_3D

namespace {

/// The shipped outline token is #FFFFFF, and so is the Palette member default,
/// so nothing about a white rim can tell "resolved from XML" apart from "never
/// resolved". This is the orange-red a user gets by editing the token, with
/// three distinct channel values so a swap cannot alias onto a correct answer.
constexpr const char* kSentinelHex = "#FF6B35";
constexpr uint32_t kSentinelRgb = 0xFF6B35u;

/// Swap one registered XML const for the duration of a test and put the original
/// back, so the rest of the suite still reads the shipped value. The const
/// registry is global to the process; every other XML scope in a shard shares it.
class ConstOverride {
  public:
    ConstOverride(const char* name, const char* value) : name_(name) {
        const char* current = lv_xml_get_const_silent(nullptr, name);
        if (current != nullptr) {
            was_registered_ = true;
            saved_ = current;
        }
        lv_xml_update_const(nullptr, name, value);
    }

    ~ConstOverride() {
        // Restoring an unregistered name would leave the value this test wrote
        // in place for the rest of the shard; put the shipped default back
        // instead. was_registered() is asserted below so the surprise is loud.
        lv_xml_update_const(nullptr, name_, was_registered_ ? saved_.c_str() : "#FFFFFF");
    }

    ConstOverride(const ConstOverride&) = delete;
    ConstOverride& operator=(const ConstOverride&) = delete;

    bool was_registered() const {
        return was_registered_;
    }

  private:
    const char* name_;
    std::string saved_;
    bool was_registered_ = false;
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "the GLES renderer resolves its selection palette from the tokens",
                 "[gcode][gles][selection]") {
    ConstOverride outline("gcode_selection_outline", kSentinelHex);
    REQUIRE(outline.was_registered()); // the fixture registered ui_xml's tokens

    // The reader itself sees the override. Without this the case below could
    // pass or fail for a reason that has nothing to do with the renderer.
    REQUIRE(helix::gcode::selection::palette_from_theme().outline == kSentinelRgb);

    helix::gcode::GCodeGLESRenderer renderer;
    REQUIRE(renderer.selection_palette().outline == kSentinelRgb);
}

TEST_CASE_METHOD(XMLTestFixture, "a non-white outline token survives the GLES readback byte order",
                 "[gcode][gles][selection]") {
    ConstOverride outline("gcode_selection_outline", kSentinelHex);
    REQUIRE(outline.was_registered());

    helix::gcode::GCodeGLESRenderer renderer;

    // Stands in for the readback buffer blit_to_lvgl() strokes: 4 bytes per
    // pixel, byte 0 red, every pixel carrying the alpha tag render_selection_tag()
    // leaves behind. Tagging the whole surface makes its border the rim, since
    // off-canvas counts as untagged.
    constexpr int kW = 8;
    constexpr int kH = 8;
    std::vector<uint8_t> readback(static_cast<size_t>(kW) * kH * 4, 0);
    for (size_t i = 3; i < readback.size(); i += 4) {
        readback[i] = helix::gcode::kSelectedAlpha;
    }

    const int rim = helix::gcode::selection::outline_width_px(kW);
    const helix::gcode::RasterTarget rt{readback.data(), static_cast<size_t>(kW) * 4, kW, kH};
    helix::gcode::stroke_selection_rim(rt, rim, rim, renderer.selection_palette().outline,
                                       helix::gcode::ChannelOrder::Rgba);

    // Corner pixel: on the rim from two directions.
    CHECK(readback[0] == 0xFF); // R
    CHECK(readback[1] == 0x6B); // G
    CHECK(readback[2] == 0x35); // B
    // The pass writes colour only, so the tag is still there and a second pass
    // over the same buffer is idempotent.
    CHECK(readback[3] == helix::gcode::kSelectedAlpha);
}

#endif // ENABLE_GLES_3D
