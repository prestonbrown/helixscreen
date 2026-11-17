// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "helix_features.h"

#if HELIX_HAS_HOST_TERMINAL

#include "ui_widget_ref.h"

#include "core/observer_guard.h"
#include "core/static_panel_registry.h"
#include "core/subject.h"
#include "lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"
#include "system/pty_session.h"
#include "system/terminal_screen.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

namespace helix::ui {

/// What one character typed into the capture field means to the terminal.
enum class TerminalInputKind {
    Text,      ///< ordinary text, sent as typed (or as its control code under Ctrl)
    Backspace, ///< the field's delete, which is the terminal's backspace
    Enter,
};

/// Classify text the capture field was about to insert: the on-screen keyboard and a
/// USB keyboard both arrive as an insert, a delete as LV_KEY_DEL (0x7F).
TerminalInputKind classify_terminal_insert(std::string_view text);

/// The grid that fits @p width x @p height pixels of @p cell_w x @p cell_h cells,
/// clamped to the terminal's supported range.
struct TerminalGrid {
    int rows = 0;
    int cols = 0;
};
TerminalGrid terminal_grid_for(int width, int height, int cell_w, int cell_h);

/// The newest lines of @p lines, oldest first and joined by '\n', that fit in
/// @p max_bytes. The label that shows them has a fixed buffer.
std::string join_tail_lines(const std::deque<std::string>& lines, std::size_t max_bytes);

/// Open the Host Terminal over @p parent_screen: through the one-time warning the
/// first time, directly after that.
void open_host_terminal(lv_obj_t* parent_screen);

/// A shell on the machine HelixScreen runs on (#1573), drawn through libvterm.
///
/// The shell starts when the overlay activates and is ended, with every process on
/// its terminal, when the overlay leaves the screen for any reason but the
/// screensaver: closing it, navigating elsewhere, a hot reload, shutdown. Behind the
/// screensaver the shell keeps running and its output is parsed but not drawn.
class HostTerminalPanel : public OverlayBase {
  public:
    static constexpr int kMinRows = 4;
    static constexpr int kMinCols = 20;
    static constexpr int kMaxRows = 60;
    static constexpr int kMaxCols = 160;
    static constexpr std::size_t kScrollbackLines = 300;
    /// Room for the largest grid, three UTF-8 bytes a cell on average.
    static constexpr std::size_t kScreenBufBytes = 32 * 1024;
    static constexpr std::size_t kScrollbackBufBytes = 32 * 1024;

    ~HostTerminalPanel() override;

    void init_subjects() override;
    void deinit_subjects();
    void register_callbacks() override;
    const char* xml_component() const override {
        return "host_terminal_panel";
    }
    lv_obj_t* create(lv_obj_t* parent) override;
    bool destroy_on_close() const override {
        return true;
    }
    [[nodiscard]] const char* get_name() const override {
        return "Host Terminal";
    }

    /// The grid the shell is drawn in, 0x0 while no shell runs.
    TerminalGrid grid() const;

    void on_activate() override;
    void on_deactivating(DeactivateReason reason) override;
    void on_ui_destroyed() override;

  private:
    void start_session();
    void end_session(const char* why);
    void drain();
    void flush_input();
    void render();
    /// Fit the grid to the part of the view the keyboard, when @p keyboard_up, leaves.
    void layout_grid(bool keyboard_up);
    TerminalGrid visible_grid(bool keyboard_up) const;
    /// Pixels at the bottom of the view the keyboard covers, 0 when it is down.
    int32_t keyboard_cover(bool keyboard_up) const;
    void apply_grid_geometry();
    void measure_cell();
    void set_status(const std::string& text);
    void focus_input();
    bool keyboard_up() const;
    void reset_capture_field();

    void handle_insert(lv_event_t* e);
    void handle_key(uint32_t key);
    void handle_ready();
    void handle_view_clicked();
    void handle_scrolled();
    void handle_special(terminal::Key key);
    void toggle_ctrl();
    bool take_ctrl();

    std::unique_ptr<terminal::PtySession> session_;
    std::unique_ptr<terminal::TerminalScreen> screen_;
    std::uint64_t rendered_scrollback_ = UINT64_MAX;
    bool dirty_ = false;
    bool follow_output_ = true;
    int cell_w_ = 0;
    int cell_h_ = 0;
    int32_t base_pad_bottom_ = 0;

    WidgetRef view_;
    WidgetRef screen_area_;
    WidgetRef screen_label_;
    WidgetRef cursor_;
    WidgetRef capture_;

    ObserverGuard soft_keyboard_observer_;
    SubjectManager subjects_;
    char status_buf_[192] = {};
    helix::Subject status_subject_{};
    helix::Subject status_visible_subject_{};
    helix::Subject ctrl_subject_{};
    helix::Subject cursor_visible_subject_{};
    helix::Subject has_scrollback_subject_{};
    std::unique_ptr<char[]> screen_buf_;
    std::unique_ptr<char[]> scrollback_buf_;
    helix::Subject screen_subject_{};
    helix::Subject scrollback_subject_{};
};

inline HostTerminalPanel& get_global_host_terminal_panel() {
    return helix::lazy_global<HostTerminalPanel>("HostTerminalPanel");
}

} // namespace helix::ui

#endif // HELIX_HAS_HOST_TERMINAL
