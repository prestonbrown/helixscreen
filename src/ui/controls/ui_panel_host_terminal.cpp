// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_host_terminal.h"

#if HELIX_HAS_HOST_TERMINAL

#include "ui_callback_helpers.h"
#include "ui_keyboard_manager.h"
#include "ui_modal.h"

#include "lvgl.h"
#include "settings_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

namespace helix::ui {

using helix::ui::find_required;

namespace {

/// The shell's terminal type. libvterm emulates an xterm.
constexpr const char* kTermEnv = "TERM=xterm";
/// What the capture field holds between keystrokes. A field with nothing in it
/// has nothing for backspace to delete, and LVGL then reports no insert at all.
constexpr const char* kCaptureSentinel = " ";

/// Cut @p s to at most @p max bytes without splitting a UTF-8 sequence.
void truncate_utf8(std::string& s, std::size_t max) {
    if (s.size() <= max) {
        return;
    }
    std::size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
        --end;
    }
    s.erase(end);
}

std::string current_user_name() {
    if (const struct passwd* pw = ::getpwuid(::geteuid())) {
        if (pw->pw_name && pw->pw_name[0] != '\0') {
            return pw->pw_name;
        }
    }
    return std::to_string(::geteuid());
}

} // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

TerminalInputKind classify_terminal_insert(std::string_view text) {
    if (text.size() == 1 && text[0] == static_cast<char>(LV_KEY_DEL)) {
        return TerminalInputKind::Backspace;
    }
    if (text == "\n" || text == "\r") {
        return TerminalInputKind::Enter;
    }
    return TerminalInputKind::Text;
}

TerminalGrid terminal_grid_for(int width, int height, int cell_w, int cell_h) {
    TerminalGrid grid;
    if (cell_w <= 0 || cell_h <= 0) {
        grid.rows = HostTerminalPanel::kMinRows;
        grid.cols = HostTerminalPanel::kMinCols;
        return grid;
    }
    grid.rows =
        std::clamp(height / cell_h, HostTerminalPanel::kMinRows, HostTerminalPanel::kMaxRows);
    grid.cols =
        std::clamp(width / cell_w, HostTerminalPanel::kMinCols, HostTerminalPanel::kMaxCols);
    return grid;
}

std::string join_tail_lines(const std::deque<std::string>& lines, std::size_t max_bytes) {
    std::size_t total = 0;
    auto first = lines.end();
    while (first != lines.begin()) {
        const auto prev = std::prev(first);
        const std::size_t cost = prev->size() + (first == lines.end() ? 0 : 1);
        if (total + cost > max_bytes) {
            break;
        }
        total += cost;
        first = prev;
    }
    std::string out;
    out.reserve(total);
    for (auto it = first; it != lines.end(); ++it) {
        if (it != first) {
            out.push_back('\n');
        }
        out += *it;
    }
    return out;
}

void open_host_terminal(lv_obj_t* parent_screen) {
    auto& settings = helix::SettingsManager::instance();
    if (settings.get_host_terminal_warning_seen()) {
        get_global_host_terminal_panel().show(parent_screen);
        return;
    }
    const std::string message = fmt::format(
        fmt::runtime(lv_tr("This is a shell on the machine running this screen, as the user "
                           "'{}'. Commands typed here run with that user's rights and can change "
                           "or break the system, including the printer's firmware.")),
        current_user_name());
    modal_confirm(lv_tr("Host Terminal"), message.c_str(), ModalSeverity::Warning,
                  lv_tr("Open Terminal"), [parent_screen] {
                      helix::SettingsManager::instance().set_host_terminal_warning_seen(true);
                      get_global_host_terminal_panel().show(parent_screen);
                  });
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

HostTerminalPanel::~HostTerminalPanel() {
    end_session("destroyed");
    deinit_subjects();
}

void HostTerminalPanel::init_subjects() {
    screen_buf_ = std::make_unique<char[]>(kScreenBufBytes);
    scrollback_buf_ = std::make_unique<char[]>(kScrollbackBufBytes);
    UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, "", "host_terminal_status", subjects_);
    UI_MANAGED_SUBJECT_INT(status_visible_subject_, 0, "host_terminal_status_visible", subjects_);
    UI_MANAGED_SUBJECT_INT(ctrl_subject_, 0, "host_terminal_ctrl", subjects_);
    UI_MANAGED_SUBJECT_INT(cursor_visible_subject_, 0, "host_terminal_cursor_visible", subjects_);
    UI_MANAGED_SUBJECT_INT(has_scrollback_subject_, 0, "host_terminal_has_scrollback", subjects_);
    UI_MANAGED_SUBJECT_STRING_N(screen_subject_, screen_buf_.get(), kScreenBufBytes, "",
                                "host_terminal_screen", subjects_);
    UI_MANAGED_SUBJECT_STRING_N(scrollback_subject_, scrollback_buf_.get(), kScrollbackBufBytes, "",
                                "host_terminal_scrollback", subjects_);
}

