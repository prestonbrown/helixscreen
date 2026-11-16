// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "plr_backend.h"

#include <string>

namespace helix {
class IMoonrakerAPI;
}

namespace helix::ui {

/// Show the one-shot "Resume interrupted print?" modal. Backend-agnostic: it
/// executes the `plan` it is handed rather than knowing any firmware's gcode.
/// Button mapping:
///   - primary "Resume"    -> plan.resume_gcode, or the guided resume wizard
///                            when the plan carries a plan.resume_rpc
///   - secondary "Discard" -> plan.discard_gcode, or plan.discard_rpc_method
///   - backdrop / ESC      -> nothing (recovery data left intact, re-offered on
///                            the next connect)
///
/// The plan is COPIED into module state before the modal is created, so the
/// button handlers never re-derive the backend or re-check the safety invariant
/// against live state that may have moved on between the offer and the tap.
///
/// PRECONDITION: `plan.resume_allowed()`. For the probed backend that means
/// the side-effectful recovery probe ran this connection and returned both
/// states true — the probe is what sets print_stats.power_loss=1, which the
/// stock sensorless-homing macro reads to choose a full Z clearance lift. The
/// Resume handler re-checks and refuses if it is not met. See
/// docs/devel/POWER_LOSS_RECOVERY.md.
///
/// `api` is stored as borrowed user_data on the two buttons — NO heap context
/// is allocated, so a backdrop/ESC dismissal (which fires neither callback)
/// cannot leak. `api` must outlive the modal; the app-global IMoonrakerAPI does.
void show_plr_recovery_prompt(IMoonrakerAPI* api, const helix::PlrRecoveryPlan& plan);

/// The prompt's Resume button on @p plan: opens the plugin's guided resume when
/// the plan resumes through the plugin, else sends the plan's resume gcode.
/// Refuses a plan without an authorized resume.
void run_plr_resume(IMoonrakerAPI* api, const helix::PlrRecoveryPlan& plan);

/// The prompt's Discard button on @p plan: its discard gcode, or its discard
/// RPC (the plugin's or the firmware's).
void run_plr_discard(IMoonrakerAPI* api, const helix::PlrRecoveryPlan& plan);

/// Pure, LVGL-free body-text builder (unit-testable). When `file_path` is
/// non-empty, its display filename (basename, gcode extension stripped) is
/// substituted into `with_file_fmt` (which must contain a single `{}`
/// placeholder). On an empty path, an empty resolved name, OR a format failure
/// (e.g. a mistranslated placeholder), `generic_body` is returned verbatim.
/// Translation is the caller's responsibility — pass lv_tr(...) strings.
std::string plr_prompt_body(const std::string& file_path, const char* with_file_fmt,
                            const char* generic_body);

/// One backend's pair of body templates: the `{}`-bearing form used when the
/// recovery filename is known, and the fallback used when it is not.
struct PlrPromptStrings {
    const char* with_file = nullptr;
    const char* generic = nullptr;
};

/// Pure choice of body copy for `plan` (unit-testable). Callers pass both
/// candidate pairs already wrapped in lv_tr(...) — translation keys must stay
/// lexically adjacent to `lv_tr(` for the extractor to find them, so the literals
/// live at the call site and only the SELECTION lives here.
///
/// A plan whose resumed layer may land off the interrupted one
/// (plan.resume_layer_approximate) gets `approximate`: promising "resume where
/// it left off" there is a claim the first seam disproves. Every other plan
/// gets `standard`.
PlrPromptStrings plr_prompt_strings(const helix::PlrRecoveryPlan& plan,
                                    const PlrPromptStrings& approximate,
                                    const PlrPromptStrings& standard);

} // namespace helix::ui
