// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

/**
 * @file text_clipboard.h
 * @brief The in-app clipboard behind Cut / Copy / Paste on text fields
 *
 * One string, held in memory and never persisted: a restart empties it. On the
 * printers' framebuffer and DRM displays there is no OS clipboard, so this is an
 * app-internal clipboard only. Where the display is SDL (desktop and Android) the
 * text is mirrored to and read from the OS clipboard as well, so a value can move
 * between the app and other programs.
 *
 * The rules (size cap, what a field accepts, which actions a field offers) are
 * pure functions so they test without LVGL.
 */
namespace helix::clipboard {

/// Most bytes the clipboard holds. Longer text is cut at a character boundary.
/// Every field the app has is a short value (an address, a lot number, a G-code
/// line), so this is generous for them and bounds what an OS paste can push in.
constexpr std::size_t kMaxBytes = 4096;

/**
 * @brief @p text cut to at most @p max_bytes without splitting a UTF-8 character
 *
 * A cut that would land inside a multi-byte sequence backs off to its lead byte,
 * so the result is always as valid as the input.
 */
[[nodiscard]] std::string clip_to_limit(std::string_view text, std::size_t max_bytes = kMaxBytes);

/**
 * @brief @p text made fit to insert into a field
 *
 * A one-line field gets every line break and tab as a single space, since LVGL
 * inserts a whole paste without the per-character filter that keeps line breaks
 * out of a typed one-line field. A multi-line field keeps its line breaks, with
 * CRLF and lone CR folded to LF, and tabs. Every other control character is
 * dropped in both.
 */
[[nodiscard]] std::string sanitize_for_field(std::string_view text, bool one_line);

/// The edit actions a field's menu offers.
struct EditActions {
    bool cut = false;
    bool copy = false;
    bool paste = false;

    [[nodiscard]] bool any() const {
        return cut || copy || paste;
    }
};

/**
 * @brief Which of Cut / Copy / Paste a field offers
 *
 * A password field offers none: its text never reaches the clipboard, and a
 * paste into it would put unseen text behind the mask. Otherwise Cut and Copy
 * need text in the field and Paste needs text on the clipboard.
 */
[[nodiscard]] EditActions actions_for_field(bool password, bool field_has_text,
                                            bool clipboard_has_text);

/// Store @p text (clipped to kMaxBytes), and mirror it to the OS clipboard where there is one.
void set_text(std::string_view text);

/// The clipboard's text: the OS clipboard's where there is one and it holds text, else ours.
[[nodiscard]] std::string get_text();

/// Whether get_text() would return anything.
[[nodiscard]] bool has_text();

/// Empty the in-app clipboard. The OS clipboard is left alone.
void clear();

} // namespace helix::clipboard
