// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_led_control_overlay.cpp
 * @brief Unit tests for LedControlOverlay color-picker visibility gating.
 *
 * The quick-control overlay must only show the color swatches + custom-color
 * button when the currently selected NATIVE strip is actually RGB-capable.
 * White-only strips (e.g. Flashforge AD5M `[led chamber_light]` with only a
 * white_pin, supports_color=false) used to show the picker; picking a color
 * silently converted RGB->white luminance, which is misleading UX.
 *
 * @see ui_led_control_overlay.h
 * @see LedSettingsOverlay::populate_auto_state_rows() (the mirrored capability gate)
 */

#include "../lvgl_test_fixture.h"
#include "led/led_backend.h"
#include "led/led_controller.h"
#include "led/ui_led_control_overlay.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::led;

// Test access to exercise the private color-picker visibility gate. Keeps
// test-only entry points out of the production class (L065). Constructs an
// overlay, drives selected_backend_type_ + update_section_visibility(), and
// reads back the color_visible_ subject.
//
// Must live in namespace helix::led to match the `friend class
// LedControlOverlayTestAccess;` declaration inside that namespace.
namespace helix::led {
class LedControlOverlayTestAccess {
  public:
    explicit LedControlOverlayTestAccess(helix::PrinterState& ps) : overlay_(ps) {
        overlay_.init_subjects();
    }
    // overlay_.subjects_ (SubjectManager) deinits its LVGL subjects in its own
    // destructor — no explicit teardown needed here.

    void set_backend(LedBackendType type) {
        overlay_.selected_backend_type_ = type;
    }

    int compute_color_visible() {
        overlay_.update_section_visibility();
        return lv_subject_get_int(&overlay_.color_visible_);
    }

    // Drives the chip-tap path so the selection semantics can be asserted.
    void tap_chip(const std::string& strip_id) {
        overlay_.handle_strip_selected(strip_id);
    }

    [[nodiscard]] LedBackendType backend() const {
        return overlay_.selected_backend_type_;
    }

    [[nodiscard]] std::string active_strip_name() {
        const char* s = lv_subject_get_string(&overlay_.strip_name_subject_);
        return s != nullptr ? std::string(s) : std::string();
    }

    // The backend fan-out gate: which selected strips a given backend's commands
    // are allowed to reach.
    [[nodiscard]] std::vector<std::string> targets_for(LedBackendType type) {
        return overlay_.target_strips_for(type);
    }

    // Seed the manual-color state the swatch and brightness handlers maintain.
    void set_manual_state(uint32_t color, int brightness, double white) {
        overlay_.current_color_ = color;
        overlay_.current_brightness_ = brightness;
        overlay_.current_white_ = white;
    }

    // The helper under test: turns the manual state into a strip command.
    void apply_current_color() {
        overlay_.apply_current_color();
    }

    // The brightness slider's real handler, which routes into apply_current_color()
    // for every backend except output_pin.
    void change_brightness(int pct) {
        overlay_.handle_brightness_change(pct);
    }

  private:
    helix::led::LedControlOverlay overlay_;
};
} // namespace helix::led

using helix::led::LedControlOverlayTestAccess;

namespace {

// Build a native strip with the requested color capability.
LedStripInfo make_native_strip(const std::string& id, bool supports_color, bool supports_white) {
    LedStripInfo strip;
    strip.name = id;
    strip.id = id;
    strip.backend = LedBackendType::NATIVE;
    strip.supports_color = supports_color;
    strip.supports_white = supports_white;
    return strip;
}

// A controller wired to a mock API. NativeBackend::set_color() refuses outright
// when it has no API, so the color a command actually carries is observable only
// with one attached — the null-API fixture the visibility tests use would make
// every assertion below pass vacuously.
struct LedApplyColorFixture : public LVGLTestFixture {
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> mock_api;

