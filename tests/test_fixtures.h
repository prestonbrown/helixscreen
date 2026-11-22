// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file test_fixtures.h
 * @brief Reusable test fixtures for HelixScreen unit tests
 *
 * Provides pre-configured fixture classes that extend LVGLTestFixture with
 * common mock setups. Use these to eliminate boilerplate in test files.
 *
 * Available Fixtures:
 * - LVGLTestFixture: Base fixture with LVGL display (from lvgl_test_fixture.h)
 * - MoonrakerTestFixture: LVGL + PrinterState + MoonrakerClient/API
 * - UITestFixture: LVGL + UITest input simulation
 *
 * Usage:
 * @code
 * TEST_CASE_METHOD(MoonrakerTestFixture, "Test name", "[tags]") {
 *     // api() and state() are ready to use
 *     api().home_all([](bool) {}, [](const std::string&) {});
 *     process_lvgl(100);
 * }
 * @endcode
 *
 * @see lvgl_test_fixture.h for base class documentation
 * @see ui_test_utils.h for UITest input simulation
 */

#include "ui_test_utils.h"

#include "asset_manager.h"
#include "lvgl_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <memory>

using namespace helix;

// ============================================================================
// MoonrakerTestFixture - For testing Moonraker API interactions
// ============================================================================

/**
 * @brief Test fixture with pre-initialized PrinterState and MoonrakerAPI
 *
 * Provides:
 * - Initialized LVGL display (from LVGLTestFixture)
 * - PrinterState with subjects initialized (skip_xml=true for tests)
 * - Disconnected MoonrakerClient (validation happens before network I/O)
 * - MoonrakerAPI ready for testing
 *
 * Use for tests that need to verify API behavior without network connectivity.
 */
class MoonrakerTestFixture : public LVGLTestFixture {
  public:
    MoonrakerTestFixture();
    ~MoonrakerTestFixture() override;

    // Non-copyable, non-movable
    MoonrakerTestFixture(const MoonrakerTestFixture&) = delete;
    MoonrakerTestFixture& operator=(const MoonrakerTestFixture&) = delete;
    MoonrakerTestFixture(MoonrakerTestFixture&&) = delete;
    MoonrakerTestFixture& operator=(MoonrakerTestFixture&&) = delete;

    /**
     * @brief Get the printer state for this test
     * @return Reference to initialized PrinterState
     */
    PrinterState& state() {
        return m_state;
    }

    /**
     * @brief Get the Moonraker client (disconnected)
     * @return Reference to MoonrakerClient
     */
    MoonrakerClient& client() {
        return *m_client;
    }

    /**
     * @brief Get the Moonraker API
     * @return Reference to MoonrakerAPI
     */
    MoonrakerAPI& api() {
        return *m_api;
    }

  protected:
    PrinterState m_state;
    std::unique_ptr<MoonrakerClient> m_client;
    std::unique_ptr<MoonrakerAPI> m_api;
};

// ============================================================================
// UITestFixture - For testing UI interactions
// ============================================================================

/**
 * @brief Test fixture with LVGL and UITest input simulation
 *
 * Provides:
 * - Initialized LVGL display (from LVGLTestFixture)
 * - UITest virtual input device for click/type simulation
 *
 * Use for tests that need to simulate user interactions like clicking buttons
 * or typing into text fields.
 *
 * @note UITest::cleanup() is called automatically in destructor
 */
class UITestFixture : public LVGLTestFixture {
  public:
    UITestFixture();
    ~UITestFixture() override;

    // Non-copyable, non-movable
    UITestFixture(const UITestFixture&) = delete;
    UITestFixture& operator=(const UITestFixture&) = delete;
    UITestFixture(UITestFixture&&) = delete;
    UITestFixture& operator=(UITestFixture&&) = delete;

    /**
     * @brief Simulate click on widget center
     * @param widget Widget to click
     * @return true if click was simulated
     */
    bool click(lv_obj_t* widget) {
        return UITest::click(widget);
    }

    /**
     * @brief Simulate click at coordinates
     * @param x X coordinate
     * @param y Y coordinate
     * @return true if click was simulated
     */
    bool click_at(int32_t x, int32_t y) {
        return UITest::click_at(x, y);
    }

    /**
     * @brief Type text into textarea
     * @param textarea Target textarea
     * @param text Text to type
     * @return true if text was typed
     */
    bool type_text(lv_obj_t* textarea, const std::string& text) {
        return UITest::type_text(textarea, text);
    }

    // wait_until() is inherited from LVGLTestFixture. A wrapper here would have
    // hidden the base version — and UITest::wait_until() is the weaker one, so
    // the shadowing would silently cost you virtual-clock advance.
};

// ============================================================================
// FullMoonrakerTestFixture - MoonrakerTestFixture + UITest
// ============================================================================

/**
 * @brief Combined fixture with Moonraker API and UI input simulation
 *
 * Provides everything from both MoonrakerTestFixture and UITestFixture.
 * Use for integration tests that need both API interactions and UI simulation.
 */
class FullMoonrakerTestFixture : public MoonrakerTestFixture {
  public:
    FullMoonrakerTestFixture();
    ~FullMoonrakerTestFixture() override;

    // Non-copyable, non-movable
    FullMoonrakerTestFixture(const FullMoonrakerTestFixture&) = delete;
    FullMoonrakerTestFixture& operator=(const FullMoonrakerTestFixture&) = delete;
    FullMoonrakerTestFixture(FullMoonrakerTestFixture&&) = delete;
    FullMoonrakerTestFixture& operator=(FullMoonrakerTestFixture&&) = delete;

