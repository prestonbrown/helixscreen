// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

// NAMESPACE_OK: libvterm declares its C types in the global namespace
struct VTerm;
// NAMESPACE_OK: libvterm declares its C types in the global namespace
struct VTermScreen;

namespace helix::terminal {

/// The keys a shell needs that a text field has no character for.
enum class Key {
    Enter,
    Tab,
    Backspace,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Delete,
};

/// One terminal's screen: the bytes a child writes to its pty go in through feed(),
/// libvterm parses them into a grid of cells, and the grid comes out as text. Keys
/// typed at the terminal go the other way: they are encoded the way an xterm sends
/// them and collected for the caller to write to the pty, together with the replies
/// the terminal owes to queries the child made (cursor position, device attributes).
///
/// No I/O and no LVGL: the caller owns the pty and the drawing. Main thread only, like
/// any LVGL-adjacent state; it holds no lock.
class TerminalScreen {
  public:
    struct Cursor {
        int row = 0;
        int col = 0;
        bool visible = true;
    };

    /// @p scrollback_lines caps the lines kept after they scroll off the top; the
    /// oldest is dropped first.
    TerminalScreen(int rows, int cols, std::size_t scrollback_lines);
    ~TerminalScreen();
    TerminalScreen(const TerminalScreen&) = delete;
    TerminalScreen& operator=(const TerminalScreen&) = delete;

    /// Sizes below 1x1 are raised to 1x1.
    void resize(int rows, int cols);
    int rows() const {
        return rows_;
    }
    int cols() const {
        return cols_;
    }

    /// Parse output from the child.
    void feed(std::string_view bytes);

    /// Encode typed text. With @p ctrl each character is sent as its control code
    /// (Ctrl+C sends 0x03). A '\n' or '\r' is the Enter key.
    void type_text(std::string_view utf8, bool ctrl = false);
    /// Encode one special key.
    void press_key(Key key, bool ctrl = false);
    /// Encode a paste: the text goes through bracketed paste when the child asked for it.
    void paste(std::string_view utf8);

    /// The bytes owed to the child since the last call, emptied by the call.
    std::string take_input();

    /// One row of the visible screen as UTF-8, trailing blanks trimmed.
    std::string row_text(int row) const;
    /// The visible screen, rows joined by '\n', trailing blank rows trimmed.
    std::string screen_text() const;

    /// Lines that scrolled off the top, oldest first, at most the cap.
    const std::deque<std::string>& scrollback() const {
        return scrollback_;
    }
    /// Lines ever pushed to the scrollback, dropped ones included: a caller that
    /// remembers it can tell whether, and by how much, the scrollback moved.
    std::uint64_t scrollback_pushed() const {
        return scrollback_pushed_;
    }

    Cursor cursor() const {
        return cursor_;
    }
    /// The child switched to the alternate screen (vi, less, top).
    bool alt_screen() const {
        return alt_screen_;
    }
    /// The child rang the bell since the last call; reading clears it.
    bool take_bell();

    /// Encode @p cp as UTF-8 into @p out.
    static void append_utf8(std::string& out, std::uint32_t cp);

  private:
    /// libvterm's callbacks, defined beside the implementation.
    struct Callbacks;

    void push_scrollback(std::string line);

    VTerm* vt_ = nullptr;
    VTermScreen* screen_ = nullptr;
    int rows_ = 0;
    int cols_ = 0;
    std::size_t scrollback_cap_ = 0;
    std::deque<std::string> scrollback_;
    std::uint64_t scrollback_pushed_ = 0;
    std::string pending_input_;
    Cursor cursor_{};
    bool alt_screen_ = false;
    bool bell_ = false;
};

} // namespace helix::terminal
