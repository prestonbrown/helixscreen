// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "text_clipboard.h"

#ifdef HELIX_DISPLAY_SDL
#include <spdlog/spdlog.h>

#include <SDL.h>
#endif

namespace helix::clipboard {

namespace {

std::string& store() {
    static std::string text;
    return text;
}

bool is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

#ifdef HELIX_DISPLAY_SDL
/// The OS clipboard needs SDL's video subsystem; tests and the headless
/// paths never start it, and then the in-app string is the whole clipboard.
bool os_clipboard_available() {
    return SDL_WasInit(SDL_INIT_VIDEO) != 0;
}
#endif

} // namespace

std::string clip_to_limit(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    std::size_t cut = max_bytes;
    while (cut > 0 && is_utf8_continuation(static_cast<unsigned char>(text[cut]))) {
        --cut;
    }
    return std::string(text.substr(0, cut));
}

std::string sanitize_for_field(std::string_view text, bool one_line) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            out.push_back(one_line ? ' ' : '\n');
        } else if (c == '\t') {
            out.push_back(one_line ? ' ' : '\t');
        } else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            continue;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

EditActions actions_for_field(bool password, bool field_has_text, bool clipboard_has_text) {
    EditActions actions;
    if (password) {
        return actions;
    }
    actions.cut = field_has_text;
    actions.copy = field_has_text;
    actions.paste = clipboard_has_text;
    return actions;
}

void set_text(std::string_view text) {
    store() = clip_to_limit(text);
#ifdef HELIX_DISPLAY_SDL
    if (os_clipboard_available() && SDL_SetClipboardText(store().c_str()) != 0) {
        spdlog::debug("[Clipboard] OS clipboard write failed: {}", SDL_GetError());
    }
#endif
}

std::string get_text() {
#ifdef HELIX_DISPLAY_SDL
    if (os_clipboard_available() && SDL_HasClipboardText()) {
        char* os_text = SDL_GetClipboardText();
        std::string text = clip_to_limit(os_text ? os_text : "");
        SDL_free(os_text);
        if (!text.empty()) {
            return text;
        }
    }
#endif
    return store();
}

bool has_text() {
    return !get_text().empty();
}

void clear() {
    store().clear();
}

} // namespace helix::clipboard
