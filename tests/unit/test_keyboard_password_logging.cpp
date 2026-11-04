// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_keyboard_password_logging.cpp
 * @brief Typed characters must never reach the log ring buffer.
 *
 * tail_ring_buffer() is uploaded verbatim as the debug bundle's log_tail, and
 * the ring retains DEBUG regardless of the user's configured verbosity — see
 * test_log_ring_buffer.cpp, which pins that behaviour deliberately. Anything
 * logged at debug on a keypress therefore leaves the device inside an artifact
 * the user shares in a public channel.
 *
 * WiFi passphrases are typed on this keyboard (ui_xml/wifi_password_modal.xml
 * and hidden_network_modal.xml both route through <text_input password_mode>),
 * so two properties have to hold:
 *
 *   1. No log line carries the character a key emits, in any field.
 *   2. Per-keystroke chatter does not evict the diagnostic window it shares
 *      with every other subsystem — a bundle collected after the user typed a
 *      bug report must still contain the state that explains the bug.
 */

#include "ui_keyboard_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "logging_init.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::logging;

namespace {

/// Reinit logging to Console so the test never touches syslog/journal. The
/// process-global ring sink is rebuilt on every init(), so this also gives each
/// test a clean buffer.
void init_console_logging(spdlog::level::level_enum level = spdlog::level::warn) {
    LogConfig cfg;
    cfg.target = LogTarget::Console;
    cfg.enable_console = false;
    cfg.level = level;
    init(cfg);
}

/// Owns the process-wide KeyboardManager for the duration of one test.
///
/// KeyboardManager::keyboard_ is cleared only by reset(): nothing nulls it when
/// the widget's parent dies, because — unlike a registered textarea — the
/// keyboard itself carries no LV_EVENT_DELETE hook. A test that init()s onto the
/// fixture screen and walks away therefore hands the next test a singleton
/// pointing at freed memory, and reset() then runs lv_anim_delete() on it.
///
/// Declared before any widget the test creates, so it tears down first. Both
/// ends drain the UpdateQueue because reset() deletes via safe_delete_deferred().
struct ScopedKeyboard {
    explicit ScopedKeyboard(lv_obj_t* parent) {
        release();
        KeyboardManager::instance().init(parent);
    }
    ~ScopedKeyboard() {
        release();
    }

    ScopedKeyboard(const ScopedKeyboard&) = delete;
    ScopedKeyboard& operator=(const ScopedKeyboard&) = delete;

