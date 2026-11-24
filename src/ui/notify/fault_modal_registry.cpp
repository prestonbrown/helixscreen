// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fault_modal_registry.h"

#include "ui_modal.h"
#include "ui_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace helix::ui {

namespace {

// Dialogs raised for a printer-side fault. Main-thread only: every mutation
// happens either beside a modal_alert() call or inside an LV_EVENT_DELETE
// handler, both of which LVGL already guarantees run on the main thread.
std::vector<lv_obj_t*> s_fault_modals;

// The dedup key each tracked dialog was raised with; empty keys are not stored.
std::vector<std::pair<lv_obj_t*, std::string>> s_fault_keys;

// Drop the handle whenever the dialog dies by ANY route — the user's OK, a
// backdrop tap, our own sweep, ModalStack::clear() at teardown. Without this the
// vector accumulates dead pointers across a long session, and since a freed
// address can be reused by a later allocation, a stale entry could eventually
// name an unrelated live widget.
void forget_fault_modal(lv_event_t* e) {
    auto* dialog = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto it = std::find(s_fault_modals.begin(), s_fault_modals.end(), dialog);
    if (it != s_fault_modals.end()) {
        s_fault_modals.erase(it);
    }
    s_fault_keys.erase(
        std::remove_if(s_fault_keys.begin(), s_fault_keys.end(),
                       [dialog](const auto& entry) { return entry.first == dialog; }),
        s_fault_keys.end());
}

// The dialog currently restating the printer fault, or nullptr.
lv_obj_t* s_fault_carrier = nullptr;

// True when `dialog` is on the stack and not already animating out.
bool modal_live(lv_obj_t* dialog) {
    auto& stack = ModalStack::instance();
    lv_obj_t* backdrop = stack.backdrop_for(dialog);
    return backdrop && !stack.is_exiting(backdrop);
}

// Hide every tracked fault modal still live; returns how many it hid.
int hide_fault_modals() {
    if (s_fault_modals.empty()) {
        return 0;
    }

    // Modal::hide() can delete the backdrop synchronously when there is no exit
    // animation to run, which fires forget_fault_modal() and mutates
    // s_fault_modals mid-iteration. Walk a detached copy.
    std::vector<lv_obj_t*> pending;
    pending.swap(s_fault_modals);

    int dismissed = 0;
    for (lv_obj_t* dialog : pending) {
        // Untracked = already acknowledged and the widgets are gone. Exiting =
        // acknowledged a frame ago and still animating out. Counting either
        // would report dismissals this sweep did not make.
        if (!modal_live(dialog)) {
            continue;
        }
        // External, not Programmatic: this sweep is not the dialog's caller,
        // so on_dismiss still reports the close to whoever armed it.
        Modal::hide(dialog, ModalCloseReason::External);
        dismissed++;
    }
    return dismissed;
}

// The title is the key up to its first newline.
std::string title_of(const std::string& key) {
    return key.substr(0, key.find('\n'));
}

// A fault whose text keeps changing in ways the key cannot fold must not stack
// without bound: past kMaxFaultModalsPerTitle the oldest one under the title goes.
void retire_oldest_with_title(const std::string& key) {
    const std::string title = title_of(key);
    std::vector<lv_obj_t*> live;
    for (const auto& entry : s_fault_keys) {
        if (title_of(entry.second) == title && modal_live(entry.first)) {
            live.push_back(entry.first);
        }
    }
    // The dialog about to be tracked takes one slot.
    while (live.size() + 1 > static_cast<size_t>(kMaxFaultModalsPerTitle)) {
        Modal::hide(live.front(), ModalCloseReason::External);
        live.erase(live.begin());
    }
}

} // namespace

std::string fault_dedup_key(const char* title, const char* message) {
    const std::string text = std::string(title ? title : "") + "\n" + (message ? message : "");
    std::string key;
    key.reserve(text.size());
    // A name Klipper quotes ('mcu1', 'EBBCan') is what tells two faults apart, so
    // its digits stay; every other number (coordinates, counters, line numbers,
    // clock ticks) collapses.
    bool quoted = false;
    bool in_number = false;
    for (const char ch : text) {
        if (ch == '\'') {
            quoted = !quoted;
        }
        const bool digit = ch >= '0' && ch <= '9';
        if (digit && !quoted) {
            if (!in_number) {
                key += '#';
            }
            in_number = true;
            continue;
        }
        // A '.' inside a number belongs to it.
        if (in_number && ch == '.') {
            continue;
        }
        in_number = false;
        key += ch;
    }
    return key;
}

bool fault_modal_showing(const std::string& key) {
    if (key.empty()) {
        return false;
    }
    return std::any_of(s_fault_keys.begin(), s_fault_keys.end(), [&key](const auto& entry) {
        return entry.second == key && modal_live(entry.first);
    });
}

void track_fault_modal(lv_obj_t* dialog, const std::string& key) {
    if (!dialog) {
        return; // modal_alert() already logged why it failed
    }
    s_fault_modals.push_back(dialog);
    if (!key.empty()) {
        retire_oldest_with_title(key);
        s_fault_keys.emplace_back(dialog, key);
    }
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    lv_obj_add_event_cb(dialog, forget_fault_modal, LV_EVENT_DELETE, nullptr);
}

int dismiss_fault_modals() {
    const int dismissed = hide_fault_modals();
    if (dismissed > 0) {
        spdlog::info("[FaultModal] Printer fault cleared - dismissed {} stale modal(s)", dismissed);
    }
    return dismissed;
}

void set_fault_carrier(lv_obj_t* dialog) {
    // Callers may hold a dialog a backdrop tap already deleted. backdrop_for()
    // only compares pointers, so asking about a dead one is safe.
    if (dialog && !modal_live(dialog)) {
        dialog = nullptr;
    }
    if (dialog == s_fault_carrier) {
        return;
    }
    helix::ui::store_until_deleted(&s_fault_carrier, dialog);
    if (!dialog) {
        return;
    }
    const int dismissed = hide_fault_modals();
    if (dismissed > 0) {
        spdlog::info("[FaultModal] Fault now shown by another dialog - dismissed {} modal(s)",
                     dismissed);
    }
}

bool fault_carrier_showing() {
    return s_fault_carrier && modal_live(s_fault_carrier);
}

bool fault_alert_gets_modal(bool modal, bool fault) {
    return modal && !(fault && fault_carrier_showing());
}

int tracked_fault_modal_count() {
    return static_cast<int>(s_fault_modals.size());
}

} // namespace helix::ui