void HostTerminalPanel::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

void HostTerminalPanel::register_callbacks() {
    register_xml_callbacks({
        {"on_host_terminal_view_clicked",
         [](lv_event_t*) { get_global_host_terminal_panel().handle_view_clicked(); }},
        {"on_host_terminal_view_resized",
         [](lv_event_t*) {
             get_global_host_terminal_panel().layout_grid(KeyboardManager::instance().is_visible());
         }},
        {"on_host_terminal_view_scrolled",
         [](lv_event_t*) { get_global_host_terminal_panel().handle_scrolled(); }},
        {"on_host_terminal_insert",
         [](lv_event_t* e) { get_global_host_terminal_panel().handle_insert(e); }},
        {"on_host_terminal_key",
         [](lv_event_t* e) { get_global_host_terminal_panel().handle_key(lv_event_get_key(e)); }},
        {"on_host_terminal_focus_changed",
         [](lv_event_t* e) {
             get_global_host_terminal_panel().layout_grid(lv_event_get_code(e) == LV_EVENT_FOCUSED);
         }},
        {"on_host_terminal_ready",
         [](lv_event_t*) {
             get_global_host_terminal_panel().handle_special(terminal::Key::Enter);
         }},
        {"on_host_terminal_ctrl",
         [](lv_event_t*) { get_global_host_terminal_panel().toggle_ctrl(); }},
        {"on_host_terminal_esc",
         [](lv_event_t*) {
             get_global_host_terminal_panel().handle_special(terminal::Key::Escape);
         }},
        {"on_host_terminal_tab",
         [](lv_event_t*) { get_global_host_terminal_panel().handle_special(terminal::Key::Tab); }},
        {"on_host_terminal_left",
         [](lv_event_t*) { get_global_host_terminal_panel().handle_special(terminal::Key::Left); }},
        {"on_host_terminal_up",
         [](lv_event_t*) { get_global_host_terminal_panel().handle_special(terminal::Key::Up); }},
        {"on_host_terminal_down",
         [](lv_event_t*) { get_global_host_terminal_panel().handle_special(terminal::Key::Down); }},
        {"on_host_terminal_right",
         [](lv_event_t*) {
             get_global_host_terminal_panel().handle_special(terminal::Key::Right);
         }},
    });
}

lv_obj_t* HostTerminalPanel::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }
    lv_obj_t* content = find_required(overlay_root_, "overlay_content", get_name());
    view_ = find_required(content, "terminal_view", get_name());
    if (!view_) {
        return nullptr;
    }
    screen_area_ = find_required(view_, "screen_area", get_name());
    screen_label_ = find_required(screen_area_, "screen_text", get_name());
    cursor_ = find_required(screen_area_, "cursor", get_name());
    capture_ =
        find_required(find_required(content, "key_row", get_name()), "key_capture", get_name());
    if (!screen_area_ || !screen_label_ || !cursor_ || !capture_) {
        return nullptr;
    }

    measure_cell();
    base_pad_bottom_ = lv_obj_get_style_pad_bottom(view_, LV_PART_MAIN);
    reset_capture_field();
    return overlay_root_;
}