    LedApplyColorFixture() {
        state.init_subjects(false);
        // execute_gcode()'s halted gate would otherwise reject every SET_LED:
        // subjects sit at SHUTDOWN until production observes a real state update.
        state.set_klippy_state_sync(helix::KlippyState::READY);
        mock_api = std::make_unique<MoonrakerAPIMock>(mock_client, state);

        auto& ctrl = LedController::instance();
        ctrl.deinit();
        ctrl.init(mock_api.get(), &mock_client);
    }

    ~LedApplyColorFixture() override {
        LedController::instance().deinit();
    }

    void select_strip(const std::string& id, bool supports_color, bool supports_white) {
        auto& ctrl = LedController::instance();
        ctrl.native().add_strip(make_native_strip(id, supports_color, supports_white));
        ctrl.set_selected_strips({id});
    }

    [[nodiscard]] helix::led::NativeBackend::StripColor sent(const std::string& id) const {
        return LedController::instance().native().get_strip_color(id);
    }
};

} // namespace

// ============================================================================
// apply_current_color(): the manual-color state -> what reaches the strip.
//
// #1129 lived here, not in NativeBackend: dimming a white-only strip sent the
// W channel as a literal 0.0 and physically switched the user's light off.
// ============================================================================

TEST_CASE_METHOD(LedApplyColorFixture,
                 "LedControlOverlay: dimming a white-only strip scales W instead of zeroing it",
                 "[led][control_overlay][1129]") {
    select_strip("led case_light", /*color=*/false, /*white=*/true);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::NATIVE);

    // Full white, dimmed to 15% — the state a user reaches by picking the white
    // swatch and pulling the brightness slider down.
    access.set_manual_state(0xFFFFFF, 15, 1.0);
    access.apply_current_color();

    auto c = sent("led case_light");
    CHECK(c.w == Catch::Approx(0.15).margin(0.001));
    CHECK(c.w > 0.0); // the whole point: the light stays on
    CHECK(c.r == Catch::Approx(0.0).margin(0.001));
    CHECK(c.g == Catch::Approx(0.0).margin(0.001));
    CHECK(c.b == Catch::Approx(0.0).margin(0.001));

    // And it round-trips: what the strip reports back decomposes to the same
    // base white at the same brightness.
    uint32_t base_color = 0;
    int brightness = 0;
    double base_white = 0.0;
    c.decompose(base_color, brightness, base_white);
    CHECK(brightness == 15);
    CHECK(base_white == Catch::Approx(1.0).margin(0.01));
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "LedControlOverlay: the brightness slider drives the same white scaling",
                 "[led][control_overlay][1129]") {
    select_strip("led case_light", /*color=*/false, /*white=*/true);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::NATIVE);

    // White selected at full brightness, then the slider moved to 20%. The
    // handler must re-apply through apply_current_color(), not skip the white path.
    access.set_manual_state(0xFFFFFF, 100, 1.0);
    access.change_brightness(20);

    auto c = sent("led case_light");
    CHECK(c.w == Catch::Approx(0.20).margin(0.001));
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "LedControlOverlay: an RGB strip gets scaled RGB and a zero W",
                 "[led][control_overlay]") {
    // Not the "led " prefix, so NativeBackend keeps the RGB channels distinct
    // instead of collapsing them to luminance.
    select_strip("neopixel chamber", /*color=*/true, /*white=*/false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::NATIVE);

    // current_white_ == 0.0 selects the RGB branch: every channel scales by
    // brightness and W is sent as 0 so the dedicated white LED stays dark.
    access.set_manual_state(0xFF0000, 50, 0.0);
    access.apply_current_color();

    auto c = sent("neopixel chamber");
    CHECK(c.r == Catch::Approx(0.5).margin(0.001));
    CHECK(c.g == Catch::Approx(0.0).margin(0.001));
    CHECK(c.b == Catch::Approx(0.0).margin(0.001));
    CHECK(c.w == Catch::Approx(0.0).margin(0.001));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "LedControlOverlay: color picker hidden for white-only native strip",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // White-only strip: e.g. AD5M [led chamber_light] with only white_pin.
    ctrl.native().add_strip(make_native_strip("led chamber_light", /*color=*/false,
                                              /*white=*/true));
    ctrl.set_selected_strips({"led chamber_light"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::NATIVE);

    REQUIRE(access.compute_color_visible() == 0);

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "LedControlOverlay: color picker shown for RGB-capable native strip",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // RGB strip: e.g. neopixel chamber_light.
    ctrl.native().add_strip(make_native_strip("neopixel chamber_light", /*color=*/true,
                                              /*white=*/true));
    ctrl.set_selected_strips({"neopixel chamber_light"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::NATIVE);

    REQUIRE(access.compute_color_visible() == 1);

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "LedControlOverlay: color picker hidden when both RGB and white-only selected "
                 "resolves on any color-capable strip",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.native().add_strip(make_native_strip("led white_only", /*color=*/false, /*white=*/true));
    ctrl.native().add_strip(make_native_strip("neopixel rgb", /*color=*/true, /*white=*/false));

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::NATIVE);

    // Only the white-only strip selected -> hidden.
    ctrl.set_selected_strips({"led white_only"});
    REQUIRE(access.compute_color_visible() == 0);

    // Selection includes a color-capable strip -> shown.
    ctrl.set_selected_strips({"led white_only", "neopixel rgb"});
    REQUIRE(access.compute_color_visible() == 1);

    ctrl.deinit();
}

// ============================================================================
// Strip-chip selection semantics
//
// The chip row renders every strip in the selection as "selected" (std::find
// over the whole vector), and every consumer of selected_strips() iterates the
// whole vector (toggle_all, set_color_all, set_brightness_all,
// light_state_trackable, send_color_to_strips). The tap handler must therefore
// be additive: replacing the selection silently discarded a multi-strip choice
// made in Settings, and on_deactivate() persisted the loss to settings.json.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "LedControlOverlay: tapping an unselected chip adds to selection",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    for (const char* id : {"neopixel a", "neopixel b", "neopixel c", "neopixel d"}) {
        ctrl.native().add_strip(make_native_strip(id, /*color=*/true, /*white=*/false));
    }
    ctrl.set_selected_strips({"neopixel a", "neopixel b", "neopixel c"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.tap_chip("neopixel d");

    const auto& sel = ctrl.selected_strips();
    REQUIRE(sel.size() == 4);
    for (const char* id : {"neopixel a", "neopixel b", "neopixel c", "neopixel d"}) {
        INFO("expected strip still selected: " << id);
        REQUIRE(std::find(sel.begin(), sel.end(), std::string(id)) != sel.end());
    }
    // The tapped strip owns the front slot: selected_strips()[0] is what drives
    // the header, the effects/WLED sections and query_tracked_led_state().
    REQUIRE(sel.front() == "neopixel d");

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "LedControlOverlay: tapping a selected chip removes only that one",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    for (const char* id : {"neopixel a", "neopixel b", "neopixel c"}) {
        ctrl.native().add_strip(make_native_strip(id, /*color=*/true, /*white=*/false));
    }
    ctrl.set_selected_strips({"neopixel a", "neopixel b", "neopixel c"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.tap_chip("neopixel b");

    const auto& sel = ctrl.selected_strips();
    REQUIRE(sel == std::vector<std::string>{"neopixel a", "neopixel c"});
    // Focus falls back to the new front so the header and the visible section
    // describe a strip that is actually still selected.
    REQUIRE(access.active_strip_name() == "neopixel a");

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture, "LedControlOverlay: the last selected chip cannot be deselected",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.native().add_strip(make_native_strip("neopixel a", /*color=*/true, /*white=*/false));
    ctrl.set_selected_strips({"neopixel a"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.tap_chip("neopixel a");

    REQUIRE(ctrl.selected_strips() == std::vector<std::string>{"neopixel a"});

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "LedControlOverlay: tapped chip drives the header name and section backend",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.native().add_strip(make_native_strip("neopixel rgb", /*color=*/true, /*white=*/false));

    LedStripInfo pin;
    pin.name = "Enclosure LEDs";
    pin.id = "output_pin enclosure";
    pin.backend = LedBackendType::OUTPUT_PIN;
    pin.supports_color = false;
    pin.supports_white = false;
    ctrl.output_pin().add_pin(pin);

    ctrl.set_selected_strips({"neopixel rgb"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.tap_chip("output_pin enclosure");

    // Both strips stay selected, but the tapped one owns the header + sections.
    REQUIRE(ctrl.selected_strips().size() == 2);
    REQUIRE(ctrl.selected_strips().front() == "output_pin enclosure");
    REQUIRE(access.backend() == LedBackendType::OUTPUT_PIN);
    REQUIRE(access.active_strip_name() == "Enclosure LEDs");

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "LedControlOverlay: a mixed selection only sends to matching-backend strips",
                 "[led][control_overlay]") {
    // Multi-strip selections became the normal case once tapping a chip stopped
    // collapsing the selection, so a native strip, an output_pin and a macro
    // device routinely sit in selected_strips() together. Fanning a backend's
    // commands across the whole selection sends nonsense to Klipper:
    //   SET_LED LED="enclosure"  -> "Unknown LED"
    //   SET_LED LED="macro:Lamp" -> rejected by is_safe_identifier (the ':'),
    //                               which toasts an error once per slider step
    //   SET_PIN PIN=a            -> for a neopixel, from the output_pin branch
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.native().add_strip(make_native_strip("neopixel rgb", /*color=*/true, /*white=*/false));

    LedStripInfo pin;
    pin.name = "Enclosure LEDs";
    pin.id = "output_pin enclosure";
    pin.backend = LedBackendType::OUTPUT_PIN;
    ctrl.output_pin().add_pin(pin);

    LedMacroInfo lamp;
    lamp.display_name = "Lamp";
    lamp.type = MacroLedType::TOGGLE;
    lamp.toggle_macro = "LAMP_TOGGLE";
    ctrl.set_configured_macros({lamp});
    ctrl.rebuild_macro_backend();

    ctrl.set_selected_strips({"neopixel rgb", "output_pin enclosure", "macro:Lamp"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);

    REQUIRE(access.targets_for(LedBackendType::NATIVE) == std::vector<std::string>{"neopixel rgb"});
    REQUIRE(access.targets_for(LedBackendType::OUTPUT_PIN) ==
            std::vector<std::string>{"output_pin enclosure"});

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "LedControlOverlay: a selection with no native strip still falls back",
                 "[led][control_overlay]") {
    // The implicit-target fallback must survive the backend filter: with only an
    // output_pin selected, the native color path still addresses the first native
    // strip rather than the output_pin's id.
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    ctrl.native().add_strip(make_native_strip("neopixel rgb", /*color=*/true, /*white=*/false));

    LedStripInfo pin;
    pin.name = "Enclosure LEDs";
    pin.id = "output_pin enclosure";
    pin.backend = LedBackendType::OUTPUT_PIN;
    ctrl.output_pin().add_pin(pin);

    ctrl.set_selected_strips({"output_pin enclosure"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);

    REQUIRE(access.targets_for(LedBackendType::NATIVE) == std::vector<std::string>{"neopixel rgb"});
    // ...and the output_pin path never picks up the native strip.
    REQUIRE(access.targets_for(LedBackendType::OUTPUT_PIN) ==
            std::vector<std::string>{"output_pin enclosure"});

    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture, "LedControlOverlay: color picker hidden for output_pin backend",
                 "[led][control_overlay]") {
    auto& ctrl = LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);

    // An output_pin LED is brightness-only; the color section must stay hidden
    // regardless of any native strips that happen to exist.
    ctrl.native().add_strip(make_native_strip("neopixel rgb", /*color=*/true, /*white=*/false));
    ctrl.set_selected_strips({"neopixel rgb"});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.set_backend(LedBackendType::OUTPUT_PIN);

    REQUIRE(access.compute_color_visible() == 0);

    ctrl.deinit();
}
