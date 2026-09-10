// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_probe_overlay.h"

#include <cstdio>
#include <string>

/// Friend access to the probe overlay's config-edit flow (L065) and to the two
/// lifetime guards it inherits. The edit modal is the only production path into
/// handle_config_save(); seeding the staged field/section/value here reaches the
/// save without building it. The guards are protected on ViewLifecycleBase, and
/// a token taken through this friend is the only way to watch from outside
/// whether deactivation and cleanup still expire them.
struct ProbeOverlayTestAccess {
    static void stage_edit(ProbeOverlay& o, const std::string& section, const std::string& field,
                           const std::string& value) {
        o.probe_section_ = section;
        o.editing_field_key_ = field;
        snprintf(o.probe_config_edit_value_buf_, sizeof(o.probe_config_edit_value_buf_), "%s",
                 value.c_str());
    }
    static void save(ProbeOverlay& o) {
        o.handle_config_save();
    }

    /// Stage a PROBE_ACCURACY run without the modal that normally starts one:
    /// the handler name a backstop belongs to, plus the PROBING state the modal
    /// binds to. handle_probe_accuracy() needs the XML modal stack, so this is
    /// the only way in from a plain LVGL fixture.
    static void stage_accuracy_run(ProbeOverlay& o, const std::string& handler_name) {
        o.probe_acc_handler_name_ = handler_name;
        lv_subject_set_int(&o.probe_acc_state_, 1);
    }
    static lv_timer_t* accuracy_backstop_timer(ProbeOverlay& o) {
        return o.probe_acc_backstop_.pending_timer();
    }
    /// 0=idle, 1=probing, 2=results, 3=error.
    static int accuracy_state(ProbeOverlay& o) {
        return lv_subject_get_int(&o.probe_acc_state_);
    }
    static std::string accuracy_error(ProbeOverlay& o) {
        const char* msg = lv_subject_get_string(&o.probe_acc_error_msg_);
        return msg ? msg : "";
    }

    /// Screen-scoped guard: dropped on every deactivation.
    static helix::LifetimeToken screen_token(ProbeOverlay& o) {
        return o.lifetime_.token();
    }
    /// Object-scoped guard: survives deactivation, dropped at cleanup().
    static helix::LifetimeToken object_token(ProbeOverlay& o) {
        return o.object_lifetime_.token();
    }
};