void HostTerminalPanel::on_activate() {
    OverlayBase::on_activate();
    if (take_wake() && session_) {
        render();
        return;
    }
    follow_output_ = true;
    start_session();
}

void HostTerminalPanel::on_deactivating(DeactivateReason reason) {
    // Behind the screensaver the shell keeps running: a command the user started
    // is still theirs when the screen wakes.
    if (reason == DeactivateReason::Suspended) {
        return;
    }
    end_session(deactivate_reason_name(reason));
}

void HostTerminalPanel::on_ui_destroyed() {
    view_ = nullptr;
    screen_area_ = nullptr;
    screen_label_ = nullptr;
    cursor_ = nullptr;
    capture_ = nullptr;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

void HostTerminalPanel::start_session() {
    if (session_) {
        return;
    }
    if (view_) {
        lv_obj_update_layout(view_);
    }
    const TerminalGrid grid = view_ ? visible_grid(KeyboardManager::instance().is_visible())
                                    : TerminalGrid{kMinRows, kMinCols};
    screen_ = std::make_unique<terminal::TerminalScreen>(grid.rows, grid.cols, kScrollbackLines);
    rendered_scrollback_ = UINT64_MAX;
    apply_grid_geometry();
    layout_grid(KeyboardManager::instance().is_visible());

    const std::string shell =
        terminal::choose_shell(std::getenv("SHELL"), [](const std::string& path) {
            return ::access(path.c_str(), X_OK) == 0;
        });
    const std::vector<std::string> argv{shell, "-l"};
    const std::vector<std::string> env{kTermEnv};

    auto session = std::make_unique<terminal::PtySession>();
    std::string error;
    const bool started = session->start(
        argv, env, grid.rows, grid.cols,
        [this, tok = lifetime_.token()] {
            tok.defer("HostTerminalPanel::drain", [this] { drain(); });
        },
        error);
    if (!started) {
        spdlog::error("[HostTerminal] Could not start {}: {}", shell, error);
        set_status(fmt::format(fmt::runtime(lv_tr("The shell could not start: {}")), error));
        screen_.reset();
        render();
        return;
    }
    session_ = std::move(session);
    spdlog::info("[HostTerminal] Session started: {} as {} (pid {}, {}x{})", shell,
                 current_user_name(), session_->pid(), grid.cols, grid.rows);
    set_status("");
    render();
}

void HostTerminalPanel::end_session(const char* why) {
    if (!session_) {
        return;
    }
    const pid_t pid = session_->pid();
    const int exit_code = session_->stop();
    session_.reset();
    lv_subject_set_int(&ctrl_subject_, 0);
    spdlog::info("[HostTerminal] Session ended ({}): pid {}, exit code {}", why, pid, exit_code);
}

void HostTerminalPanel::drain() {
    if (!session_ || !screen_) {
        return;
    }
    const std::string out = session_->take_output();
    if (!out.empty()) {
        screen_->feed(out);
        dirty_ = true;
    }
    flush_input();
    if (session_->hung_up()) {
        screen_->feed(session_->take_output());
        end_session("shell exited");
        set_status(lv_tr("The shell exited. Tap the terminal to start a new one."));
        dirty_ = true;
    }
    if (dirty_ && !suspended()) {
        // The keyboard's own close key hides it without a focus change; the next
        // output is where the grid grows back.
        layout_grid(KeyboardManager::instance().is_visible());
    }
}

void HostTerminalPanel::flush_input() {
    if (!screen_) {
        return;
    }
    const std::string bytes = screen_->take_input();
    if (!bytes.empty() && session_) {
        session_->write(bytes);
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void HostTerminalPanel::render() {
    dirty_ = false;
    if (!subjects_initialized_) {
        return;
    }
    if (!screen_) {
        lv_subject_set_int(&cursor_visible_subject_, 0);
        return;
    }
    if (screen_->scrollback_pushed() != rendered_scrollback_) {
        rendered_scrollback_ = screen_->scrollback_pushed();
        std::string text = join_tail_lines(screen_->scrollback(), kScrollbackBufBytes - 1);
        lv_subject_set_string(&scrollback_subject_, text.c_str());
        lv_subject_set_int(&has_scrollback_subject_, screen_->scrollback().empty() ? 0 : 1);
    }
    std::string text = screen_->screen_text();
    truncate_utf8(text, kScreenBufBytes - 1);
    lv_subject_set_string(&screen_subject_, text.c_str());

    const terminal::TerminalScreen::Cursor cursor = screen_->cursor();
    lv_subject_set_int(&cursor_visible_subject_, (session_ && cursor.visible) ? 1 : 0);
    if (cursor_) {
        // DECLARATIVE_OK: the cursor block sits on a cell measured from the font.
        lv_obj_set_pos(cursor_, cursor.col * cell_w_, cursor.row * cell_h_);
    }
    if (view_ && follow_output_) {
        lv_obj_update_layout(view_);
        lv_obj_scroll_to_y(view_, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void HostTerminalPanel::apply_grid_geometry() {
    if (!screen_area_ || !cursor_ || !screen_) {
        return;
    }
    // DECLARATIVE_OK: the grid's pixel size is the font's cell size times the grid.
    lv_obj_set_size(screen_area_, screen_->cols() * cell_w_, screen_->rows() * cell_h_);
    lv_obj_set_size(cursor_, cell_w_, cell_h_);
}

void HostTerminalPanel::measure_cell() {
    // The monospace face follows the screen's breakpoint, so a resize can change it.
    const lv_font_t* font = lv_obj_get_style_text_font(screen_label_, LV_PART_MAIN);
    cell_w_ = std::max(1, static_cast<int>(lv_font_get_glyph_width(font, 'M', 0)));
    cell_h_ = std::max(1, static_cast<int>(lv_font_get_line_height(font)) +
                              lv_obj_get_style_text_line_space(screen_label_, LV_PART_MAIN));
}

int32_t HostTerminalPanel::keyboard_cover(bool keyboard_up) const {
    lv_obj_t* keyboard = KeyboardManager::instance().get_instance();
    if (!keyboard_up || !keyboard || !view_) {
        return 0;
    }
    lv_area_t view{};
    lv_obj_get_coords(view_, &view);
    // Where the keyboard settles, whether or not its slide-in has finished.
    const int32_t keyboard_top =
        lv_display_get_vertical_resolution(lv_obj_get_display(view_)) - lv_obj_get_height(keyboard);
    return std::max<int32_t>(0, view.y2 + 1 - keyboard_top);
}

TerminalGrid HostTerminalPanel::visible_grid(bool keyboard_up) const {
    const int32_t width = lv_obj_get_width(view_) -
                          2 * lv_obj_get_style_border_width(view_, LV_PART_MAIN) -
                          lv_obj_get_style_pad_left(view_, LV_PART_MAIN) -
                          lv_obj_get_style_pad_right(view_, LV_PART_MAIN);
    const int32_t height = lv_obj_get_height(view_) -
                           2 * lv_obj_get_style_border_width(view_, LV_PART_MAIN) -
                           lv_obj_get_style_pad_top(view_, LV_PART_MAIN) - base_pad_bottom_ -
                           keyboard_cover(keyboard_up);
    return terminal_grid_for(width, height, cell_w_, cell_h_);
}

void HostTerminalPanel::layout_grid(bool keyboard_up) {
    if (!view_ || !screen_) {
        return;
    }
    // The grid sits at the end of the scrolled content, so padding the view by what
    // the keyboard covers keeps the grid's last row above the keyboard.
    // DECLARATIVE_OK: the padding is measured from the keyboard's height.
    lv_obj_set_style_pad_bottom(view_, base_pad_bottom_ + keyboard_cover(keyboard_up),
                                LV_PART_MAIN);
    const int old_cell_w = cell_w_;
    const int old_cell_h = cell_h_;
    measure_cell();
    const bool cell_changed = cell_w_ != old_cell_w || cell_h_ != old_cell_h;
    const TerminalGrid grid = visible_grid(keyboard_up);
    if (cell_changed || grid.rows != screen_->rows() || grid.cols != screen_->cols()) {
        screen_->resize(grid.rows, grid.cols);
        if (session_) {
            session_->resize(grid.rows, grid.cols);
        }
        apply_grid_geometry();
    }
    render();
}

void HostTerminalPanel::set_status(const std::string& text) {
    if (!subjects_initialized_) {
        return;
    }
    lv_subject_set_string(&status_subject_, text.c_str());
    lv_subject_set_int(&status_visible_subject_, text.empty() ? 0 : 1);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void HostTerminalPanel::reset_capture_field() {
    if (!capture_) {
        return;
    }
    // DECLARATIVE_OK: the field's content is an input mechanism, not displayed data.
    if (std::string_view(lv_textarea_get_text(capture_)) != kCaptureSentinel) {
        lv_textarea_set_text(capture_, kCaptureSentinel);
    }
    lv_textarea_set_cursor_pos(capture_, LV_TEXTAREA_CURSOR_LAST);
}

void HostTerminalPanel::focus_input() {
    if (!capture_) {
        return;
    }
    if (lv_group_t* group = lv_obj_get_group(capture_)) {
        lv_group_focus_obj(capture_);
    }
    KeyboardManager::instance().show(capture_);
}

void HostTerminalPanel::handle_view_clicked() {
    if (!session_) {
        follow_output_ = true;
        start_session();
    }
    focus_input();
}

void HostTerminalPanel::handle_scrolled() {
    if (!view_) {
        return;
    }
    follow_output_ = lv_obj_get_scroll_bottom(view_) <= cell_h_;
}

void HostTerminalPanel::handle_insert(lv_event_t* e) {
    auto* field = lv_event_get_target_obj(e);
    const char* text = static_cast<const char*>(lv_event_get_param(e));
    // The field never keeps what is typed: the shell echoes it onto the grid.
    lv_textarea_set_insert_replace(field, "");
    if (!text || !screen_ || !session_) {
        return;
    }
    switch (classify_terminal_insert(text)) {
    case TerminalInputKind::Backspace:
        screen_->press_key(terminal::Key::Backspace, take_ctrl());
        break;
    case TerminalInputKind::Enter:
        screen_->press_key(terminal::Key::Enter, take_ctrl());
        break;
    case TerminalInputKind::Text:
        screen_->type_text(text, take_ctrl());
        break;
    }
    follow_output_ = true;
    flush_input();
}

void HostTerminalPanel::handle_key(uint32_t key) {
    switch (key) {
    case LV_KEY_UP:
        handle_special(terminal::Key::Up);
        break;
    case LV_KEY_DOWN:
        handle_special(terminal::Key::Down);
        break;
    case LV_KEY_LEFT:
        handle_special(terminal::Key::Left);
        break;
    case LV_KEY_RIGHT:
        handle_special(terminal::Key::Right);
        break;
    case LV_KEY_HOME:
        handle_special(terminal::Key::Home);
        break;
    case LV_KEY_END:
        handle_special(terminal::Key::End);
        break;
    case LV_KEY_ESC:
        handle_special(terminal::Key::Escape);
        break;
    case LV_KEY_DEL:
        handle_special(terminal::Key::Delete);
        break;
    default:
        // Printable keys, Backspace and Enter reach the shell as the field's
        // insert and ready events.
        break;
    }
    // The field's own handling of the key ran first and may have moved its
    // cursor off the end, where a later backspace would find nothing to delete.
    reset_capture_field();
}

void HostTerminalPanel::handle_special(terminal::Key key) {
    if (!screen_ || !session_) {
        return;
    }
    screen_->press_key(key, take_ctrl());
    follow_output_ = true;
    flush_input();
}

void HostTerminalPanel::toggle_ctrl() {
    lv_subject_set_int(&ctrl_subject_, lv_subject_get_int(&ctrl_subject_) ? 0 : 1);
}

bool HostTerminalPanel::take_ctrl() {
    if (lv_subject_get_int(&ctrl_subject_) == 0) {
        return false;
    }
    lv_subject_set_int(&ctrl_subject_, 0);
    return true;
}

} // namespace helix::ui

#endif // HELIX_HAS_HOST_TERMINAL
