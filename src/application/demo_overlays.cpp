// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file demo_overlays.cpp
 * @brief Demo overlays and modals with representative sample data, for the
 *        remote-control `demo` command.
 */

#include "demo_overlays.h"

#include "ui_ams_loading_error_modal.h"
#include "ui_dialog.h"
#include "ui_error_reporting.h"
#include "ui_lock_screen.h"
#include "ui_modal.h"
#include "ui_notification.h"
#include "ui_panel_ams_overview.h"
#include "ui_panel_belt_tension.h"
#include "ui_panel_print_status.h"
#include "ui_preflight_check_modal.h"
#include "ui_print_tune_overlay.h"
#include "ui_runout_guidance_modal.h"
#include "ui_spaghetti_detection_modal.h"

#include "action_prompt_modal.h"
#include "ams_error.h"
#include "ams_types.h"
#include "app_globals.h"
#include "camera_frame.h"
#include "camera_stream.h"
#include "color_utils.h"
#include "data_root_resolver.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/ui_led_control_overlay.h"
#include "preflight_validator.h"

#include <spdlog/spdlog.h>

#include <memory>

#if HELIX_HAS_CAMERA
// Defined in src/ui/panel_widgets/camera_widget.cpp; that directory is not on
// application.cpp's include path, so forward-declare rather than including the
// header (same pattern as ui_settings_hardware.cpp).
namespace helix {
void open_standalone_camera_fullscreen(lv_obj_t* parent_screen);
}
#endif

using namespace helix;

