// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_macros.h"

#include "config.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

/// Private model access for MacrosPanel tests, without prod-header test methods (L088).
struct MacrosPanelTestAccess {
    static void prepare(MacrosPanel& p, std::vector<std::string> macros) {
        // HelixTestFixture::reset_all() does NOT clear Config data, and the
        // per-printer "macros/hidden" key is a process-singleton — reset it so
        // each case starts key-absent (first-run seed) regardless of order.
        helix::Config::get_instance()->reset_to_defaults();
        // The test build's get_moonraker_api() stub always returns nullptr
        // (ui_test_utils.cpp), so refresh_macros() no-ops and keeps the
        // injected list — no live API can clobber it.
        p.ensure_subjects();
        p.ui_alive_ = true;
        p.edit_mode_ = false;
        p.pending_hidden_.clear();
        p.all_macros_ = std::move(macros);
        std::sort(p.all_macros_.begin(), p.all_macros_.end());
    }
    static void teardown(MacrosPanel& p) {
        p.edit_mode_ = false;
        p.pending_hidden_.clear();
        p.all_macros_.clear();
        p.displayed_.clear();
        p.ui_alive_ = false;
    }
    static void enter(MacrosPanel& p) {
        p.enter_edit_mode();
    }
    static void exit(MacrosPanel& p, bool save) {
        p.exit_edit_mode(save);
    }
    static void toggle(MacrosPanel& p, size_t i) {
        p.toggle_row(i);
    }
    static const std::vector<std::string>& displayed(MacrosPanel& p) {
        return p.displayed_;
    }
    static const std::set<std::string>& pending_hidden(MacrosPanel& p) {
        return p.pending_hidden_;
    }
    static bool edit_mode(MacrosPanel& p) {
        return p.edit_mode_;
    }
    static int visible_int(MacrosPanel& p, size_t i) {
        return p.row_values(i).visible;
    }
    static int defaults_hidden_int(MacrosPanel& p, size_t i) {
        return p.row_values(i).defaults_hidden;
    }
    static void set_macros(MacrosPanel& p, std::vector<std::string> macros) {
        p.all_macros_ = std::move(macros);
    }
    static void rebuild(MacrosPanel& p) {
        p.rebuild_rows();
    }
    static int row_count(MacrosPanel& p) {
        return lv_subject_get_int(&p.macro_row_count_);
    }
    static int edit_mode_subject(MacrosPanel& p) {
        return lv_subject_get_int(&p.macro_edit_mode_);
    }
    static int save_hidden_subject(MacrosPanel& p) {
        return lv_subject_get_int(&p.macros_edit_save_hidden_);
    }
    static size_t item_in_slot(MacrosPanel& p, size_t slot) {
        return p.item_in_slot(slot);
    }
    /// Hand the panel a discovered list without an API.
    static void seed(MacrosPanel& p, std::vector<std::string> macros) {
        std::sort(macros.begin(), macros.end());
        p.all_macros_ = std::move(macros);
    }
};
