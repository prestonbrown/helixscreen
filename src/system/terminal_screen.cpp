// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/terminal_screen.h"

#include "helix_features.h"

#if HELIX_HAS_HOST_TERMINAL

#include <algorithm>
#include <utility>
#include <vterm.h>

namespace helix::terminal {

namespace {

/// A cell libvterm reports for the right half of a double-width character.
constexpr std::uint32_t kWideContinuation = static_cast<std::uint32_t>(-1);

/// Append the text of @p count cells to @p out; an empty cell is a space.
void append_cells(std::string& out, const VTermScreenCell* cells, int count) {
    for (int i = 0; i < count; ++i) {
        const VTermScreenCell& cell = cells[i];
        if (cell.chars[0] == kWideContinuation) {
            continue;
        }
        if (cell.chars[0] == 0) {
            out.push_back(' ');
            continue;
        }
        for (int c = 0; c < VTERM_MAX_CHARS_PER_CELL && cell.chars[c] != 0; ++c) {
            TerminalScreen::append_utf8(out, cell.chars[c]);
        }
        if (cell.width > 1) {
            i += cell.width - 1;
        }
    }
}

void trim_trailing_blanks(std::string& s) {
    const auto end = s.find_last_not_of(' ');
    s.erase(end == std::string::npos ? 0 : end + 1);
}

VTermModifier modifier(bool ctrl) {
    return ctrl ? VTERM_MOD_CTRL : VTERM_MOD_NONE;
}

/// Decode one UTF-8 sequence starting at @p i, advancing @p i past it. A malformed
/// byte decodes as U+FFFD and advances by one.
std::uint32_t next_codepoint(std::string_view s, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    int len = 1;
    std::uint32_t cp = b0;
    if (b0 >= 0xF0 && b0 < 0xF8) {
        len = 4;
        cp = b0 & 0x07;
    } else if (b0 >= 0xE0) {
        len = 3;
        cp = b0 & 0x0F;
    } else if (b0 >= 0xC0) {
        len = 2;
        cp = b0 & 0x1F;
    } else if (b0 >= 0x80) {
        ++i;
        return 0xFFFD;
    }
    if (i + static_cast<std::size_t>(len) > s.size()) {
        ++i;
        return 0xFFFD;
    }
    for (int k = 1; k < len; ++k) {
        const auto b = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    i += static_cast<std::size_t>(len);
    return cp;
}

} // namespace

struct TerminalScreen::Callbacks {
    static int sb_pushline(int cols, const VTermScreenCell* cells, void* user) {
        auto* self = static_cast<TerminalScreen*>(user);
        std::string line;
        append_cells(line, cells, cols);
        trim_trailing_blanks(line);
        self->push_scrollback(std::move(line));
        return 1;
    }

    static int movecursor(VTermPos pos, VTermPos /*oldpos*/, int visible, void* user) {
        auto* self = static_cast<TerminalScreen*>(user);
        self->cursor_.row = pos.row;
        self->cursor_.col = pos.col;
        self->cursor_.visible = visible != 0;
        return 1;
    }

    static int settermprop(VTermProp prop, VTermValue* val, void* user) {
        auto* self = static_cast<TerminalScreen*>(user);
        switch (prop) {
        case VTERM_PROP_CURSORVISIBLE:
            self->cursor_.visible = val->boolean != 0;
            return 1;
        case VTERM_PROP_ALTSCREEN:
            self->alt_screen_ = val->boolean != 0;
            return 1;
        default:
            return 0;
        }
    }

    static int bell(void* user) {
        static_cast<TerminalScreen*>(user)->bell_ = true;
        return 1;
    }

    static void output(const char* s, std::size_t len, void* user) {
        static_cast<TerminalScreen*>(user)->pending_input_.append(s, len);
    }