#ifdef HELIX_ENABLE_REMOTE_CONTROL
namespace helix {

// Bring up a demo overlay/modal with representative sample data. These screens
// only appear in response to a real printer event (pre-print check, runout,
// active print) or configured state (lock PIN), so mock-mode navigation can't
// reach them — the remote-control `demo` command uses this to capture them for
// screenshots with the real widget lifecycle. Must run on the UI thread.
bool show_demo_overlay(const std::string& name) {
    lv_obj_t* screen = lv_screen_active();

    if (name == "preflight-check") {
        // Representative pre-print filament check: one matching tool, one color
        // mismatch (advisory), one empty required slot (the blocking case).
        helix::PreflightResult pf;
        helix::ToolCheck ok;
        ok.tool_index = 0;
        ok.intended_material = "PLA";
        ok.intended_color = 0x2E8B57;
        ok.mapped_slot = 0;
        ok.slot_present = true;
        ok.severity = helix::ToolCheck::Severity::Ok;
        helix::ToolCheck color;
        color.tool_index = 1;
        color.intended_material = "PLA";
        color.intended_color = 0xE23B3B;
        color.mapped_slot = 1;
        color.slot_present = true;
        color.color_ok = false;
        color.severity = helix::ToolCheck::Severity::ColorMismatch;
        helix::ToolCheck empty;
        empty.tool_index = 2;
        empty.intended_material = "PETG";
        empty.intended_color = 0xF5A623;
        empty.mapped_slot = -1;
        empty.slot_present = false;
        empty.severity = helix::ToolCheck::Severity::EmptySlot;
        pf.checks = {ok, color, empty};
        auto modal = std::make_unique<helix::ui::PreflightCheckModal>();
        modal->set_checks(pf);
        Modal::show_owned(std::move(modal), screen);
        return true;
    }

    if (name == "color-mismatch") {
        // The SECOND gate on a Print tap, after the pre-flight empty-slot block:
        // the print-start pipeline warns when a tool resolves to no slot at all.
        // Only reachable from a real multi-tool file whose tools do not map, so
        // mock navigation cannot get here. Text mirrors the unresolved_tools
        // gate's dialog exactly — two unresolved tools, color name plus
        // material per row.
        std::string message = lv_tr("These tools have no matching filament loaded:");
        message += "\n\n";
        message += std::string("  ") + LV_SYMBOL_BULLET +
                   " T2: " + helix::describe_color(0xF5A623) + " (PETG)\n";
        message += std::string("  ") + LV_SYMBOL_BULLET +
                   " T3: " + helix::describe_color(0x2E8B57) + " (PLA)\n";
        message += "\n";
        message += lv_tr("Load the required filaments or start anyway?");
        static char demo_message[1024];
        snprintf(demo_message, sizeof(demo_message), "%s", message.c_str());
        helix::ui::modal_confirm(lv_tr("Color Mismatch"), demo_message, ModalSeverity::Warning,
                                 lv_tr("Start Anyway"), nullptr);
        return true;
    }

    if (name == "leds") {
        return helix::open_led_control_overlay(screen) != nullptr;
    }

    if (name == "runout-modal") {
        auto modal = std::make_unique<RunoutGuidanceModal>();
        modal->set_autofeed_capable(false);
        modal->set_resume_blocked(false);
        // A runout is a warning, and this token screenshots the runout dialog —
        // state it rather than inheriting whatever ran last, same rule every
        // other show site follows (RunoutGuidanceModal::set_advisory()).
        modal->set_advisory(false);
        Modal::show_owned(std::move(modal), screen);
        return true;
    }

    if (name == "ams-loading-error") {
        // Worst case for the modal chrome budget (prestonbrown/helixscreen#1277):
        // a fault string long enough to drive content_container to its
        // #dialog_content_max cap, with the AFC diagram pinned BELOW it and
        // outside the scroll area. That combination overruns the 85% card cap on
        // a 480x272 panel and the button row falls off the bottom. Unreachable in
        // mock mode — AmsBackendMock never produces a recognised AFC fault — so
        // this is the only way to check the real layout instead of arithmetic on
        // a token table.
        // ams_loading_error_modal.xml is registered lazily by AmsPanel, which has
        // not necessarily run — register it here so the demo works from a cold start.
        // Idempotent: re-registering a component replaces the identical entry.
        lv_xml_register_component_from_file(
            helix::asset_component_uri("ui_xml/ams_loading_error_modal.xml").c_str());

        lv_subject_t* seg = lv_xml_get_subject(nullptr, "afc_fault_segment");
        if (seg != nullptr) {
            lv_subject_set_int(seg, static_cast<int>(PathSegment::HUB));
        }
        auto modal = std::make_unique<helix::ui::AmsLoadingErrorModal>();
        const bool shown =
            modal->show(screen,
                        "Filament did not reach the toolhead sensor after the "
                        "configured load length. The lane may be jammed at the hub, "
                        "the spool may have run out mid-load, or the bowden length "
                        "configured for this lane may not match the physical tube "
                        "run between the hub and the toolhead.",
                        "Check the filament path and try again. If the lane is clear, "
                        "verify the configured bowden length for this lane and confirm "
                        "the hub sensor triggers when filament passes it.",
                        []() {});
        // The stack frees the demo instance when the dialog closes.
        if (shown) {
            lv_obj_t* backdrop = modal->backdrop();
            ModalStack::instance().assume_ownership(backdrop, std::move(modal));
        }
        return true;
    }

    if (name == "action-prompt-worst") {
        // Worst case for action_prompt_modal's chrome budget (#1277). This modal
        // carries MORE pinned chrome than ams_loading_error_modal: the AFC
        // diagram, a row_wrap button container that can spill to a second row,
        // and a footer divider + footer row that are hidden by default. All of
        // it sits below the scroll area, so it is the shape most likely to
        // overrun the 85% card cap. Unreachable in mock mode — it needs a live
        // Klipper `action:prompt_begin` — so this is the only way to measure it.
        lv_subject_t* seg = lv_xml_get_subject(nullptr, "afc_fault_segment");
        if (seg != nullptr) {
            lv_subject_set_int(seg, static_cast<int>(PathSegment::HUB));
        }
        helix::PromptData data;
        data.title = "Filament Runout Detected";
        data.severity = "error";
        data.text_lines = {
            "Lane 1 ran out of filament during the print.",
            "The toolhead has been parked and the print is paused.",
            "Load a new spool into lane 1, then choose how to continue.",
        };
        data.buttons = {
            {"Resume", "RESUME", "primary", "", false, -1},
            {"Retry Load", "AFC_LOAD LANE=1", "secondary", "", false, -1},
            {"Change Lane", "AFC_CHANGE_LANE", "secondary", "", false, -1},
            {"Cancel Print", "CANCEL_PRINT", "error", "", true, -1},
        };
        helix::ui::ActionPromptModal::show_owned_prompt(screen, data);
        return true;
    }

    if (name == "action-prompt-many") {
        // A prompt whose buttons cannot share one row: a preheat macro offering
        // seven material presets. Each label is far wider than a seventh of the
        // card, so this is the case that must fall back to row_wrap instead of
        // being squeezed into equal-width cells. Unreachable in mock mode - it
        // needs a live Klipper `action:prompt_begin` - so this is the only way
        // to check the wrapped layout against a real 480x272 panel.
        helix::PromptData data;
        data.title = "Preheat for Load";
        data.text_lines = {"Preheat filament and choose a material."};
        data.buttons = {
            {"PLA 220/60", "SET_MATERIAL M=PLA", "primary", "", false, -1},
            {"PETG 240/80", "SET_MATERIAL M=PETG", "primary", "", false, -1},
            {"ABS 250/100", "SET_MATERIAL M=ABS", "primary", "", false, -1},
            {"ASA 260/100", "SET_MATERIAL M=ASA", "primary", "", false, -1},
            {"TPU 230/50", "SET_MATERIAL M=TPU", "primary", "", false, -1},
            {"PC 280/110", "SET_MATERIAL M=PC", "primary", "", false, -1},
            {"Nylon 260/80", "SET_MATERIAL M=NYLON", "primary", "", false, -1},
            {"Cancel", "", "error", "", true, -1},
        };
        helix::ui::ActionPromptModal::show_owned_prompt(screen, data);
        return true;
    }

    if (name == "lock-screen") {
        helix::ui::LockScreenOverlay::instance().show();
        return true;
    }

    if (name == "print-status") {
        PrintStatusPanel::push_overlay(screen);
        return true;
    }

    if (name == "spaghetti-detection") {
        // The response modal with the camera still attached the way a real
        // detection does. The mock publishes no reachable webcam, so
        // HELIX_DEMO_SNAPSHOT_URL names a snapshot to fetch instead.
        auto sources = helix::live_camera_sources();
        if (const char* url = std::getenv("HELIX_DEMO_SNAPSHOT_URL"); url && *url) {
            sources.stream_frame = nullptr;
            sources.snapshot = [u = std::string(url)] {
                return helix::SnapshotTarget{u, [](const std::string& j, int w, int h) {
                                                 return helix::CameraStream::decode_snapshot(j, w,
                                                                                             h, {});
                                             }};
            };
        }
        auto modal = std::make_unique<SpaghettiDetectionModal>();
        modal->set_detection("Spaghetti detected (78%)");
        modal->request_camera_frame(sources);
        return Modal::show_owned(std::move(modal), screen);
    }

    if (name == "ams-error-toast") {
        // The widest thing the AMS error renderer ever has to lay out: the
        // longest suggestion any backend produces (96 chars) under a 44-char
        // message. Unreachable in mock mode — AmsBackendMock carries no print
        // gate — so this is the only way to check the two-line toast against a
        // real 480x272 panel instead of arithmetic on a font table.
        helix::ui::notify_ams_error(AmsErrorHelper::print_active(/*is_paused=*/true));
        return true;
    }

    if (name == "print-tune") {
        // show() is the real entry point (create() alone builds a hidden panel
        // that never gets pushed) — it wires api + printer state and pushes.
        get_print_tune_overlay().show(screen, get_moonraker_api(), get_printer_state());
        return true;
    }

    if (name == "belt-tension") {
        // Opens the panel directly, skipping the Advanced row's beta and
        // accelerometer gates, for screenshots and ctl runs. Same
        // lazy-create-plus-show the row click performs.
        auto& panel = get_global_belt_tension_panel();
        if (!panel.get_root()) {
            panel.set_api(get_moonraker_client(), get_moonraker_api());
            if (!panel.create(screen)) {
                spdlog::warn("[demo] failed to create panel_belt_tension");
                return false;
            }
        }
        panel.show();
        return true;
    }

    if (name == "ams") {
        // The filament panel's AMS row no-ops without a configured backend, so
        // reach the dedicated AMS management panel directly (mock provides the
        // backend in --test mode).
        navigate_to_ams_panel();
        return true;
    }

    if (name == "camera") {
#if HELIX_HAS_CAMERA
        // No-ops if no webcam is discovered yet; point at a live Moonraker with a
        // webcam (--moonraker ws://host:7125) for a real feed.
        open_standalone_camera_fullscreen(screen);
        return true;
#else
        spdlog::warn("[demo] camera viewer requested but HELIX_HAS_CAMERA is off");
        return false;
#endif
    }

    // Fallback: show any registered XML component that is a self-contained
    // modal. The cases above exist because they need state wired up first;
    // a plain dialog needs none, so screenshotting one should not require
    // adding an entry here. Unknown names return nullptr and fall through.
    if (Modal::show(name.c_str()) != nullptr) {
        spdlog::debug("[demo] shown as a plain modal component: {}", name);
        return true;
    }

    return false;
}

} // namespace helix
#endif // HELIX_ENABLE_REMOTE_CONTROL
