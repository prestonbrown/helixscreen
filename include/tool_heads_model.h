// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h"

#include "display_numbering.h"
#include "helix/xml/indexed_subject_pool.h"
#include "subject_managed_panel.h"
#include "tool_state.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace helix {
class PrinterState;
}

namespace helix::ui {

/// What a head is loaded with, as far as the filament system knows.
struct HeadFilament {
    bool has_color = false;
    uint32_t color_rgb = 0;
    std::string colors; ///< SlotInfo::multi_color_hexes
    std::string finish; ///< the finish by name, for the band
    std::string material;
};

/**
 * @brief The per-head data every tool chooser draws, published whatever
 *        surface is on screen.
 *
 * One `tool_switcher_segment` (ui_xml/components/tool_switcher_segment.xml)
 * draws a head in the Home tool switcher, its picker, the Filament panel's tool
 * row and the temperature graph's tool row. This model publishes what they
 * share, per physical head (ToolState::physical_heads()), as the
 * `tool_switcher_seg_<i>_*` pools:
 *  - the filament band: `_color`, `_has_color`, `_colors`, `_finish`,
 *    `_material`, `_band_text`;
 *  - `_temp`, the head's temperature;
 *  - `_state`, the cell look with the mounted head highlighted, and
 *    `_target_state`, the same with the target tool's head highlighted
 *    (ToolSwitcherCellState);
 *  - `_row` and `_row_graph`, the labels the Filament panel's and the
 *    temperature graph's target rows show, in the display numbering so they
 *    agree with "Nozzle 2" beside them (fit_row_labels());
 *  - `_label`/`_sub` and `_pick`/`_pick_sub`/`_pick_state`, which the Home
 *    widget and the picker fill for their own cells.
 * and `tool_switcher_heads`, the head count. `tool_row_cell_w`/`_h`,
 * `tool_row_band_h` and `tool_row_off` (constantly 0) size a row's cells.
 *
 * Main thread only.
 */
class ToolHeadsModel {
  public:
    /// The heads the pools hold before any XML binds them; they grow past it
    /// before a count that needs more is published.
    static constexpr int kPoolSize = 16;

    static ToolHeadsModel& instance();

    void init_subjects();
    void deinit_subjects();

    /// Follow ToolState, the filament system, the target tool and
    /// @p printer_state's temperatures. Resubscribes every call, so a surface
    /// attaches the PrinterState it reads.
    void attach(PrinterState& printer_state);

    /// Republish every head.
    void publish();

    [[nodiscard]] int head_count() const;
    /// Head @p i as the tool it stands for, numbered as the head.
    [[nodiscard]] ToolInfo head_tool(int i) const;
    /// The mounted head; -1 with the carriage empty, head_count() where the
    /// active tool is one the heads do not hold.
    [[nodiscard]] int mounted_head() const;
    [[nodiscard]] const HeadFilament& filament(int i) const;
    /// Head @p i's temperature, empty while unknown.
    [[nodiscard]] std::string head_temp_text(int i) const;
    [[nodiscard]] bool head_hot(int i) const;
    /// Whether a changer whose heads dock reports a fault.
    [[nodiscard]] static bool changer_fault();

    /// Head @p i's two lines in a picker cell in @p form.
    [[nodiscard]] std::pair<std::string, std::string> picker_lines(int i, ToolLabelForm form) const;

    /// Head @p i as a target surface names it: "Tool 2" (or a configured
    /// name) with @p names, else "2", in the display numbering that "Nozzle 2"
    /// uses.
    [[nodiscard]] std::string row_label(int i, bool names) const;
    /// Head @p i's two lines in a Target-mode picker cell: row_label() over
    /// its temperature.
    [[nodiscard]] std::pair<std::string, std::string> target_lines(int i, bool names) const;

    /// Label every head in the row pool @p key ("_row", "_row_graph") for
    /// cells @p cell_w wide: "Tool 2" (or a configured name) where every
    /// head's fits, else "2". A row calls it as it resizes.
    void fit_row_labels(const std::string& key, int cell_w);

    /// The pools a surface writes for its own cells.
    xml::IndexedSubjectPool& label_pool();
    xml::IndexedSubjectPool& sub_pool();
    xml::IndexedSubjectPool& pick_pool();
    xml::IndexedSubjectPool& pick_sub_pool();
    xml::IndexedSubjectPool& pick_state_pool();
    xml::IndexedSubjectPool& state_pool();
    xml::IndexedSubjectPool& target_state_pool();

    /// Called after a republish of the heads, and with the head index after
    /// one head's temperature changed. Returns an id for remove_listener().
    int add_listener(std::function<void()> on_heads, std::function<void(int)> on_temp);
    void remove_listener(int id);

  private:
    ToolHeadsModel() = default;

    struct HeadTemp {
        // Declared before the observer, so the observer goes first.
        SubjectLifetime lifetime;
        ObserverGuard observer;
        int deci = 0;
        bool known = false;
    };
    struct Listener {
        int id;
        std::function<void()> on_heads;
        std::function<void(int)> on_temp;
    };

    void rebuild_head_temps();
    void publish_head_temp(int i);
    void publish_states();
    void notify_heads();
    void fit_row_pool(size_t r);
    static void on_row_resized(lv_event_t* e);

    PrinterState* printer_state_ = nullptr;
    SubjectLifetime printer_lifetime_;
    bool subjects_initialized_ = false;
    /// Each row pool's last cell width.
    int row_cell_w_[2] = {0, 0};

    SubjectManager subjects_;
    lv_subject_t heads_subject_{};
    lv_subject_t row_band_h_subject_{};
    lv_subject_t row_off_subject_{};
    lv_subject_t row_cell_w_subject_{};
    lv_subject_t row_cell_h_subject_{};

    std::vector<HeadTemp> head_temps_;
    std::vector<HeadFilament> filament_;
    std::vector<Listener> listeners_;
    int next_listener_ = 1;

    ObserverGuard active_tool_observer_;
    ObserverGuard tools_observer_;
    ObserverGuard slots_observer_;
    ObserverGuard extruders_observer_;
    ObserverGuard ams_action_observer_;
    ObserverGuard target_observer_;
    ObserverGuard numbering_observer_;
};

} // namespace helix::ui
