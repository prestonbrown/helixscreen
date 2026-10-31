// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_widget_ref.h"

#include "filament_mapper.h"

#include <cstddef>
#include <functional>
#include <lvgl.h>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @brief Dropdown-style slot picker for filament mapping
 *
 * Shows a scrollable list of available AMS/toolchanger slots anchored
 * to a trigger widget (below or above, mirroring lv_dropdown logic).
 * Tap a slot to select it immediately (no OK/Cancel).
 * Tap the backdrop to dismiss without changing the selection.
 *
 * The picker is the XML component filament_slot_picker.xml bound to the
 * filament_picker_* subjects this class publishes; one row per slot through
 * <repeat count="filament_picker_row_count">. C++ fills the subjects and
 * measures where the card goes. One picker shows at a time: showing one hides
 * any other.
 */
class FilamentSlotPicker {
  public:
    struct Selection {
        int slot_index = -1;
        int backend_index = -1;
        bool is_auto = false;
        /// The row Options::extra_choice added was tapped, not a slot.
        bool is_extra = false;
    };

    using SelectCallback = std::function<void(const Selection&)>;

    /// Slot rows the picker has subjects for. Above any backend's lane count;
    /// slots past it are not listed.
    static constexpr std::size_t kMaxPickerRows = 32;

    FilamentSlotPicker() = default;
    ~FilamentSlotPicker();

    // Non-copyable
    FilamentSlotPicker(const FilamentSlotPicker&) = delete;
    FilamentSlotPicker& operator=(const FilamentSlotPicker&) = delete;

    /**
     * @brief Show the slot picker anchored to a dropdown trigger widget
     * @param parent Screen to attach backdrop to (typically lv_screen_active())
     * @param trigger The dropdown trigger widget to anchor the picker to
     * @param tool_index Tool number (for header title)
     * @param expected_material G-code expected material for mismatch warnings
     * @param slots Available AMS/toolchanger slots
     * @param current Current selection (for highlighting)
     * @param on_select Callback fired immediately when a slot is tapped
     */
    void show(lv_obj_t* parent, lv_obj_t* trigger, int tool_index,
              const std::string& expected_material, const std::vector<helix::AvailableSlot>& slots,
              const Selection& current, SelectCallback on_select);

    /// How the card presents itself beyond the slot rows.
    struct Options {
        /// Heading above the rows; empty shows none.
        std::string title;
        /// Outline the trigger while open. Meant for a swatch trigger, whose
        /// resting border hide() restores; leave it off for a button.
        bool highlight_trigger = true;
        /// Text of one more row after the slots, an answer that names no
        /// slot (Selection::is_extra); empty adds none.
        std::string extra_choice;
        /// Text of a last row that closes the picker like a backdrop tap,
        /// answering nothing; empty adds none.
        std::string cancel_choice;
        /// Row order and a muted reason per slot row: when non-empty, the slot rows are
        /// drawn in this order (indices into the slots shown) with each reason on the
        /// right; empty keeps the slots' own order with no reasons.
        std::vector<std::size_t> order;
        std::vector<std::string> reasons; ///< parallel to order
        /// Offer an Auto row first (Selection::is_auto).
        bool offer_auto = false;
    };

    /// @copydoc show(lv_obj_t*, lv_obj_t*, int, const std::string&, const
    /// std::vector<helix::AvailableSlot>&, const Selection&, SelectCallback)
    /// @param options Heading and trigger highlight
    void show(lv_obj_t* parent, lv_obj_t* trigger, const std::vector<helix::AvailableSlot>& slots,
              const Selection& current, SelectCallback on_select, const Options& options);

    void hide();
    [[nodiscard]] bool is_visible() const {
        return backdrop_ != nullptr;
    }

    /// Row callbacks from filament_slot_picker.xml, routed to the shown picker.
    static void row_cb(lv_event_t* e);
    static void backdrop_cb(lv_event_t* e);

  private:
    void show_impl(lv_obj_t* parent, lv_obj_t* trigger,
                   const std::vector<helix::AvailableSlot>& slots, const Selection& current,
                   SelectCallback on_select, const Options& options,
                   const std::string& expected_material);
    /// Publish every filament_picker_* subject for @p options and the rows shown;
    /// @p reasons is parallel to rows_.
    void publish(const Options& options, const std::vector<std::string>& reasons);
    void on_row(const char* choice);
    void select(const Selection& sel);
    /// Size the card to its widest row, capped at the screen width.
    void fit_card_width(lv_obj_t* card);
    void position_card(lv_obj_t* card);
    void scroll_to_selected(lv_obj_t* card);

    helix::ui::WidgetRef backdrop_;
    lv_obj_t* trigger_ = nullptr;
    bool trigger_highlighted_ = false;
    SelectCallback on_select_;
    std::vector<helix::AvailableSlot> slots_;
    /// Row k shows slots_[rows_[k]].
    std::vector<std::size_t> rows_;
    std::string expected_material_;
    Selection current_selection_;
};

} // namespace helix::ui