    static const VTermScreenCallbacks& table() {
        static const VTermScreenCallbacks callbacks = [] {
            VTermScreenCallbacks cb{};
            cb.movecursor = &Callbacks::movecursor;
            cb.settermprop = &Callbacks::settermprop;
            cb.bell = &Callbacks::bell;
            cb.sb_pushline = &Callbacks::sb_pushline;
            return cb;
        }();
        return callbacks;
    }
};

TerminalScreen::TerminalScreen(int rows, int cols, std::size_t scrollback_lines)
    : rows_(std::max(rows, 1)), cols_(std::max(cols, 1)), scrollback_cap_(scrollback_lines) {
    vt_ = vterm_new(rows_, cols_);
    vterm_set_utf8(vt_, 1);
    vterm_output_set_callback(vt_, &Callbacks::output, this);
    screen_ = vterm_obtain_screen(vt_);
    vterm_screen_set_callbacks(screen_, &Callbacks::table(), this);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_reset(screen_, 1);
}

TerminalScreen::~TerminalScreen() {
    if (vt_) {
        vterm_free(vt_);
    }
}

void TerminalScreen::resize(int rows, int cols) {
    rows = std::max(rows, 1);
    cols = std::max(cols, 1);
    if (rows == rows_ && cols == cols_) {
        return;
    }
    rows_ = rows;
    cols_ = cols;
    vterm_set_size(vt_, rows_, cols_);
    cursor_.row = std::min(cursor_.row, rows_ - 1);
    cursor_.col = std::min(cursor_.col, cols_ - 1);
}

void TerminalScreen::feed(std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    vterm_input_write(vt_, bytes.data(), bytes.size());
    vterm_screen_flush_damage(screen_);
}

void TerminalScreen::type_text(std::string_view utf8, bool ctrl) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::uint32_t cp = next_codepoint(utf8, i);
        if (cp == '\n' || cp == '\r') {
            vterm_keyboard_key(vt_, VTERM_KEY_ENTER, modifier(ctrl));
        } else if (cp == '\t') {
            vterm_keyboard_key(vt_, VTERM_KEY_TAB, modifier(ctrl));
        } else if (ctrl && cp >= 'A' && cp <= 'Z') {
            // The control code is the same for either case; libvterm encodes only
            // lowercase letters as plain control bytes.
            vterm_keyboard_unichar(vt_, cp - 'A' + 'a', VTERM_MOD_CTRL);
        } else {
            vterm_keyboard_unichar(vt_, cp, modifier(ctrl));
        }
    }
}

void TerminalScreen::press_key(Key key, bool ctrl) {
    VTermKey vk = VTERM_KEY_NONE;
    switch (key) {
    case Key::Enter:
        vk = VTERM_KEY_ENTER;
        break;
    case Key::Tab:
        vk = VTERM_KEY_TAB;
        break;
    case Key::Backspace:
        vk = VTERM_KEY_BACKSPACE;
        break;
    case Key::Escape:
        vk = VTERM_KEY_ESCAPE;
        break;
    case Key::Up:
        vk = VTERM_KEY_UP;
        break;
    case Key::Down:
        vk = VTERM_KEY_DOWN;
        break;
    case Key::Left:
        vk = VTERM_KEY_LEFT;
        break;
    case Key::Right:
        vk = VTERM_KEY_RIGHT;
        break;
    case Key::Home:
        vk = VTERM_KEY_HOME;
        break;
    case Key::End:
        vk = VTERM_KEY_END;
        break;
    case Key::PageUp:
        vk = VTERM_KEY_PAGEUP;
        break;
    case Key::PageDown:
        vk = VTERM_KEY_PAGEDOWN;
        break;
    case Key::Delete:
        vk = VTERM_KEY_DEL;
        break;
    }
    vterm_keyboard_key(vt_, vk, modifier(ctrl));
}

void TerminalScreen::paste(std::string_view utf8) {
    vterm_keyboard_start_paste(vt_);
    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::uint32_t cp = next_codepoint(utf8, i);
        if (cp == '\n') {
            vterm_keyboard_key(vt_, VTERM_KEY_ENTER, VTERM_MOD_NONE);
        } else {
            vterm_keyboard_unichar(vt_, cp, VTERM_MOD_NONE);
        }
    }
    vterm_keyboard_end_paste(vt_);
}

std::string TerminalScreen::take_input() {
    return std::exchange(pending_input_, std::string{});
}

std::string TerminalScreen::row_text(int row) const {
    std::string out;
    if (row < 0 || row >= rows_) {
        return out;
    }
    VTermScreenCell cell{};
    for (int col = 0; col < cols_; ++col) {
        vterm_screen_get_cell(screen_, VTermPos{row, col}, &cell);
        append_cells(out, &cell, 1);
        if (cell.width > 1) {
            ++col;
        }
    }
    trim_trailing_blanks(out);
    return out;
}

std::string TerminalScreen::screen_text() const {
    std::string out;
    std::size_t keep = 0;
    for (int row = 0; row < rows_; ++row) {
        if (row > 0) {
            out.push_back('\n');
        }
        const std::string text = row_text(row);
        out += text;
        if (!text.empty()) {
            keep = out.size();
        }
    }
    out.erase(keep);
    return out;
}

bool TerminalScreen::take_bell() {
    return std::exchange(bell_, false);
}

void TerminalScreen::push_scrollback(std::string line) {
    ++scrollback_pushed_;
    if (scrollback_cap_ == 0) {
        return;
    }
    scrollback_.push_back(std::move(line));
    while (scrollback_.size() > scrollback_cap_) {
        scrollback_.pop_front();
    }
}

void TerminalScreen::append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        append_utf8(out, 0xFFFD);
    }
}

} // namespace helix::terminal

#endif // HELIX_HAS_HOST_TERMINAL
