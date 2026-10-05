// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "helix/xml/indexed_subject_pool.h"
#include "led/led_backend.h"
#include "led/led_device_page.h"
#include "overlay_base.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class PrinterState;
}

namespace helix::led {

/**
 * @file ui_led_control_overlay.h
 * @brief The LEDs overlay: one tab per LED device, and a page for the focused one
 *
 * Every control acts on the focused device and nothing else. Focusing a tab
 * never writes the controller's selection, the auto-state targets or config.
 * The page reads its slider and swatch state on focus and activation only; a
 * live status frame updates the tab dots, the power state and the page color.
 *
 * @see LedController for backend discovery and control
 * @see led_control_overlay.xml for layout definition
 */
class LedControlOverlay : public OverlayBase {
    friend class LedControlOverlayTestAccess;

  public:
    void init_subjects() override;
    const char* xml_component() const override {
        return "led_control_overlay";
    }
    lv_obj_t* create(lv_obj_t* parent) override;
    void register_callbacks() override;

    /**
     * @brief Death signal for the subjects this LedControlOverlay owns.
     *
     * Pass to observe_*() from anything that can outlive this object's
     * deinit_subjects(): that path frees every observer node without bumping
     * the ObserverGuard invalidation epoch, so a guard without the token
     * dereferences a freed observer on its next reset().
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    [[nodiscard]] const char* get_name() const override {
        return "LEDs";
    }

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void cleanup() override;

    /// Freed on close; the next open rebuilds it. Focus and the current look live
    /// in members, the page in subjects, so nothing is lost with the widgets.
    bool destroy_on_close() const override {
        return true;
    }

    /// The device the next activation opens on, when it still exists. Empty
    /// opens on the last focused device.
    void request_focus(const std::string& device_id);

    [[nodiscard]] const std::string& focused_device() const {
        return focused_strip_;
    }

  private:
    /// Releases the tab, swatch and chip pools the tree was bound to.
    void on_ui_destroyed() override;

    /// LV_EVENT_DELETE on the root: a tree deleted by anyone else leaves no
    /// pointer into it, and the next open recreates it.
    static void on_root_deleted(lv_event_t* e);

    void rebuild_tabs();
    void publish_tab_dots();
    void focus_device(const std::string& id);
    void scroll_tab_into_view(int index);
    /// Reads the focused device's brightness, color and white level.
    void load_page_state();
    void publish_page();
    void publish_color_state();
    /// Edges are judged against the current theme's surfaces.
    void publish_swatch_edges();
    void publish_list();
    void on_led_state_changed();
    /// Polls the focused WLED strip and re-reads its page when the poll lands.
    void refresh_wled_page();

    [[nodiscard]] const LedStripInfo* focused_info() const;
    [[nodiscard]] MacroLedType focused_macro_type() const;
    /// The focused strip's running effect among its list chips, or -1.
    [[nodiscard]] int active_effect_index() const;
    [[nodiscard]] std::vector<bool> focused_effects_enabled() const;
    /// Shows @p chip (-1 the None chip) until a frame changes the focused
    /// strip's effect state or PENDING_EFFECT_TIMEOUT_MS passes.
    void set_pending_effect_chip(int chip);
    /// A status frame: a pending chip holds until the effect state moves, then
    /// the tapped chip stays if it is running, else the running one wins.
    void resolve_effect_chip();
    /// The bounded wait for pending chip @p gen ran out: show the real state.
    void end_pending_effect(unsigned gen);

    void handle_tab_clicked(int index);
    void handle_power();
    void handle_brightness(int pct);
    void handle_white(int tone);
    void handle_swatch(int index);
    void handle_custom_color();
    void handle_list_chip(int index);
    void handle_effects_none();
    void handle_macro_on();
    void handle_macro_off();
    void handle_macro_toggle();

    /// A full-brightness @p rgb with no white, at the current brightness (full
    /// when the light is off).
    void apply_swatch_color(uint32_t rgb);
    /// Makes @p rgb and @p w the current look and sends it.
    void apply_look(uint32_t rgb, double w);
    /// Sends current color, W and brightness to the focused NATIVE device,
    /// stopping its effects first.
    void apply_current_color();
    void stop_focused_effects();

    SubjectManager subjects_;

    // Tab row
    lv_subject_t tab_count_{};
    lv_subject_t focused_tab_{};
    lv_subject_t tabs_fade_{};
    helix::xml::IndexedSubjectPool tab_name_pool_{"led_tab_name",
                                                  helix::xml::IndexedSubjectPool::Type::String};
    /// PowerState per tab.
    helix::xml::IndexedSubjectPool tab_dot_pool_{"led_tab_dot",
                                                 helix::xml::IndexedSubjectPool::Type::Int};
    helix::xml::IndexedSubjectPool tab_dot_color_pool_{"led_tab_dot_color",
                                                       helix::xml::IndexedSubjectPool::Type::Color};

    // Page sections: the DevicePage enums as ints
    lv_subject_t page_lamp_{};
    lv_subject_t page_white_{};
    lv_subject_t page_color_vis_{};
    lv_subject_t page_list_{};

    // Page state
    lv_subject_t page_on_{}; ///< PowerState of the focused device
    lv_subject_t page_color_{};
    lv_subject_t page_fill_text_{};
    lv_subject_t page_brightness_{};
    lv_subject_t page_brightness_text_{};
    char page_brightness_text_buf_[16] = {0};
    lv_subject_t page_white_sel_{};
    lv_subject_t swatch_count_{};
    helix::xml::IndexedSubjectPool swatch_color_pool_{"led_swatch_color",
                                                      helix::xml::IndexedSubjectPool::Type::Color};
    /// 1 where a swatch's fill is near-white and needs the light-theme hairline.
    helix::xml::IndexedSubjectPool swatch_edge_pool_{"led_swatch_edge",
                                                     helix::xml::IndexedSubjectPool::Type::Int};
    lv_subject_t selected_swatch_{};
    lv_subject_t page_list_title_{};
    char page_list_title_buf_[64] = {0};
    lv_subject_t chip_count_{};
    helix::xml::IndexedSubjectPool chip_label_pool_{"led_chip_label",
                                                    helix::xml::IndexedSubjectPool::Type::String};
    lv_subject_t active_chip_{}; ///< chip index, -1 the None chip, -2 nothing
    lv_subject_t page_level_{};  ///< the LEVEL_CHIPS value shown, 0 none
    lv_subject_t page_note_{};
    char page_note_buf_[128] = {0};

    ObserverGuard state_observer_;
    ObserverGuard theme_observer_;

    std::string focused_strip_;
    std::string last_focused_; ///< for this session only
    std::string requested_focus_;
    std::vector<LedStripInfo> devices_;
    DevicePage page_;
    /// What each list chip sends: effect names, WLED preset ids, or macro gcode.
    std::vector<std::string> list_values_;

    int current_brightness_ = 100;
    uint32_t current_color_ = 0xFFFFFF; ///< full-brightness RGB
    double current_white_ = 0.0;        ///< full-brightness W, 0.0-1.0
    /// Bumped by every control; a poll landing after a bump leaves the page alone.
    unsigned page_gen_ = 0;

    static constexpr uint32_t PENDING_EFFECT_TIMEOUT_MS = 4000;
    std::optional<int> pending_effect_chip_;
    std::vector<bool> pending_effects_snapshot_;
    unsigned pending_effect_gen_ = 0;
};

} // namespace helix::led

inline helix::led::LedControlOverlay& get_led_control_overlay() {
    return helix::lazy_global<helix::led::LedControlOverlay>("LedControlOverlay");
}

namespace helix {
/**
 * @brief Push the LED control overlay, creating it under @p parent_screen when it has no live tree
 *
 * The overlay singleton owns its one widget tree; every caller opens through
 * here and none keeps or deletes the root.
 * @param parent_screen Screen to create the overlay on when it has no live tree
 * @param device_id Device to open on; empty opens on the last focused device,
 *        then the chamber light
 * @return The pushed root, or nullptr if it could not be created
 */
lv_obj_t* open_led_control_overlay(lv_obj_t* parent_screen, const std::string& device_id = "");
} // namespace helix