    static void release() {
        KeyboardManager::instance().reset();
        helix::ui::UpdateQueue::instance().drain();
    }
};

/// Button id of the key that emits `want`, or UINT32_MAX if the layout has none.
uint32_t find_key(lv_obj_t* kb, char want) {
    for (uint32_t id = 0; id < 64; ++id) {
        const char* text = lv_buttonmatrix_get_button_text(kb, id);
        if (text && text[0] == want && text[1] == '\0') {
            return id;
        }
    }
    return UINT32_MAX;
}

/// Drive a real press/release of `btn_id` through the keyboard's own handlers.
void press(lv_obj_t* kb, uint32_t btn_id) {
    lv_buttonmatrix_set_selected_button(kb, btn_id);
    lv_obj_send_event(kb, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(kb, LV_EVENT_RELEASED, nullptr);
}

/// Count occurrences of `needle` in `hay`.
size_t count_of(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

} // namespace

// ============================================================================
// Property 1 — the typed character never appears in a log line
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Keyboard: typed characters never reach the log ring buffer",
                 "[ui][keyboard][security][log_ring]") {
    ScopedKeyboard keyboard_guard(test_screen());
    lv_obj_t* kb = KeyboardManager::instance().get_instance();
    REQUIRE(kb != nullptr);

    // 'g' is a worst case: it is a letter (so it carries an alternate, which is
    // what gated the old log line) and it is common in passphrases.
    const uint32_t g = find_key(kb, 'g');
    REQUIRE(g != UINT32_MAX);

    init_console_logging();
    press(kb, g);

    const std::string tail = tail_ring_buffer(500);

    // The character must not appear quoted, bare, or anywhere else in the line.
    INFO("ring buffer after one keypress:\n" << tail);
    REQUIRE(tail.find("PRESSED 'g'") == std::string::npos);
    REQUIRE(tail.find("'g'") == std::string::npos);
    REQUIRE(tail.find("pressed_char") == std::string::npos);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Keyboard: a typed passphrase is not reconstructable from the ring buffer",
                 "[ui][keyboard][security][log_ring]") {
    ScopedKeyboard keyboard_guard(test_screen());
    lv_obj_t* kb = KeyboardManager::instance().get_instance();
    REQUIRE(kb != nullptr);

    init_console_logging();

    // Type a realistic passphrase. Digits and symbols live on other layers; the
    // letters alone are what made the original leak damaging.
    const std::string secret = "correcthorsebattery";
    size_t typed = 0;
    for (char c : secret) {
        const uint32_t id = find_key(kb, c);
        if (id != UINT32_MAX) {
            press(kb, id);
            ++typed;
        }
    }
    REQUIRE(typed >= secret.size() - 2); // layout must actually carry these keys

    const std::string tail = tail_ring_buffer(2000);
    INFO("ring buffer after typing " << typed << " characters:\n" << tail.substr(0, 4000));

    for (char c : secret) {
        const std::string quoted = std::string("'") + c + "'";
        INFO("character " << c << " leaked as " << quoted);
        REQUIRE(tail.find(quoted) == std::string::npos);
    }
}

// ============================================================================
// Property 2 — keystrokes do not evict the diagnostic window
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Keyboard: keystrokes do not flood the log ring buffer",
                 "[ui][keyboard][log_ring]") {
    ScopedKeyboard keyboard_guard(test_screen());
    lv_obj_t* kb = KeyboardManager::instance().get_instance();
    REQUIRE(kb != nullptr);

    const uint32_t g = find_key(kb, 'g');
    REQUIRE(g != UINT32_MAX);

    init_console_logging();

    // A marker standing in for the diagnostic context a bundle is collected to
    // capture. Typing a bug report must not push it out of the window.
    spdlog::info("DIAGNOSTIC-MARKER-BEFORE-TYPING");

    for (int i = 0; i < 200; ++i) {
        press(kb, g);
    }

    const std::string tail = tail_ring_buffer(2000);

    // The real defect: 1776 of 2000 lines in bundle 5HLV7EAJ were keyboard
    // chatter, and the entire ring covered 3m50s. Per-keystroke logging must not
    // reach the ring at all.
    const size_t kb_lines = count_of(tail, "[KeyboardManager]");
    INFO("KeyboardManager lines in ring after 200 keypresses: " << kb_lines);
    REQUIRE(kb_lines == 0);

    REQUIRE(tail.find("DIAGNOSTIC-MARKER-BEFORE-TYPING") != std::string::npos);
}

// ============================================================================
// Property 3 — password fields are identifiable at the point of logging
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture, "Keyboard: password context is detected from the live textarea",
                 "[ui][keyboard][security]") {
    ScopedKeyboard keyboard_guard(test_screen());

    lv_obj_t* plain = lv_textarea_create(test_screen());
    lv_obj_t* secret = lv_textarea_create(test_screen());
    lv_textarea_set_password_mode(secret, true);

    KeyboardManager::instance().register_textarea(plain);
    KeyboardManager::instance().register_textarea_ex(secret, true);

    KeyboardManager::instance().show(plain);
    REQUIRE_FALSE(KeyboardManager::instance().is_password_context());

    KeyboardManager::instance().show(secret);
    REQUIRE(KeyboardManager::instance().is_password_context());

    // password_mode can be toggled at runtime by a "show password" control, so
    // the answer must track the widget's current state rather than a value
    // latched at registration.
    lv_textarea_set_password_mode(secret, false);
    REQUIRE_FALSE(KeyboardManager::instance().is_password_context());

    KeyboardManager::instance().show(nullptr);
    REQUIRE_FALSE(KeyboardManager::instance().is_password_context());
}
