// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

class PanelWidget;

using WidgetFactory = std::function<std::unique_ptr<PanelWidget>(const std::string& instance_id)>;
using SubjectInitFn = std::function<void()>;

struct PanelWidgetDef {
    const char* id;                    // Stable string for JSON config
    const char* display_name;          // For settings overlay UI
    const char* icon;                  // Icon name
    const char* description;           // Short description for settings overlay
    const char* hardware_gate_subject; // nullptr = always available
    const char* hardware_gate_hint; // Human-readable reason, e.g., "Requires AMS or MMU hardware"
    bool default_enabled = true;    // Whether enabled in fresh/default config
    int colspan = 1;                // Default grid columns spanned
    int rowspan = 1;                // Default grid rows spanned
    int min_colspan = 0;            // Minimum columns (0 = use colspan)
    int min_rowspan = 0;            // Minimum rows (0 = use rowspan)
    int max_colspan = 0;            // Maximum columns (0 = use colspan, i.e. not scalable)
    int max_rowspan = 0;            // Maximum rows (0 = use rowspan, i.e. not scalable)
    bool multi_instance = false;    // Allows dynamic instance creation with base_id:N IDs

    /// True when this widget may occupy half a cell on that axis (#1126).
    ///
    /// The home grid lays out half-cell tracks, so an authored colspan of 1 is
    /// GridLayout::TRACKS_PER_CELL tracks wide. A widget with the flag set may
    /// also be placed and sized at odd track counts; edit mode snaps everything
    /// else to even boundaries so a whole-cell widget can never straddle two.
    bool supports_half_col = false;
    bool supports_half_row = false;

    WidgetFactory factory = nullptr;       // nullptr = pure XML or externally managed
    SubjectInitFn init_subjects = nullptr; // Called once before XML creation

    // Resolved accessors (0 = "use default colspan/rowspan")
    int effective_min_colspan() const {
        return min_colspan > 0 ? min_colspan : colspan;
    }
    int effective_min_rowspan() const {
        return min_rowspan > 0 ? min_rowspan : rowspan;
    }
    int effective_max_colspan() const {
        return max_colspan > 0 ? max_colspan : colspan;
    }
    int effective_max_rowspan() const {
        return max_rowspan > 0 ? max_rowspan : rowspan;
    }
    bool is_scalable() const {
        return effective_max_colspan() > effective_min_colspan() ||
               effective_max_rowspan() > effective_min_rowspan();
    }
};

const std::vector<PanelWidgetDef>& get_all_widget_defs();
const PanelWidgetDef* find_widget_def(std::string_view id);
size_t widget_def_count();
void register_widget_factory(std::string_view id, WidgetFactory factory);
void register_widget_subjects(std::string_view id, SubjectInitFn init_fn);
// Internal — called once from PanelWidgetManager::init_widget_subjects().
// Do not call directly; widget factories require runtime context (singletons, shared resources).
void init_widget_registrations();

} // namespace helix
