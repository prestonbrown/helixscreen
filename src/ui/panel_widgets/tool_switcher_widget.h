// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_context_menu.h"

#include "ams_error.h"
#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "display_numbering.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/tool_switcher_layout.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace helix {

class PrinterState;
class ToolSwitcherTestAccess;
struct ToolInfo;

/// Draws its own measured layout (decide_tool_switcher_layout()), so it takes no
/// part in the TileSizing ladder.
class ToolSwitcherWidget : public PanelWidget {
  public:
    /// The most heads the rail draws. A printer with more gets the face, and
    /// the picker lists every head: the per-head subject pools start at this
    /// size and grow to the head count before any cell binds them.
    static constexpr int kMaxRailHeads = 16;

    explicit ToolSwitcherWidget(PrinterState& printer_state);
    ~ToolSwitcherWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "tool_switcher";
    }
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    /// Whether the content box (the tile less space_md each side) holds one
    /// touch target on both axes: the face every smaller box would fall to.
    bool fits_at(int width_px, int height_px) const override;
    /// Whether the picker this tile raised is open.
    bool has_overlay_open() const override;

  private:
    friend class ToolSwitcherTestAccess;

    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Physical size the widget was last granted, cached for observer paths
    // (the heads listener, on_active_tool_changed()) that fire later and
    // need to re-decide without a size to read.
    int current_width_px_ = 0;
    int current_height_px_ = 0;

    /// The verdict last published, and the label form the segments read in.
    ToolSwitcherLayout layout_{};
    ui::ToolLabelForm label_form_ = ui::ToolLabelForm::Full;
    /// This tile's listener on ui::ToolHeadsModel, 0 while detached.
    int heads_listener_ = 0;
    /// Re-publishes the head readings on a °C/°F switch.
    ObserverGuard temp_unit_observer_;
    ObserverGuard print_state_observer_;
    ObserverGuard spool_latch_observer_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. Without this, queued observer callbacks captured
    // via tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    /// The widget a segment belongs to: a rail cell's tile, or the open picker's owner.
    static ToolSwitcherWidget* from_event(lv_event_t* e);

    void show_tool_picker();
    /// Close the picker when this tile raised it.
    void hide_mount_picker();
    void forget_heads_listener();
    /// Mount head @p head_index (an index into ToolState::physical_heads())
    /// by its own tool. Every segment and picker index is a head index: the
    /// switcher draws physical heads, and the filament panel chooses lanes.
    void handle_tool_selected(int head_index);
    /// A rail segment's tap: mount that head, or raise the sheet when it is
    /// the mounted one.
    void handle_segment_tapped(int head_index);

    /// Null every cached widget pointer without touching observers or the
    /// widget tree. Shared by detach() and the raw-delete hook.
    void forget_tile_widgets();

    void on_hooked_root_deleted() override;

    /// Measure the cached box, decide, and publish everything the XML binds.
    void publish();
    /// The mode and geometry for the cached box, and the label form the
    /// segments or face have room for.
    void publish_layout();
    /// Every head's segment text and colour, the detail rows, the face and
    /// whether the cells hold a second line, from ToolState, the filament
    /// system and the temperatures.
    void publish_heads();
    /// Head @p i's new temperature: its segment's temperature (and second
    /// line in the Code form), its detail line, the face's second line when
    /// it is mounted, and whether the second lines still fit. Labels, colours
    /// and materials keep what publish_heads() last published.
    void publish_head_temp(int i);
    /// Whether every cell shown holds its second line.
    void publish_two_line();
    /// Head @p i's temperature, empty while unknown.
    std::string head_temp_text(int i) const;
    /// Whether head @p i reads hot.
    bool head_hot(int i) const;
    /// Head @p i's detail line, named for @p tool: its name, material and
    /// temperature, in the full form for the @p main line where the content
    /// box has room for it.
    std::string detail_head_text(int i, const ToolInfo& tool, bool main) const;

    /// @p tool's label in @p form, in the connected printer's tool noun.
    static std::string tool_text(const ToolInfo& tool, ui::ToolLabelForm form);
    /// What a wide segment calls @p tool: its configured name, else "Tool 1".
    static std::string name_text(const ToolInfo& tool);
    /// What a narrow segment calls @p tool: the name it answers to in G-code
    /// and in the slicer, configured name or not. The sheet's G-code form, so
    /// the rail and the sheet never name one head two ways.
    static std::string short_text(const ToolInfo& tool);
    void on_active_tool_changed(int tool_index);

    /**
     * @brief Whether a running print blocks a tool change, and why.
     *
     * The same question every other filament surface asks — the filament panel,
     * the AMS sidebar and the AMS context menu all route through
     * helix::ui::print_blocks_filament_op(), the mirror of
     * AmsSubscriptionBackend::refuse_if_printing(). PRINTING always refuses;
     * PAUSED refuses only on a backend whose filament macro homes itself
     * (AD5X IFS). A tool change on a shared-toolhead AMS *is* a filament op, so
     * it gets the same gate rather than a policy of its own.
     *
     * @return AmsErrorHelper::print_active() when blocked, success() otherwise.
     */
    [[nodiscard]] AmsError tool_change_refusal() const;

    /// Publish tool_switcher_change_blocked: tool_change_refusal(), or spools
    /// on the bed. Called from publish() and the print-state and latch
    /// observers, so a recycled instance re-applies it on attach as well as on
    /// state change.
    void refresh_print_gating();

    /// Whether spools lying on the bed refuse every move, telling the user so
    /// when they do. The tile is disabled then too; this catches the tap that
    /// lands first.
    [[nodiscard]] bool refuse_while_spools_on_bed() const;

    /// The action cells the connected backend offers: Park where it can dock
    /// the mounted tool, the feeder where it has one the user can open.
    static ToolSwitcherActions actions_offered();
    /// Publish actions_offered() for the rail's action cells and the picker's
    /// footer.
    void refresh_action_support();
    /// Whether a job holding the machine or spools on the bed refuse an action
    /// that moves it, telling the user so when they do. @p technical is the
    /// log text, @p while_printing the user's copy for the print case.
    [[nodiscard]] bool refuse_machine_action(const char* technical,
                                             const char* while_printing) const;
    /// Dock the mounted tool, refusing while a print holds the machine or
    /// nothing is mounted. Every Park tap lands here.
    void handle_park_requested();
    /// Open the feeder when it is closed and close it when it is open,
    /// refusing while a print holds the machine. Every feeder tap lands here.
    void handle_feeder_toggled();

  public:
    static void tool_segment_cb(lv_event_t* e);
    static void tool_face_cb(lv_event_t* e);
    static void tool_park_cb(lv_event_t* e);
    static void tool_feeder_cb(lv_event_t* e);
};

void register_tool_switcher_widget();

} // namespace helix