    // UITest convenience wrappers
    bool click(lv_obj_t* widget) {
        return UITest::click(widget);
    }
    bool click_at(int32_t x, int32_t y) {
        return UITest::click_at(x, y);
    }
    bool type_text(lv_obj_t* textarea, const std::string& text) {
        return UITest::type_text(textarea, text);
    }
    // wait_until() inherited from LVGLTestFixture — see UITestFixture.
};

// ============================================================================
// Test Helper Functions
// ============================================================================

namespace TestHelpers {

/**
 * @brief Create a simple test label
 * @param parent Parent object
 * @param text Label text
 * @return Created label
 */
inline lv_obj_t* create_test_label(lv_obj_t* parent, const char* text) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    return label;
}

/**
 * @brief Create a simple test button
 * @param parent Parent object
 * @param text Button label text
 * @return Created button
 */
inline lv_obj_t* create_test_button(lv_obj_t* parent, const char* text) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    return btn;
}

/**
 * @brief Create a simple test textarea
 * @param parent Parent object
 * @param placeholder Placeholder text
 * @return Created textarea
 */
inline lv_obj_t* create_test_textarea(lv_obj_t* parent, const char* placeholder = "") {
    lv_obj_t* ta = lv_textarea_create(parent);
    if (placeholder && placeholder[0] != '\0') {
        lv_textarea_set_placeholder_text(ta, placeholder);
    }
    return ta;
}

} // namespace TestHelpers

// ============================================================================
// XMLTestFixture - For testing XML components with full theme/subject support
// ============================================================================

/**
 * @brief Test fixture for loading and testing XML components
 *
 * Extends MoonrakerTestFixture with:
 * - Font registration via AssetManager::register_all()
 * - globals.xml component registration (for theme constants)
 * - Theme initialization via theme_manager_init()
 * - Helpers to register and create XML components
 * - Subject registration for XML bindings
 *
 * Use for tests that need to load real XML component files and test their
 * rendering, bindings, and behavior.
 *
 * @code
 * TEST_CASE_METHOD(XMLTestFixture, "Test XML component", "[xml]") {
 *     REQUIRE(register_component("home_panel"));
 *     auto* panel = create_component("home_panel");
 *     REQUIRE(panel != nullptr);
 *     process_lvgl(100);
 * }
 * @endcode
 */
class XMLTestFixture : public LVGLTestFixture {
  public:
    XMLTestFixture();
    ~XMLTestFixture() override;

    // Non-copyable, non-movable
    XMLTestFixture(const XMLTestFixture&) = delete;
    XMLTestFixture& operator=(const XMLTestFixture&) = delete;
    XMLTestFixture(XMLTestFixture&&) = delete;
    XMLTestFixture& operator=(XMLTestFixture&&) = delete;

    /**
     * @brief Get the printer state for this test
     * @return Reference to the global PrinterState (get_printer_state())
     *
     * Reset to a fresh session at construction, its subjects registered in the
     * global LVGL XML scope, so XML bindings and C++ code that reads
     * get_printer_state() share one set of subjects, as they do in the app. The
     * destructor tears down all UI components and leaves the state up.
     */
    PrinterState& state() {
        return m_state;
    }

    /**
     * @brief Get the Moonraker client (disconnected)
     * @return Reference to MoonrakerClient
     */
    MoonrakerClient& client() {
        return *m_client;
    }

    /**
     * @brief Get the Moonraker API
     * @return Reference to MoonrakerAPI
     */
    MoonrakerAPI& api() {
        return *m_api;
    }

    /**
     * @brief Register an XML component file for use in tests
     *
     * Loads component definition from ui_xml/{component_name}.xml.
     * Must be called before create_component() for that component.
     *
     * @param component_name Name like "home_panel" (will load from ui_xml/{name}.xml)
     * @return true if registration succeeded
     */
    bool register_component(const char* component_name);

    /**
     * @brief Create an XML component on the test screen
     *
     * Creates an instance of a previously registered component.
     * Automatically registers subjects if not already done.
     *
     * @param component_name Name of previously registered component
     * @return Root object of created component, or nullptr on failure
     */
    lv_obj_t* create_component(const char* component_name);

    /**
     * @brief Create an XML component with attributes on the test screen
     *
     * Creates an instance of a previously registered component with custom attributes.
     * Automatically registers subjects if not already done.
     *
     * @param component_name Name of previously registered component
     * @param attrs NULL-terminated array of key-value pairs, e.g.:
     *              {"bind_current", "extruder_temp", "bind_target", "extruder_target", nullptr}
     * @return Root object of created component, or nullptr on failure
     */
    lv_obj_t* create_component(const char* component_name, const char** attrs);

    /**
     * @brief One-time global XML registrations shared across all XMLTestFixture tests
     *
     * Registers XML widget classes, fonts, globals.xml, event-cb no-ops, and
     * initializes the theme. These are process-wide side effects on the LVGL XML
     * registry that only need to happen once. Guarded by s_global_registered.
     *
     * A fixture built on LVGLTestFixture calls it too when it creates XML
     * components that use design tokens or HelixScreen widgets. Call it with no
     * screen present: theme initialization hangs when one exists.
     */
    static void setup_global_xml_registrations_once();

  private:
    static bool s_global_registered;

    PrinterState& m_state;
    std::unique_ptr<MoonrakerClient> m_client;
    std::unique_ptr<MoonrakerAPI> m_api;
};
