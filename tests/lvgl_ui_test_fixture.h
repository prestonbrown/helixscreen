// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file lvgl_ui_test_fixture.h
 * @brief Full UI integration test fixture with XML component registration
 *
 * This fixture provides a production-like environment for UI integration tests:
 * - Full LVGL initialization with display
 * - Asset registration (fonts, images)
 * - Theme initialization
 * - ALL XML components registered (mirrors production)
 * - All subject initialization (wizard, navigation, etc.)
 * - Event callback registration
 *
 * Use this for tests that need the full XML component tree, such as:
 * - Wizard flow tests
 * - Panel navigation tests
 * - Complex UI interaction tests
 *
 * For simpler binding tests, prefer XMLTestFixture which is faster
 * and registers only the components you need.
 *
 * Initialization follows CLAUDE.md rules:
 * - [L004]: Subjects initialized BEFORE lv_xml_create()
 * - [L013]: Event callbacks registered BEFORE lv_xml_create()
 * - [L041]: Every init_subjects() has corresponding deinit_subjects()
 *
 * Usage:
 * @code
 * TEST_CASE_METHOD(LVGLUITestFixture, "Wizard flow test", "[wizard][ui_integration]") {
 *     // Create wizard - all components and subjects are ready
 *     lv_obj_t* wizard = ui_wizard_create(test_screen());
 *     REQUIRE(wizard != nullptr);
 *
 *     ui_wizard_navigate_to_step(2);
 *     process_lvgl(100);
 *
 *     // Verify wizard state...
 * }
 * @endcode
 *
 * @see lvgl_test_fixture.h for base class
 * @see test_fixtures.h for XMLTestFixture (selective component registration)
 */

#include "lvgl/lvgl.h"
#include "lvgl_test_fixture.h"
#include "moonraker_api.h"

#include <memory>

// Forward declarations
namespace helix {
class PrinterState;
class MoonrakerClient;
} // namespace helix
class MoonrakerAPI;

using namespace helix;

/**
 * @brief Full UI integration test fixture with production-like initialization
 *
 * Provides complete UI environment matching production Application startup:
 * 1. LVGL display initialization
 * 2. Font and image asset registration
 * 3. Theme initialization (light mode for test consistency)
 * 4. Custom widget registration
 * 5. ALL XML components from ui_xml/ directory
 * 6. Subject initialization (PrinterState, wizard, navigation, etc.)
 * 7. Event callback registration
 *
 * Cleanup order mirrors Application::shutdown() to ensure proper teardown.
 */
class LVGLUITestFixture : public LVGLTestFixture {
  public:
    LVGLUITestFixture();
    ~LVGLUITestFixture() override;

    // Non-copyable, non-movable (owns resources)
    LVGLUITestFixture(const LVGLUITestFixture&) = delete;
    LVGLUITestFixture& operator=(const LVGLUITestFixture&) = delete;
    LVGLUITestFixture(LVGLUITestFixture&&) = delete;
    LVGLUITestFixture& operator=(LVGLUITestFixture&&) = delete;

    /**
     * @brief Get the printer state for this test
     * @return Reference to initialized PrinterState
     */
    PrinterState& state();

    /**
     * @brief Get the Moonraker client (disconnected, for test use)
     * @return Pointer to MoonrakerClient (may be nullptr)
     */
    MoonrakerClient* client();

    /**
     * @brief Get the Moonraker API
     * @return Pointer to MoonrakerAPI (may be nullptr)
     */
    MoonrakerAPI* api();

    /**
     * @brief Check if full initialization completed successfully
     * @return true if all components are initialized
     */
    bool is_fully_initialized() const {
        return m_fully_initialized;
    }

  private:
    /**
     * @brief Initialize assets (fonts, images)
     *
     * Registers all fonts and images via AssetManager.
     * Must happen before theme initialization.
     */
    void init_assets();

    /**
     * @brief Initialize theme
     *
     * Loads globals.xml and initializes the UI theme.
     * Must happen after assets, before XML components.
     */
    void init_theme();

    /**
     * @brief Register custom widgets
     *
     * Registers C++ widgets that XML components depend on.
     * Must happen before XML component registration.
     */
    void register_widgets();

    /**
     * @brief Register all XML components
     *
     * Loads all XML component definitions from ui_xml/.
     * Mirrors production helix::register_xml_components().
     */
    void register_xml_components();

    /**
     * @brief Initialize subjects
     *
     * Initializes all reactive subjects needed for XML bindings.
     * Must happen BEFORE creating any XML components.
     */
    void init_subjects();

    /**
     * @brief Register event callbacks
     *
     * Registers all event callbacks for XML components.
     * Must happen BEFORE creating any XML components.
     */
    void register_event_callbacks();

    /**
     * @brief Clean up all initialized resources
     *
     * Called by destructor. Follows reverse initialization order.
     */
    void cleanup();

    // Owned resources
    std::unique_ptr<MoonrakerClient> m_client;
    std::unique_ptr<MoonrakerAPI> m_api;

    // Initialization state tracking
    bool m_assets_initialized = false;
    bool m_theme_initialized = false;
    bool m_widgets_registered = false;
    bool m_xml_registered = false;
    bool m_subjects_initialized = false;
    bool m_callbacks_registered = false;
    bool m_fully_initialized = false;
};
