// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_format_utils.h"
#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_print_select.h"
#include "ui_panel_print_status.h"
#include "ui_progress_arc.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "app_constants.h"
#include "app_globals.h"
#include "data_root_resolver.h"
#include "filament_op_dispatch.h"
#include "filament_op_execute.h"
#include "filament_op_router.h"
#include "filament_sensor_manager.h"
#include "format_utils.h"
#include "klipper_extruder_naming.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "panel_widget_size.h"
#include "print_history_manager.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "standard_macros.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "thumbnail_cache.h"
#include "thumbnail_load_context.h"
#include "thumbnail_processor.h"
#include "tool_state.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <string_view>
#include <unordered_set>

namespace {
// The idle hero thumbnail is the same benchy image the print-thumbnail subject
// publishes when a file has no thumbnail of its own, so both come from one
// resolution point. The returned pointer outlives the widget, which
// lv_image_set_src and the idle-thumb subject both require.
const char* benchy_thumb_path() {
    return helix::PrinterPrintState::no_thumbnail_placeholder();
}
} // namespace

namespace helix {
void register_print_status_widget() {
    register_widget_factory(
        "print_status", [](const std::string&) { return std::make_unique<PrintStatusWidget>(); });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "print_card_clicked_cb",
                             PrintStatusWidget::print_card_clicked_cb);
    lv_xml_register_event_cb(nullptr, "library_files_cb", PrintStatusWidget::library_files_cb);
    lv_xml_register_event_cb(nullptr, "library_last_cb", PrintStatusWidget::library_last_cb);
    lv_xml_register_event_cb(nullptr, "library_recent_cb", PrintStatusWidget::library_recent_cb);
    lv_xml_register_event_cb(nullptr, "library_queue_cb", PrintStatusWidget::library_queue_cb);
    lv_xml_register_event_cb(nullptr, "print_status_nozzle_chevron_cb",
                             PrintStatusWidget::print_status_nozzle_chevron_cb);
    lv_xml_register_event_cb(nullptr, "print_status_layout_library_cb",
                             PrintStatusWidget::print_status_layout_library_cb);
    lv_xml_register_event_cb(nullptr, "print_status_layout_detailed_cb",
                             PrintStatusWidget::print_status_layout_detailed_cb);
    lv_xml_register_event_cb(nullptr, "on_print_status_nozzle_temp_clicked",
                             PrintStatusWidget::on_print_status_nozzle_temp_clicked);
    lv_xml_register_event_cb(nullptr, "on_print_status_bed_temp_clicked",
                             PrintStatusWidget::on_print_status_bed_temp_clicked);
    lv_xml_register_event_cb(nullptr, "on_print_status_chamber_temp_clicked",
                             PrintStatusWidget::on_print_status_chamber_temp_clicked);
}
} // namespace helix

using namespace helix;

std::unordered_set<PrintStatusWidget*>& PrintStatusWidget::live_instances() {
    static std::unordered_set<PrintStatusWidget*> instances;
    return instances;
}

void PrintStatusWidget::init_static_subjects() {
    // Register subjects before XML parsing so bind_flag_if_eq / bind_style can find them.
    // Idempotent — guarded by column_mode_subject_initialized_.
    if (column_mode_subject_initialized_)
        return;

    lv_subject_init_int(&column_mode_subject_, 0);
    lv_xml_register_subject(nullptr, "print_status_column_mode", &column_mode_subject_);
    column_mode_subject_initialized_ = true;
    lv_subject_init_int(&width_band_subject_, 1); // 1 = normal, matches the old colspan=2 default
    lv_xml_register_subject(nullptr, "print_status_width_band", &width_band_subject_);
    width_band_subject_initialized_ = true;

    lv_subject_init_int(&title_hidden_subject_, 0);
    lv_xml_register_subject(nullptr, "print_status_title_hidden", &title_hidden_subject_);
    lv_subject_init_int(&files_hidden_subject_, 0);
    lv_xml_register_subject(nullptr, "print_status_files_hidden", &files_hidden_subject_);
    lv_subject_init_int(&last_hidden_subject_, 0);
    lv_xml_register_subject(nullptr, "print_status_last_hidden", &last_hidden_subject_);
    lv_subject_init_int(&recent_hidden_subject_, 0);
    lv_xml_register_subject(nullptr, "print_status_recent_hidden", &recent_hidden_subject_);
    lv_subject_init_int(&queue_hidden_subject_, 1); // queue starts hidden until jobs arrive
    lv_xml_register_subject(nullptr, "print_status_queue_hidden", &queue_hidden_subject_);
    lv_subject_init_int(&actions_hidden_subject_, 0);
    lv_xml_register_subject(nullptr, "print_status_actions_hidden", &actions_hidden_subject_);
    visibility_subjects_initialized_ = true;

    // Detailed-layout subjects
    lv_subject_init_int(&layout_effective_subject_, 0);
    // Observed through layout_effective_subject_for_test() by
    // tests/unit/test_print_status_widget_layout_gate.cpp, the width-gating guard.
    lv_xml_register_subject(nullptr, "print_status_layout_effective",
                            &layout_effective_subject_); // SUBJECT_OK: read by the layout-gate test
    lv_subject_init_int(&show_filament_active_subject_, 0);
    lv_xml_register_subject(nullptr, "print_status_show_filament_active",
                            &show_filament_active_subject_);
    lv_subject_init_int(&multi_tool_subject_, 0);
    lv_xml_register_subject(nullptr, "print_status_multi_tool", &multi_tool_subject_);
    // Initial value 0 = idle_library_full — matches the default ref_value=0
    // on print_card_idle's bind_flag_if_not_eq so it shows by default
    // before any state events fire.
    lv_subject_init_int(&view_subject_, 0);
    lv_xml_register_subject(nullptr, "print_status_view", &view_subject_);
    // Default to benchy; reset_print_card_to_idle replaces with last-print
    // thumbnail when history loads.
    // Resolve the benchy default to its bundle-absolute path before the subject
    // captures it (a raw "A:assets/images/..." literal misses the /assets mount
    // on firmware and lv_image_set_src can't open it).
    snprintf(idle_thumb_path_buf_, sizeof(idle_thumb_path_buf_), "%s", benchy_thumb_path());
    lv_subject_init_string(&idle_thumb_path_subject_, idle_thumb_path_buf_, nullptr,
                           sizeof(idle_thumb_path_buf_), idle_thumb_path_buf_);
    lv_xml_register_subject(nullptr, "print_status_idle_thumb_path", &idle_thumb_path_subject_);
    // Default to tier 2 (8px) — matches the previous hardcoded medium thickness
    // until the arc lays out and C++ publishes the diameter-derived tier.
    lv_subject_init_int(&arc_thickness_tier_subject_, 2);
    lv_xml_register_subject( // SUBJECT_OK: attach_progress_arc() publishes into this by
                             // pointer and helix_progress_arc.xml bind_styles read it
        nullptr, "print_status_arc_thickness_tier", &arc_thickness_tier_subject_);
    detailed_subjects_initialized_ = true;

    StaticSubjectRegistry::instance().register_deinit("PrintStatusWidgetSubjects", []() {
        if (detailed_subjects_initialized_ && lv_is_initialized()) {
            lv_subject_deinit(&layout_effective_subject_);
            lv_subject_deinit(&show_filament_active_subject_);
            lv_subject_deinit(&multi_tool_subject_);
            lv_subject_deinit(&view_subject_);
            lv_subject_deinit(&idle_thumb_path_subject_);
            lv_subject_deinit(&arc_thickness_tier_subject_);
            detailed_subjects_initialized_ = false;
        }
        if (visibility_subjects_initialized_ && lv_is_initialized()) {
            lv_subject_deinit(&title_hidden_subject_);
            lv_subject_deinit(&files_hidden_subject_);
            lv_subject_deinit(&last_hidden_subject_);
            lv_subject_deinit(&recent_hidden_subject_);
            lv_subject_deinit(&queue_hidden_subject_);
            lv_subject_deinit(&actions_hidden_subject_);
            visibility_subjects_initialized_ = false;
        }
        if (width_band_subject_initialized_ && lv_is_initialized()) {
            lv_subject_deinit(&width_band_subject_);
            width_band_subject_initialized_ = false;
        }
        if (column_mode_subject_initialized_ && lv_is_initialized()) {
            lv_subject_deinit(&column_mode_subject_);
            column_mode_subject_initialized_ = false;
        }
    });
}

PrintStatusWidget::PrintStatusWidget() : printer_state_(get_printer_state()) {
    init_static_subjects();

    // Eager DetailedFormatter creation — its subjects (print_status_layer_text,
    // print_status_bed_text, etc.) MUST be registered BEFORE lv_xml_create
    // parses the widget XML. helix-xml's bind_text/bind_flag_if_eq parser
    // permanently skips bindings whose subject doesn't exist at parse time.
    // Constructing the formatter here (before the ctor returns, and well
    // before attach() runs lv_xml_create) ensures the subjects exist when the
    // XML tree is built.
    acquire_formatter();
}

void PrintStatusWidget::acquire_formatter() {
    if (s_formatter_refcount_++ != 0) {
        return;
    }
    // Release the parked predecessor BEFORE building the replacement. Both
    // formatters publish the same thirteen names into helix-xml's process-wide
    // scope, and ~DetailedFormatter withdraws them by name — so constructing
    // first would have the outgoing formatter's teardown delete the scope
    // records that now point at the incoming one's subjects, leaving every
    // bind_text on the Detailed card resolving to nothing for the rest of the
    // session. Reached whenever the last print-status widget leaves the
    // dashboard and one is added back.
    s_formatter_.reset();
    s_formatter_ = std::make_unique<DetailedFormatter>();
}

PrintStatusWidget::~PrintStatusWidget() {
    detach();
    // Pair the eager ctor refcount bump, but leave s_formatter_ standing at
    // refcount==0: destroying it here would dangle the helix-xml scope's subject
    // pointers for any XML still bound to them. It is released in
    // acquire_formatter(), where a replacement is about to take over the names.
    if (s_formatter_refcount_ > 0)
        --s_formatter_refcount_;
}

void PrintStatusWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    using helix::ui::observe_int_immediate;
    using helix::ui::observe_int_sync;
    using helix::ui::observe_print_lifecycle;
    using helix::ui::observe_string_immediate;

    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    live_instances().insert(this);

    // Formatter is already alive (created in the ctor before XML parse).
    // Just re-apply the per-instance nozzle override. If the override
    // pointed at a missing extruder, clear our cached state so we don't
    // re-trigger the fallback every attach.
    if (s_formatter_ && !s_formatter_->set_nozzle_tool_override(nozzle_tool_override_)) {
        nozzle_tool_override_ = "auto";
        config_["nozzle_tool_override"] = "auto";
        save_widget_config(config_);
    }

    // Store this pointer for event callback recovery
    lv_obj_set_user_data(widget_obj_, this);

    // Cache widget references from XML
    print_card_thumb_ = lv_obj_find_by_name(widget_obj_, "print_card_thumb");
    print_card_active_thumb_ = lv_obj_find_by_name(widget_obj_, "print_card_active_thumb");
    print_card_layout_ = lv_obj_find_by_name(widget_obj_, "print_card_layout");
    print_card_thumb_wrap_ = lv_obj_find_by_name(widget_obj_, "print_card_thumb_wrap");
    print_card_info_ = lv_obj_find_by_name(widget_obj_, "print_card_info");
    print_card_printing_ = lv_obj_find_by_name(widget_obj_, "print_card_printing");
    print_card_preparing_info_ = lv_obj_find_by_name(widget_obj_, "print_card_preparing_info");

    // Library idle state widgets
    print_card_idle_ = lv_obj_find_by_name(widget_obj_, "print_card_idle");
    print_card_idle_compact_ = lv_obj_find_by_name(widget_obj_, "print_card_idle_compact");
    print_card_idle_detailed_ = lv_obj_find_by_name(widget_obj_, "print_card_idle_detailed");
    print_card_printing_detailed_ =
        lv_obj_find_by_name(widget_obj_, "print_card_printing_detailed");
    print_card_thumb_compact_ = lv_obj_find_by_name(widget_obj_, "print_card_thumb_compact");
    library_row_last_ = lv_obj_find_by_name(widget_obj_, "library_row_last");
    compact_row_last_ = lv_obj_find_by_name(widget_obj_, "compact_row_last");

    // Hand the detailed-layout arc widget to the formatter (may be nullptr if not in DOM yet)
    if (s_formatter_) {
        lv_obj_t* arc = lv_obj_find_by_name(widget_obj_, "detailed_progress_arc");
        if (arc)
            s_formatter_->attach_arc(arc);
    }

    // Nozzle reads the tool-pin-aware proxy subjects rather than the raw
    // active-extruder ones, so the icon tracks whichever tool the card is
    // showing. Bed and chamber use the PrinterState defaults. The proxy
    // subjects live on s_formatter_, which is only ever replaced from
    // acquire_formatter() — i.e. while no widget is attached — so they genuinely
    // outlive this binder.
    nozzle_icon_binder_.bind_subjects(widget_obj_, "nozzle_icon_glyph",
                                      lv_xml_get_subject(nullptr, "print_status_nozzle_current"),
                                      lv_xml_get_subject(nullptr, "print_status_nozzle_target"));
    bed_icon_binder_.bind(widget_obj_, printer_state_, helix::HeaterType::Bed);
    chamber_icon_binder_.bind(widget_obj_, printer_state_, helix::HeaterType::Chamber);

    // Set up observers (after widget references are cached and widget_obj_ is set)
    print_state_observer_ = observe_print_lifecycle<PrintStatusWidget>(
        printer_state_.get_print_lifecycle_subject(), this,
        [](PrintStatusWidget* self, PrintState state) {
            if (!self->widget_obj_)
                return;
            self->on_print_state_changed(state);
        },
        printer_state_.get_subjects_lifetime());

    // Use observe_string_immediate: the thumbnail handler only calls lv_image_set_src
    // (no observer lifecycle changes), and set_print_thumbnail is always called
    // from the UI thread via queue_update. Immediate avoids the double-deferral that
    // caused stale reads when the subject changed between notification and handler.
    print_thumbnail_path_observer_ = observe_string_immediate<PrintStatusWidget>(
        printer_state_.get_print_thumbnail_path_subject(), this,
        [](PrintStatusWidget* self, const char* path) {
            if (!self->widget_obj_)
                return;
            self->on_print_thumbnail_path_changed(path);
        },
        printer_state_.get_subjects_lifetime());

#if defined(HELIX_PLATFORM_ESP32)
    // ESP32 has no disk thumbnail cache, so print_thumbnail_path stays empty and
    // the image arrives as a PSRAM buffer instead. Observe the generation counter
    // ActivePrintMediaManager bumps when it installs one. observe_int_immediate
    // for the same reason as the path observer above: the handler only does
    // lv_image_set_src plus a shared_ptr swap (no observer lifecycle changes, no
    // widget destruction), and the setter always runs on the UI thread — so the
    // extra deferral would only add a frame and a stale-read window.
    print_psram_thumb_observer_ = observe_int_immediate<PrintStatusWidget>(
        printer_state_.get_print_psram_thumb_gen_subject(), this,
        [](PrintStatusWidget* self, int /*gen*/) {
            if (!self->widget_obj_)
                return;
            self->apply_esp_psram_thumbnail();
        },
        printer_state_.get_subjects_lifetime());
#endif

    auto& fsm = helix::FilamentSensorManager::instance();
    filament_runout_observer_ = observe_int_sync<PrintStatusWidget>(
        fsm.get_any_runout_subject(), this, [](PrintStatusWidget* self, int any_runout) {
            if (!self->widget_obj_)
                return;
            spdlog::debug("[PrintStatusWidget] Filament runout subject changed: {}", any_runout);
            if (any_runout == 1) {
                self->check_and_show_idle_runout_modal();
            } else {
                self->runout_modal_shown_ = false;
            }
        });

    // Observe job queue count to show/hide queue row
    auto* jq_count_subj = lv_xml_get_subject(nullptr, "job_queue_count");
    if (jq_count_subj) {
        job_queue_count_observer_ = helix::ui::observe_int_sync<PrintStatusWidget>(
            jq_count_subj, this, [](PrintStatusWidget* self, int /*count*/) {
                if (!self->widget_obj_)
                    return;
                self->update_job_queue_row_visibility();
            });
    }

    // Register history observer to update idle thumbnail when history loads.
    // PrintHistoryManager fires observers on the main thread today, but
    // tok.defer() future-proofs against any bg-thread callsite and satisfies
    // the L081 lint gate (no bare `if (tok.expired()) return;` + `this` access).
    auto token = lifetime_.token();
    history_changed_cb_ = [this, token]() {
        token.defer("PrintStatusWidget::on_history_changed", [this]() {
            if (!widget_obj_ || !print_card_thumb_)
                return;
            bool is_idle = !job_holds_machine(printer_state_.get_print_lifecycle());
            if (is_idle) {
                // Defer: token.defer body runs inside UpdateQueue::process_pending,
                // and synchronous reset_print_card_to_idle would cascade lv_image_set_src
                // into a grid layout that populate_page may be mid-rebuilding.
                defer_reset_print_card_to_idle();
            }
        });
    };
    if (auto* hm = get_print_history_manager()) {
        hm->add_observer(&history_changed_cb_);
        // Populate history so the idle thumbnail shows the last print (not
        // benchy). ensure_loaded(), not fetch(): the observer just registered
        // is served by whatever response is already in flight, while fetch()
        // would read this ask as an invalidation and queue a second identical
        // request.
        hm->ensure_loaded(HistoryScope::RECENT);
    }

    // Observe connection state to fetch history once connected (widget may
    // attach before the WebSocket connection is established)
    connection_observer_ = helix::ui::observe_int_sync<PrintStatusWidget>(
        printer_state_.get_printer_connection_state_subject(), this,
        [](PrintStatusWidget* /*self*/, int state) {
            if (state == static_cast<int>(ConnectionState::CONNECTED)) {
                if (auto* hm = get_print_history_manager()) {
                    hm->ensure_loaded(HistoryScope::RECENT);
                }
            }
        },
        printer_state_.get_subjects_lifetime());

    spdlog::debug("[PrintStatusWidget] Subscribed to print state/progress/time/thumbnail/runout");

    // Check initial print state
    if (print_card_thumb_ && print_card_active_thumb_) {
        const PrintState state = printer_state_.get_print_lifecycle();
        if (job_holds_machine(state)) {
            on_print_state_changed(state);
#if defined(HELIX_PLATFORM_ESP32)
            // Widget instances are recycled across page rebuilds, so a fresh
            // attach lands on a print that already has its thumbnail loaded and
            // no further generation bump coming. Re-apply from the held buffer;
            // on other platforms the print_thumbnail_path observer's initial
            // notification covers this.
            apply_esp_psram_thumbnail();
#endif
        } else {
            // Defer the initial idle reset: synchronous reset_print_card_to_idle
            // cascades lv_image_set_src → update_align → lv_obj_update_layout up
            // to the page grid that populate_page is still building sibling
            // widgets into, and grid_update crashes on half-initialized track
            // data (AD5M SY6JLLKJ, Pi5 FFATPQWB).
            defer_reset_print_card_to_idle();
        }
        spdlog::debug("[PrintStatusWidget] Found print card widgets for dynamic updates");
    } else {
        spdlog::warn("[PrintStatusWidget] Could not find all print card widgets "
                     "(thumb={}, active_thumb={})",
                     print_card_thumb_ != nullptr, print_card_active_thumb_ != nullptr);
    }

    // Apply section visibility from config (drives all print_status_*_hidden subjects)
    apply_visibility_config();

    // Re-run visibility when the breakpoint changes so the 'Print Library' header
    // hides on shrink-to-micro and returns on grow-past-micro.
    if (auto* bp_subj = theme_manager_get_breakpoint_subject()) {
        breakpoint_observer_ = observe_int_sync<PrintStatusWidget>(
            bp_subj, this, [](PrintStatusWidget* self, int /*bp*/) {
                if (self->widget_obj_)
                    self->apply_visibility_config();
            });
    }

    // Explicit visibility pass — observer fires may be deferred; ensure correct
    // Library/Detailed sibling is shown immediately on attach.
    update_idle_compact_mode();
    update_active_layout_mode();

    // Sync the imperative print-card flex layout to the persisted is_column_.
    // Instances are recycled across rebuilds onto a fresh component whose default
    // flow is column; without this a recycled row-layout card whose colspan
    // matches is_column_ would keep the default column layout (see #1109 pattern).
    apply_card_layout();

    spdlog::debug("[PrintStatusWidget] Attached (layout_style={})", layout_style_);
}

void PrintStatusWidget::detach() {
    // Dismiss any open pickers
    configure_picker_.hide();
    nozzle_picker_.hide();

    // Invalidate lifetime guard FIRST to abort in-flight async fetches
    lifetime_.invalidate();
    live_instances().erase(this);

    // Unregister history observer
    if (auto* hm = get_print_history_manager()) {
        hm->remove_observer(&history_changed_cb_);
    }

    // Release observers
    print_state_observer_.reset();
    print_thumbnail_path_observer_.reset();
#if defined(HELIX_PLATFORM_ESP32)
    print_psram_thumb_observer_.reset();
    // detach() is main-thread, which EspPsramThumbnail's destructor requires.
    // Stop the image pointing at the descriptor before releasing: instances are
    // recycled, so the lv_image outlives this detach, and for a variable source
    // lv_image stores the raw pointer (it only strdups paths). Ours can be the
    // last reference — PrinterState drops its own on the next filename change.
    if (esp_thumbnail_ && print_card_active_thumb_ &&
        lv_image_get_src(print_card_active_thumb_) == esp_thumbnail_->dsc()) {
        lv_image_set_src(print_card_active_thumb_,
                         helix::PrinterPrintState::no_thumbnail_placeholder());
    }
    esp_thumbnail_.reset();
#endif
    filament_runout_observer_.reset();
    job_queue_count_observer_.reset();
    connection_observer_.reset();
    breakpoint_observer_.reset();

    // Heater icon animators — per-instance, so unbinding here cannot disturb
    // a recycled successor widget's binders.
    nozzle_icon_binder_.unbind();
    bed_icon_binder_.unbind();
    chamber_icon_binder_.unbind();

    // Clear widget references
    print_card_thumb_ = nullptr;
    print_card_active_thumb_ = nullptr;
    print_card_layout_ = nullptr;
    print_card_thumb_wrap_ = nullptr;
    print_card_info_ = nullptr;
    print_card_printing_ = nullptr;
    print_card_preparing_info_ = nullptr;
    print_card_idle_ = nullptr;
    print_card_idle_compact_ = nullptr;
    print_card_idle_detailed_ = nullptr;
    print_card_printing_detailed_ = nullptr;
    print_card_thumb_compact_ = nullptr;
    library_row_last_ = nullptr;
    compact_row_last_ = nullptr;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[PrintStatusWidget] Detached");

    // NOTE: s_formatter_refcount_ is decremented in ~PrintStatusWidget, not
    // here, because the dtor ALSO calls detach(). Double-decrement would
    // corrupt the count with multiple PrintStatusWidget instances live (panel
    // manager may keep up to 4 — one per breakpoint variant). detach() is
    // safely idempotent — re-entry resets observers that are already null.
}

// ============================================================================
// Size-Dependent Layout
// ============================================================================

void PrintStatusWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int width_px,
                                        int height_px) {
    last_width_px_ = width_px;
    last_height_px_ = height_px;

    // Width band from physical pixels, not colspan — see panel_widget_size.h. Three
    // bands (0=compact, 1=normal, 2=wide) mirror the old colspan<=1 / ==2 / >=3
    // taxonomy closely enough to drive the same predicates below. Published to XML:
    // library_body's two bind_style entries (panel_widget_print_status.xml) key off
    // this band, not the raw pixel count, which wouldn't mean anything to a ref_value
    // comparison.
    int width_band;
    if (width_px < widget_size::w_normal()) {
        width_band = 0; // compact
    } else if (width_px < widget_size::w_wide()) {
        width_band = 1; // normal
    } else {
        width_band = 2; // wide
    }
    lv_subject_set_int(&width_band_subject_, width_band);

    // Derive layout_effective: detailed only when user opted in AND width clears the normal floor
    int user_pref = (layout_style_ == "detailed") ? 1 : 0;
    int effective = (user_pref == 1 && width_band >= 1) ? 1 : 0;
    lv_subject_set_int(&layout_effective_subject_, effective);
    // Combined gate: only show the filament line at the wide band AND when actual
    // filament has been extruded. update_filament_text() also writes this
    // subject on used_mm changes, keeping both inputs in sync.
    int used_mm = lv_subject_get_int(printer_state_.get_print_filament_used_subject());
    lv_subject_set_int(&show_filament_active_subject_, (width_band >= 2 && used_mm > 0) ? 1 : 0);

    // Compact mode: narrow — not enough horizontal space for thumbnail + action rows
    bool compact = (width_band == 0);
    if (compact != is_compact_) {
        is_compact_ = compact;
        update_idle_compact_mode();
        update_active_layout_mode();
    }

    if (!print_card_layout_ || !print_card_thumb_wrap_) {
        return;
    }

    // Normal band + tall enough: column layout (thumbnail on top, info below)
    // Compact or wide band: row layout (thumbnail left, info right)
    bool use_column = (width_band == 1 && height_px >= widget_size::h_tall());
    if (use_column == is_column_) {
        return;
    }
    is_column_ = use_column;
    apply_card_layout();

    spdlog::debug("[PrintStatusWidget] on_size_changed {}x{}px -> {} (compact={})", width_px,
                  height_px, use_column ? "column" : "row", is_compact_);
}

// Apply the imperative print-card flex layout (thumbnail row vs column) for the
// current is_column_ state. Split out of on_size_changed so attach() can re-apply
// it: widget instances are recycled across rebuilds, but a fresh XML component
// starts at the default flow (column). Without an attach()-time apply, a card
// whose new colspan matches the persisted is_column_ makes on_size_changed
// early-return (use_column == is_column_) and the imperative layout is never
// established on the fresh component — leaving a row-layout card (1x2/3x2) stuck
// in the default column arrangement. Same recycled-instance class as #1109.
void PrintStatusWidget::apply_card_layout() {
    if (!print_card_layout_ || !print_card_thumb_wrap_)
        return;

    const bool use_column = is_column_;

    // Update subject for declarative icon visibility in XML
    lv_subject_set_int(&column_mode_subject_, use_column ? 1 : 0);

    auto apply_info_layout = [use_column](lv_obj_t* info) {
        if (!info)
            return;
        if (use_column) {
            lv_obj_set_width(info, LV_PCT(100));
            lv_obj_set_height(info, LV_SIZE_CONTENT);
            lv_obj_set_style_flex_grow(info, 0, 0);
        } else {
            lv_obj_set_height(info, LV_PCT(100));
            lv_obj_set_width(info, LV_SIZE_CONTENT);
            lv_obj_set_style_flex_grow(info, 1, 0);
        }
    };

    if (use_column) {
        lv_obj_set_flex_flow(print_card_layout_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(print_card_thumb_wrap_, LV_PCT(100));
        lv_obj_set_style_flex_grow(print_card_thumb_wrap_, 1, 0);
    } else {
        lv_obj_set_flex_flow(print_card_layout_, LV_FLEX_FLOW_ROW);
        lv_obj_set_width(print_card_thumb_wrap_, LV_PCT(40));
        lv_obj_set_height(print_card_thumb_wrap_, LV_PCT(100));
        lv_obj_set_style_flex_grow(print_card_thumb_wrap_, 0, 0);
    }
    apply_info_layout(print_card_preparing_info_);
    apply_info_layout(print_card_info_);

    // Re-fit the Detailed-layout progress arc to a square sized from its
    // (now-known) parent column dimensions.
    if (s_formatter_) {
        s_formatter_->resize_arc();
    }
}

void PrintStatusWidget::update_view_subject() {
    bool use_detailed = (layout_style_ == "detailed") && !is_compact_;
    int v;
    if (is_active_) {
        v = use_detailed ? 4 : 3;
    } else {
        v = use_detailed ? 2 : (is_compact_ ? 1 : 0);
    }
    lv_subject_set_int(&view_subject_, v);
}

// Kept as thin wrappers so existing call sites (on_size_changed,
// set_config, picker layout-button cbs, on_print_state_changed) remain
// readable. All three roads now lead through update_view_subject().
void PrintStatusWidget::update_idle_compact_mode() {
    update_view_subject();
}
void PrintStatusWidget::update_active_layout_mode() {
    update_view_subject();
}

// ============================================================================
// Print Card Click Handler
// ============================================================================

void PrintStatusWidget::handle_print_card_clicked() {
    // Startup grace period: reject phantom clicks during early boot
    auto elapsed = std::chrono::steady_clock::now() - AppConstants::Startup::PROCESS_START_TIME;
    if (elapsed < AppConstants::Startup::PRINT_START_GRACE_PERIOD) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        spdlog::warn("[PrintStatusWidget] Rejected print card click during startup grace period "
                     "({}s < {}s)",
                     secs, AppConstants::Startup::PRINT_START_GRACE_PERIOD.count());
        return;
    }

    if (!printer_state_.can_start_new_print()) {
        // Print in progress - show print status overlay
        spdlog::info(
            "[PrintStatusWidget] Print card clicked - showing print status (print in progress)");

        if (!PrintStatusPanel::push_overlay(parent_screen_)) {
            spdlog::error("[PrintStatusWidget] Failed to push print status overlay");
        }
    } else {
        // No print in progress - navigate to print select panel (same as "Print Files")
        handle_library_files();
    }
}

// ============================================================================
// Library Action Handlers
// ============================================================================

void PrintStatusWidget::handle_library_files() {
    spdlog::info("[PrintStatusWidget] Library: Print Files");
    NavigationManager::instance().set_active(PanelId::PrintSelect);
}

void PrintStatusWidget::handle_library_last() {
    if (!last_print_available_) {
        return;
    }

    auto* history = get_print_history_manager();
    if (!history || !history->is_loaded(HistoryScope::RECENT)) {
        spdlog::info("[PrintStatusWidget] Library: Print Last - no history available");
        return;
    }

    const PrintHistoryJob* last_job = history->get_newest_existing_job();
    if (!last_job) {
        spdlog::info("[PrintStatusWidget] Library: Print Last - no files exist on disk");
        return;
    }

    spdlog::info("[PrintStatusWidget] Library: Print Last -> {}", last_job->filename);

    // Navigate to PrintSelectPanel, select the file, and return to home on back
    NavigationManager::instance().set_active(PanelId::PrintSelect);

    auto* panel = get_print_select_panel(printer_state_, get_moonraker_api());
    if (panel) {
        panel->set_return_to_home_on_close();
        if (!panel->select_file_by_name(last_job->filename)) {
            panel->set_pending_file_selection(last_job->filename);
        }
    }
}

void PrintStatusWidget::handle_library_recent() {
    spdlog::info("[PrintStatusWidget] Library: Recent");

    NavigationManager::instance().set_active(PanelId::PrintSelect);

    auto* panel = get_print_select_panel(printer_state_, get_moonraker_api());
    if (panel) {
        panel->set_sort_recent();
    }
}

void PrintStatusWidget::handle_library_queue() {
    spdlog::info("[PrintStatusWidget] Library: Job Queue");
    if (parent_screen_) {
        job_queue_modal_.show(parent_screen_);
    }
}

void PrintStatusWidget::update_job_queue_row_visibility() {
    // Queue row: visible when config allows AND there are jobs in queue.
    // Purely subject-driven — XML binds hidden flag to print_status_queue_hidden.
    bool has_jobs = false;
    auto* jq_count_subj = lv_xml_get_subject(nullptr, "job_queue_count");
    if (jq_count_subj) {
        has_jobs = lv_subject_get_int(jq_count_subj) > 0;
    }
    bool queue_visible = show_job_queue_ && has_jobs;
    lv_subject_set_int(&queue_hidden_subject_, queue_visible ? 0 : 1);

    // The library_actions container hides when no action row is visible — re-evaluate
    // here since queue visibility contributes to that combined state.
    recompute_actions_visibility();
}

// ============================================================================
// Observer Callbacks
// ============================================================================

void PrintStatusWidget::on_print_state_changed(PrintState state) {
    if (!widget_obj_ || !print_card_thumb_) {
        return;
    }

    // job_holds_machine(), not the wire: during a host-side pre-print block the
    // printer still reports standby, and the card claimed nothing was happening
    // while the machine homed and probed (seen on the K2). Tapping it went to the
    // file browser instead of the status overlay.
    is_active_ = job_holds_machine(state);

    // The 5 card-body siblings are subject-driven (bind_flag_if_not_eq on
    // print_status_view). Recompute that subject; XML handles visibility.
    update_view_subject();

    if (is_active_) {
        spdlog::debug("[PrintStatusWidget] Print active - state updated via subject bindings");
    } else {
        spdlog::debug("[PrintStatusWidget] Print not active - reverting card to idle state");
        // print_state_observer_ fires deferred via UpdateQueue::process_pending;
        // synchronous reset_print_card_to_idle would crash grid_update if
        // populate_page is concurrently rebuilding the page grid (J2URYGSM AD5M).
        defer_reset_print_card_to_idle();
    }
}

void PrintStatusWidget::on_print_thumbnail_path_changed(const char* path) {
    if (!widget_obj_ || !print_card_active_thumb_) {
        return;
    }

    // No empty-path branch: ActivePrintMediaManager is the subject's sole writer
    // and publishes no_thumbnail_placeholder() — the very image this used to
    // substitute — when a file has no thumbnail, so the value is always an image.
    defer_apply_active_thumbnail(path);
}

#if defined(HELIX_PLATFORM_ESP32)
void PrintStatusWidget::apply_esp_psram_thumbnail() {
    if (!widget_obj_ || !print_card_active_thumb_) {
        return;
    }
    auto thumb = printer_state_.get_print_psram_thumbnail();
    if (!thumb) {
        return;
    }
    // `previous` keeps the outgoing buffer alive until after the widget stops
    // pointing at it — otherwise the last release could free the descriptor the
    // widget's src still names. Both releases happen here, on the main thread,
    // which is what EspPsramThumbnail's destructor requires.
    auto previous = std::move(esp_thumbnail_);
    esp_thumbnail_ = std::move(thumb);
    lv_image_set_src(print_card_active_thumb_, esp_thumbnail_->dsc());
    spdlog::info("[PrintStatusWidget] Active print PSRAM thumbnail applied");
}
#endif

std::string PrintStatusWidget::get_last_print_thumbnail_path() const {
    auto* history = get_print_history_manager();
    if (!history) {
        return {};
    }

    // A deleted file's thumbnail never 404s: the cache is keyed on the job's
    // relative path and validated against its `modified` stamp, neither of
    // which changes when the gcode is removed. Skipping the job is the only
    // thing that stops the dead file's image being served.
    const PrintHistoryJob* newest = history->get_newest_existing_job();
    if (!newest) {
        return {};
    }
    const auto& job = *newest;

    // Select the best thumbnail for the widget's actual rendered size
    if (!job.thumbnails.empty() && print_card_thumb_ && lv_obj_is_valid(print_card_thumb_)) {
        int target_w = lv_obj_get_width(print_card_thumb_);
        int target_h = lv_obj_get_height(print_card_thumb_);

        // Find smallest thumbnail that meets or exceeds the widget dimensions
        const ThumbnailInfo* best_adequate = nullptr;
        const ThumbnailInfo* largest = &job.thumbnails[0];

        for (const auto& t : job.thumbnails) {
            if (t.pixel_count() > largest->pixel_count()) {
                largest = &t;
            }
            if (t.width >= target_w && t.height >= target_h) {
                if (!best_adequate || t.pixel_count() < best_adequate->pixel_count()) {
                    best_adequate = &t;
                }
            }
        }

        const auto* best = best_adequate ? best_adequate : largest;
        spdlog::debug("[PrintStatusWidget] Widget {}x{}, selected thumbnail {}x{} ({})", target_w,
                      target_h, best->width, best->height, best->relative_path);
        return best->relative_path;
    }

    // Fallback: use pre-selected largest thumbnail
    return job.thumbnail_path;
}

time_t PrintStatusWidget::get_last_print_source_modified() const {
    auto* history = get_print_history_manager();
    if (!history) {
        return 0;
    }

    // Same entry get_last_print_thumbnail_path() picks its key from, so the
    // freshness stamp always describes the key it is validating.
    const PrintHistoryJob* newest = history->get_newest_existing_job();
    return newest ? static_cast<time_t>(newest->modified) : 0;
}

void PrintStatusWidget::defer_reset_print_card_to_idle() {
    // Raw lv_async_call escapes the UpdateQueue::process_pending() batch (see
    // CLAUDE.md "Safe escape routes"). live_instances() + widget_obj_ guard UAF
    // if the widget is destroyed before the next tick.
    lv_async_call(
        [](void* ud) {
            auto* self = static_cast<PrintStatusWidget*>(ud);
            if (live_instances().count(self) != 0 && self->widget_obj_) {
                self->reset_print_card_to_idle();
            }
        },
        this);
}

void PrintStatusWidget::defer_apply_active_thumbnail(const char* path) {
    // Heap-allocate the payload so lv_async_call can carry it as void*, and copy
    // the path: the subject may publish again before the tick, and the pointer it
    // handed us is its own buffer. The LifetimeToken (not a live_instances()
    // lookup) is what keeps this safe — detach() invalidates it, so a pending
    // write cannot land on a widget that has already let go of its objects.
    struct PendingThumb {
        helix::LifetimeToken token;
        PrintStatusWidget* self;
        std::string path;
    };
    auto* pending = new PendingThumb{lifetime_.token(), this, path ? path : ""};

    // Raw lv_async_call escapes the UpdateQueue::process_pending() batch this
    // observer body runs in, the same escape defer_reset_print_card_to_idle()
    // makes for the idle sibling.
    lv_async_call(
        [](void* ud) {
            std::unique_ptr<PendingThumb> p(static_cast<PendingThumb*>(ud));
            if (p->token.expired())
                return;
            PrintStatusWidget* self = p->self;
            if (!self->widget_obj_ || !self->print_card_active_thumb_)
                return;

            // Trigger hardening (#1001), matching reset_print_card_to_idle():
            // by the time the tick fires, populate_page's safe_clean_children()
            // may have reparented this subtree onto lv_layer_top() to await
            // deletion. lv_image_set_src → update_align → lv_obj_update_layout
            // would then walk the whole layer and recurse into sibling condemned
            // grid subtrees whose children may already be freed → SIGSEGV in
            // grid calc().
            if (!helix::ui::is_on_active_screen(self->print_card_active_thumb_)) {
                spdlog::debug("[PrintStatusWidget] Skip active thumbnail: off active screen "
                              "(mid-teardown)");
                return;
            }

            lv_image_set_src(self->print_card_active_thumb_, p->path.c_str());
            spdlog::info("[PrintStatusWidget] Active print thumbnail updated: {}", p->path);
        },
        pending);
}

void PrintStatusWidget::reset_print_card_to_idle() {
    // Update "Print Last" row availability
    update_last_print_availability();

    if (!print_card_thumb_ || !lv_obj_is_valid(print_card_thumb_)) {
        return;
    }

    // Trigger hardening (#1001): deferral alone (defer_reset_print_card_to_idle) is
    // not enough — by the time the async tick fires, populate_page's
    // safe_clean_children() may have reparented this widget's subtree onto
    // lv_layer_top() to await deletion. Setting the idle thumb's image src here would
    // cascade lv_image_set_src → update_align → lv_obj_update_layout across the whole
    // layer, recursing into sibling condemned grid subtrees whose children may already
    // be freed → SIGSEGV in grid calc() (the LVGL guard patch is the second net).
    if (!helix::ui::is_on_active_screen(print_card_thumb_)) {
        spdlog::debug(
            "[PrintStatusWidget] Skip idle reset: thumb off active screen (mid-teardown)");
        return;
    }

    // Every idle reset supersedes whatever thumbnail load is still in flight for
    // the previous history head. Created here rather than at the fetch below so
    // it also covers the cache-hit exit: that path publishes synchronously, and
    // an older fetch completing afterwards would otherwise overwrite it.
    auto ctx = ThumbnailLoadContext::create(lifetime_, &idle_thumb_generation_);

    // Try to show the last printed file's thumbnail instead of benchy
    std::string thumb_rel_path = get_last_print_thumbnail_path();
    if (thumb_rel_path.empty()) {
        set_thumb_on_widgets(benchy_thumb_path());
        spdlog::debug("[PrintStatusWidget] Idle thumbnail: benchy (no history)");
        return;
    }

    // Compute pre-scale target from actual widget size (not hardcoded breakpoints)
    int widget_w = lv_obj_get_width(print_card_thumb_);
    int widget_h = lv_obj_get_height(print_card_thumb_);
    auto target = helix::ThumbnailProcessor::get_target_for_resolution(
        widget_w, widget_h, helix::ThumbnailSize::Detail);

    // One request describes both the synchronous probe and the async fetch
    // below, so the two cannot disagree about what they are looking for. In
    // particular source_modified applies to BOTH: the probe answers first, and
    // without it a render from a previous slice of the same filename is served
    // indefinitely and the guarded fetch is never reached.
    ThumbnailRequest req;
    req.key = thumb_rel_path;
    req.target = target;
    req.source_modified = get_last_print_source_modified();

    // Check if we already have a fresh pre-scaled BIN version
    auto cached = get_thumbnail_cache().get_if_cached(req);
    if (!cached.empty()) {
        set_thumb_on_widgets(cached.c_str());
        spdlog::debug("[PrintStatusWidget] Idle thumbnail from cache: {}", cached);
        return;
    }

    // Set benchy as placeholder while we fetch
    set_thumb_on_widgets(benchy_thumb_path());

    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::debug("[PrintStatusWidget] Idle thumbnail: benchy (no API)");
        return;
    }

    // Fetch async from Moonraker. ctx carries both the lifetime token and the
    // generation, so ThumbnailCache drops a result a later reset has superseded
    // before it ever reaches this callback.
    req.api = api;

    auto token = lifetime_.token();

    get_thumbnail_cache().fetch(
        req, ctx,
        [this, token](const std::string& lvgl_path, bool /*degraded*/) {
            // Marshal first, then touch members — a bare expired() check
            // followed by a `this` dereference is L081 Mechanism C.
            token.defer("PrintStatusWidget::apply_idle_thumb", [this, lvgl_path]() {
                set_thumb_on_widgets(lvgl_path.c_str());
                spdlog::info("[PrintStatusWidget] Idle thumbnail loaded: {}", lvgl_path);
            });
        },
        [](const std::string& error) {
            spdlog::debug("[PrintStatusWidget] Idle thumbnail fetch failed: {}", error);
        });
}

void PrintStatusWidget::set_thumb_on_widgets(const char* src) {
    // Library-mode thumbs are imperative (they don't bind_src); the subject is
    // what drives the detailed-idle hero. All three must move together or the
    // hero shows a different image from the thumbs next to it.
    if (print_card_thumb_ && lv_obj_is_valid(print_card_thumb_)) {
        lv_image_set_src(print_card_thumb_, src);
    }
    if (print_card_thumb_compact_ && lv_obj_is_valid(print_card_thumb_compact_)) {
        lv_image_set_src(print_card_thumb_compact_, src);
    }
    lv_subject_copy_string(&idle_thumb_path_subject_, src);
}

void PrintStatusWidget::update_last_print_availability() {
    auto* history = get_print_history_manager();
    last_print_available_ = history && history->get_newest_existing_job() != nullptr;

    // Apply to both full and compact "Print Last" rows
    lv_obj_t* rows[] = {library_row_last_, compact_row_last_};
    for (auto* row : rows) {
        if (!row || !lv_obj_is_valid(row))
            continue;
        if (last_print_available_) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(row, LV_OPA_100, 0);
        } else {
            lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(row, LV_OPA_40, 0);
        }
    }
}

// ============================================================================
// Filament Runout Modal
// ============================================================================

void PrintStatusWidget::check_and_show_idle_runout_modal() {
    // Grace period - don't show modal during startup
    auto& fsm = helix::FilamentSensorManager::instance();
    if (fsm.is_in_startup_grace_period()) {
        spdlog::debug("[PrintStatusWidget] In startup grace period - skipping runout modal");
        return;
    }

    // Verify actual sensor state. Use has_real_runout() (not has_any_runout())
    // so a runout sensor on an intentionally-empty / never-loaded AMS lane does
    // NOT raise the idle modal — e.g. a multi-color print using heads 0+2 with
    // head 1 left empty. A lane that was loaded and lost filament mid-use still
    // counts as a real runout. (Snapmaker U1 false-alarm fix.)
    if (!fsm.has_real_runout()) {
        spdlog::debug("[PrintStatusWidget] No real runout detected - skipping modal");
        return;
    }

    // Check suppression logic (AMS without bypass, wizard active, etc.)
    if (!get_runtime_config()->should_show_runout_modal()) {
        spdlog::debug("[PrintStatusWidget] Runout modal suppressed by runtime config");
        return;
    }

    // A manual load/unload deliberately drags filament past the runout sensor;
    // the resulting empty reading is not a real runout. Mirrors the toast
    // suppression in FilamentSensorManager (is_filament_operation_active()).
    // This is the idle case that fired the spurious modal on the U1 when a lane
    // was unloaded by hand while the printer sat idle.
    if (AmsState::instance().is_filament_operation_active()) {
        spdlog::debug(
            "[PrintStatusWidget] AMS filament operation in progress - skipping runout modal");
        return;
    }

    // Only show modal if not already shown
    if (runout_modal_shown_) {
        spdlog::debug("[PrintStatusWidget] Runout modal already shown - skipping");
        return;
    }

    // Only show if the machine is genuinely between jobs. Same three states as
    // before plus Preparing, which the wire could not express: a "load filament"
    // dialog on top of a start the user just committed to is an ambush, and the
    // block below would burn the one-shot grace on the way past.
    const PrintState lifecycle = printer_state_.get_print_lifecycle();
    if (lifecycle != PrintState::Idle && lifecycle != PrintState::Complete &&
        lifecycle != PrintState::Cancelled) {
        spdlog::debug(
            "[PrintStatusWidget] Print active (lifecycle={}) - skipping idle runout modal",
            static_cast<int>(lifecycle));
        return;
    }

    // Some backends (Snapmaker U1) drive load/unload entirely on their own, so an
    // idle lane going empty — a hand-pull, or a lane simply left unloaded — needs
    // no operator action and the runout-guidance modal is just noise. Other
    // backends that require manual intervention keep the idle modal. Mid-print
    // runout is a separate path (FilamentRunoutHandler) and is unaffected.
    if (auto* backend = AmsState::instance().get_backend(0);
        backend && backend->should_suppress_idle_runout_modal()) {
        spdlog::debug("[PrintStatusWidget] AMS idle - suppressing runout modal");
        return;
    }

    // An unload the user just asked for ends by dragging filament off the
    // sensor. is_filament_operation_active() above only covers the window while
    // the action runs, and the removal edge can land seconds after it finishes.
    // Taken last, immediately before the modal: it is one-shot, so consuming it
    // above a gate that returns anyway burns it on a call that could never have
    // shown anything, and the deliberate unload it was armed for pops the modal
    // unsuppressed.
    if (AmsState::instance().consume_post_unload_runout_grace()) {
        spdlog::info("[PrintStatusWidget] Skipping runout modal — filament left after an unload");
        return;
    }

    spdlog::info("[PrintStatusWidget] Showing idle runout modal");
    show_idle_runout_modal();
    runout_modal_shown_ = true;
}

void PrintStatusWidget::trigger_idle_runout_check() {
    spdlog::debug("[PrintStatusWidget] Triggering deferred runout check");
    runout_modal_shown_ = false;
    check_and_show_idle_runout_modal();
}

void PrintStatusWidget::show_idle_runout_modal() {
    if (runout_modal_.is_visible()) {
        return;
    }

    // The widget is recycled by the panel manager (attach A -> detach A ->
    // attach B) and destroyed on dashboard rebuild, while RunoutGuidanceModal
    // retains this callback until it is overwritten. Guard with the same
    // AsyncLifetimeGuard token FilamentRunoutHandler uses; the press arrives on
    // the main thread, so a plain expired() check is correct here.
    // A detected runout is a warning, not an advisory tap. State it here rather
    // than inheriting: the subject is shared with the home tile's tap modal,
    // which sets it the other way — see RunoutGuidanceModal::set_advisory().
    runout_modal_.set_advisory(false);

    auto token = lifetime_.token();
    runout_modal_.set_on_load_filament([this, token]() {
        if (token.expired())
            return;
        spdlog::info("[PrintStatusWidget] User chose to load filament (idle)");
        dispatch_load();
    });

    runout_modal_.set_on_resume([]() {
        // Resume not applicable when idle
    });

    runout_modal_.set_on_cancel_print([]() {
        // Cancel not applicable when idle
    });

    runout_modal_.show(parent_screen_);
}

void PrintStatusWidget::dispatch_load() {
    AmsBackend* backend = AmsState::instance().get_backend();
    // Nothing is printing, so there is no "currently feeding" lane to infer from
    // a print job — the backend's own active slot is the only target available,
    // and this dialog has no slot picker.
    const int slot = backend ? backend->get_current_slot() : -1;
    helix::ui::execute_filament_load(backend, slot, "[PrintStatusWidget]");
}

// ============================================================================
// Configuration
// ============================================================================

void PrintStatusWidget::set_config(const nlohmann::json& config) {
    config_ = config;
    if (config.contains("layout_style") && config["layout_style"].is_string()) {
        std::string ls = config["layout_style"].get<std::string>();
        if (ls == "library" || ls == "detailed") {
            layout_style_ = std::move(ls);
        }
    }
    if (config.contains("nozzle_tool_override") && config["nozzle_tool_override"].is_string()) {
        nozzle_tool_override_ = config["nozzle_tool_override"].get<std::string>();
    }
    if (config.contains("show_title") && config["show_title"].is_boolean()) {
        show_title_ = config["show_title"].get<bool>();
    }
    if (config.contains("show_print_files") && config["show_print_files"].is_boolean()) {
        show_print_files_ = config["show_print_files"].get<bool>();
    }
    if (config.contains("show_reprint_last") && config["show_reprint_last"].is_boolean()) {
        show_reprint_last_ = config["show_reprint_last"].get<bool>();
    }
    if (config.contains("show_recent_prints") && config["show_recent_prints"].is_boolean()) {
        show_recent_prints_ = config["show_recent_prints"].get<bool>();
    }
    if (config.contains("show_job_queue") && config["show_job_queue"].is_boolean()) {
        show_job_queue_ = config["show_job_queue"].get<bool>();
    }
    if (s_formatter_ && !s_formatter_->set_nozzle_tool_override(nozzle_tool_override_)) {
        nozzle_tool_override_ = "auto";
        config_["nozzle_tool_override"] = "auto";
        save_widget_config(config_);
    }
    // Re-apply layout visibility — layout_style may have changed.
    if (widget_obj_ && lv_obj_is_valid(widget_obj_)) {
        update_idle_compact_mode();
        update_active_layout_mode();
    }
}

bool PrintStatusWidget::on_edit_configure() {
    spdlog::info("[PrintStatusWidget] Configure requested - showing section picker");
    show_configure_picker();
    return false; // picker handles save internally, no rebuild needed
}

void PrintStatusWidget::apply_visibility_config() {
    // Per-element hidden flags are driven entirely by subjects; XML binds hidden
    // to print_status_*_hidden via bind_flag_if_eq ref_value="1". Here we just
    // compute each value from config + live breakpoint and push into subjects.
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    bool at_micro = bp_subj && as_breakpoint(lv_subject_get_int(bp_subj)) == UiBreakpoint::Micro;

    lv_subject_set_int(&title_hidden_subject_, (!show_title_ || at_micro) ? 1 : 0);
    lv_subject_set_int(&files_hidden_subject_, show_print_files_ ? 0 : 1);
    lv_subject_set_int(&last_hidden_subject_, show_reprint_last_ ? 0 : 1);
    lv_subject_set_int(&recent_hidden_subject_, show_recent_prints_ ? 0 : 1);

    // Queue row uses a live count — defer to the dedicated helper, which also
    // triggers recompute_actions_visibility() since queue state affects it.
    update_job_queue_row_visibility();
}

void PrintStatusWidget::recompute_actions_visibility() {
    // The action-list container hides when no individual row is visible. That
    // also forces the thumbnail to grow to 100% width and re-centers the body.
    // Width/alignment changes stay imperative (not simple flag toggles); the
    // hidden flag itself rides the actions_hidden subject.
    bool queue_visible = lv_subject_get_int(&queue_hidden_subject_) == 0;
    bool any_button_visible =
        show_print_files_ || show_reprint_last_ || show_recent_prints_ || queue_visible;

    lv_subject_set_int(&actions_hidden_subject_, any_button_visible ? 0 : 1);

    if (!widget_obj_ || !print_card_thumb_)
        return;
    lv_obj_t* library_body = lv_obj_find_by_name(widget_obj_, "library_body");
    if (any_button_visible) {
        lv_obj_set_width(print_card_thumb_, LV_PCT(40));
        if (library_body) {
            lv_obj_set_style_flex_main_place(library_body, LV_FLEX_ALIGN_START, 0);
            lv_obj_set_style_flex_cross_place(library_body, LV_FLEX_ALIGN_START, 0);
        }
    } else {
        lv_obj_set_width(print_card_thumb_, LV_PCT(100));
        if (library_body) {
            lv_obj_set_style_flex_main_place(library_body, LV_FLEX_ALIGN_CENTER, 0);
            lv_obj_set_style_flex_cross_place(library_body, LV_FLEX_ALIGN_CENTER, 0);
        }
    }
}

void PrintStatusWidget::show_configure_picker() {
    if (configure_picker_.is_visible() || !parent_screen_ || !widget_obj_) {
        return;
    }

    // The card hangs under the widget tile, centred on it, flipping above when the
    // tile sits low on the screen.
    configure_picker_.show_below_widget(parent_screen_, widget_obj_,
                                        helix::ui::ContextMenu::AnchorAlign::Center);
}

void PrintStatusWidget::ConfigurePicker::on_created(lv_obj_t* backdrop) {
    lv_obj_t* option_list = lv_obj_find_by_name(backdrop, "option_list");
    if (!option_list) {
        spdlog::error("[PrintStatusWidget] option_list not found in picker XML");
        return;
    }

    // Helper to resolve space tokens
    auto resolve_space = [](const char* name, int fallback) -> int {
        const char* s = lv_xml_get_const(nullptr, name);
        return s ? std::atoi(s) : fallback;
    };
    int space_sm = resolve_space("space_sm", 6);
    int space_xs = resolve_space("space_xs", 4);

    // Create checkbox rows for each toggle option
    struct Option {
        const char* name; // lv_obj name for lookup in apply_state()
        const char* label;
        bool checked;
    };
    Option options[] = {
        {"opt_title", "Title", owner_.show_title_},
        {"opt_print_files", "Print Files", owner_.show_print_files_},
        {"opt_reprint_last", "Reprint Last", owner_.show_reprint_last_},
        {"opt_recent_prints", "Recent Prints", owner_.show_recent_prints_},
        {"opt_job_queue", "Job Queue", owner_.show_job_queue_},
    };

    for (const auto& opt : options) {
        lv_obj_t* row = lv_obj_create(option_list);
        lv_obj_set_name(row, opt.name);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_style_pad_gap(row, space_xs, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* cb = lv_checkbox_create(row);
        lv_checkbox_set_text(cb, "");
        lv_obj_set_style_pad_all(cb, 0, 0);
        if (opt.checked) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        }
        lv_obj_remove_flag(cb, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, opt.label);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_font(label, lv_font_get_default(), 0);

        // Click row to toggle checkbox. The picker travels as the event's
        // user_data, since a toggle applies straight through to the widget.
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] option_row_cb");
                auto* target = lv_event_get_current_target_obj(e);
                uint32_t count = lv_obj_get_child_count(target);
                for (uint32_t i = 0; i < count; ++i) {
                    lv_obj_t* child = lv_obj_get_child(target, static_cast<int32_t>(i));
                    if (lv_obj_check_type(child, &lv_checkbox_class)) {
                        if (lv_obj_has_state(child, LV_STATE_CHECKED)) {
                            lv_obj_remove_state(child, LV_STATE_CHECKED);
                        } else {
                            lv_obj_add_state(child, LV_STATE_CHECKED);
                        }
                        break;
                    }
                }

                // Apply immediately
                if (auto* picker = static_cast<ConfigurePicker*>(lv_event_get_user_data(e))) {
                    picker->apply_state();
                }
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, this);
    }

    // Initial visual state: primary-fill the selected layout button, hide the
    // Show Sections group when Detailed is active. Visuals-only — full
    // apply_state() would re-save the config on every open.
    apply_visuals();

    spdlog::debug("[PrintStatusWidget] Configure picker built");
}

void PrintStatusWidget::ConfigurePicker::apply_visuals() {
    lv_obj_t* backdrop = menu();
    if (!backdrop)
        return;

    // Library/Detailed selector buttons: selected = primary accent fill,
    // unselected = outlined (XML default).
    lv_obj_t* lib_btn = lv_obj_find_by_name(backdrop, "layout_btn_library");
    lv_obj_t* det_btn = lv_obj_find_by_name(backdrop, "layout_btn_detailed");
    if (lib_btn && det_btn) {
        bool detailed = (owner_.layout_style_ == "detailed");
        lv_obj_t* active_btn = detailed ? det_btn : lib_btn;
        lv_obj_t* inactive = detailed ? lib_btn : det_btn;
        lv_color_t accent = theme_manager_get_color("primary");
        lv_obj_set_style_bg_color(active_btn, accent, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(active_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(inactive, LV_OPA_TRANSP, LV_PART_MAIN);
    } else {
        spdlog::warn("[PrintStatusWidget] picker layout buttons not found "
                     "(layout_btn_library={}, layout_btn_detailed={}) — XML name drift?",
                     fmt::ptr(lib_btn), fmt::ptr(det_btn));
    }

    // Show Sections only applies to the Library layout — hide in Detailed.
    lv_obj_t* show_sections = lv_obj_find_by_name(backdrop, "show_sections_group");
    if (show_sections) {
        if (owner_.layout_style_ == "detailed")
            lv_obj_add_flag(show_sections, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(show_sections, LV_OBJ_FLAG_HIDDEN);
    }
}

void PrintStatusWidget::ConfigurePicker::apply_state() {
    lv_obj_t* backdrop = menu();
    if (!backdrop)
        return;

    apply_visuals();

    lv_obj_t* option_list = lv_obj_find_by_name(backdrop, "option_list");
    if (!option_list)
        return;

    // Read checkbox states by named rows (not positional index)
    auto read_checkbox = [&](const char* row_name) -> bool {
        lv_obj_t* row = lv_obj_find_by_name(option_list, row_name);
        if (!row)
            return true;
        uint32_t row_count = lv_obj_get_child_count(row);
        for (uint32_t j = 0; j < row_count; ++j) {
            lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(j));
            if (lv_obj_check_type(child, &lv_checkbox_class)) {
                return lv_obj_has_state(child, LV_STATE_CHECKED);
            }
        }
        return true;
    };

    owner_.show_title_ = read_checkbox("opt_title");
    owner_.show_print_files_ = read_checkbox("opt_print_files");
    owner_.show_reprint_last_ = read_checkbox("opt_reprint_last");
    owner_.show_recent_prints_ = read_checkbox("opt_recent_prints");
    owner_.show_job_queue_ = read_checkbox("opt_job_queue");

    // Persist
    nlohmann::json new_config = owner_.config_;
    new_config["show_title"] = owner_.show_title_;
    new_config["show_print_files"] = owner_.show_print_files_;
    new_config["show_reprint_last"] = owner_.show_reprint_last_;
    new_config["show_recent_prints"] = owner_.show_recent_prints_;
    new_config["show_job_queue"] = owner_.show_job_queue_;
    owner_.config_ = new_config;
    owner_.save_widget_config(new_config);

    // Apply visibility immediately
    owner_.apply_visibility_config();

    spdlog::info("[PrintStatusWidget] Config updated: title={}, print_files={}, reprint_last={}, "
                 "recent_prints={}, job_queue={}",
                 owner_.show_title_, owner_.show_print_files_, owner_.show_reprint_last_,
                 owner_.show_recent_prints_, owner_.show_job_queue_);
}

void PrintStatusWidget::ConfigurePicker::select_layout(const char* style) {
    owner_.layout_style_ = style;
    owner_.config_["layout_style"] = style;
    apply_state();
    // Apply to the live widget immediately so the user sees the swap behind the picker overlay.
    owner_.update_idle_compact_mode();
    owner_.update_active_layout_mode();
}

void PrintStatusWidget::ConfigurePicker::on_backdrop_clicked() {
    apply_state();
    hide();
    owner_.regate_after_configure();
}

void PrintStatusWidget::regate_after_configure() {
    // A layout_style change only reaches the visible widget once width gating has
    // been re-run against the last size the grid granted this tile.
    if (widget_obj_) {
        spdlog::debug("[PrintStatusWidget] Re-gating after configure picker ({}x{}px)",
                      last_width_px_, last_height_px_);
        on_size_changed(0, 0, last_width_px_, last_height_px_);
    }
}

void PrintStatusWidget::print_status_layout_library_cb(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] print_status_layout_library_cb");
    // The buttons live inside the configure picker's own card, so the menu on
    // screen when one is tapped is the picker that owns them.
    if (auto* picker = helix::ui::ContextMenu::active_as<ConfigurePicker>()) {
        picker->select_layout("library");
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::print_status_layout_detailed_cb(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] print_status_layout_detailed_cb");
    if (auto* picker = helix::ui::ContextMenu::active_as<ConfigurePicker>()) {
        picker->select_layout("detailed");
    }
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Nozzle Tool Picker
// ============================================================================

void PrintStatusWidget::show_nozzle_tool_picker(lv_obj_t* anchor) {
    if (nozzle_picker_.is_visible() || !parent_screen_ || !anchor)
        return;

    // Single-hotend printer: picker has nothing useful to offer. The whole
    // nozzle group is now the click target (chevron is visual-only), so this
    // can fire on a regular print and should be a clean no-op. Counts nozzles
    // rather than tools, matching the print_status_multi_tool gate that hides
    // the chevron — an AMS's filament lanes all feed the one hotend.
    if (!ToolState::instance().has_multiple_extruders()) {
        return;
    }

    // Left edge flush with the nozzle readout the tap came from, so the card
    // reads as hanging off that slot rather than off the whole print card.
    nozzle_picker_.show_below_widget(parent_screen_, anchor,
                                     helix::ui::ContextMenu::AnchorAlign::Left);
}

void PrintStatusWidget::NozzleToolPicker::on_created(lv_obj_t* backdrop) {
    lv_obj_t* option_list = lv_obj_find_by_name(backdrop, "option_list");
    if (!option_list) {
        spdlog::error("[PrintStatusWidget] option_list not found in nozzle picker XML");
        return;
    }

    // Resolve a couple of space tokens for padding.
    auto resolve_space = [](const char* name, int fallback) -> int {
        const char* s = lv_xml_get_const(nullptr, name);
        return s ? std::atoi(s) : fallback;
    };
    int space_sm = resolve_space("space_sm", 6);

    auto add_row = [this, option_list, space_sm](const char* label, const std::string& tool_key) {
        // Plain lv_obj row with a centered label — same shape the configure
        // picker uses for its checkbox rows. Going through lv_xml_create on
        // ui_button didn't render reliably here.
        lv_obj_t* row = lv_obj_create(option_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, space_sm, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, theme_manager_get_color("text"), 0);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_set_user_data(row, new RowPayload{this, tool_key});

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("nozzle_picker_row_cb");
                auto* target = lv_event_get_current_target_obj(e);
                auto* payload = static_cast<RowPayload*>(lv_obj_get_user_data(target));
                if (!payload)
                    return;

                // Copy the key: hide() takes the row - and this payload - with it.
                std::string tool_key = payload->tool_key;
                NozzleToolPicker* picker = payload->picker;
                picker->hide();

                // apply_nozzle_tool_override records whichever pin actually took
                // effect, so a name the formatter refuses never reaches config.
                picker->owner_.apply_nozzle_tool_override(tool_key);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("nozzle_picker_row_delete_cb");
                auto* target = lv_event_get_current_target_obj(e);
                delete static_cast<RowPayload*>(lv_obj_get_user_data(target));
                lv_obj_set_user_data(target, nullptr);
                LVGL_SAFE_EVENT_CB_END();
            },
            LV_EVENT_DELETE, nullptr);
    };

    add_row(lv_tr("Follow active tool"), "auto");
    const auto options =
        build_nozzle_tool_options(owner_.printer_state_.temperature_state().extruders());
    for (const auto& opt : options) {
        add_row(opt.label.c_str(), opt.extruder_name);
    }

    spdlog::debug("[PrintStatusWidget] Nozzle tool picker built with {} nozzle(s)", options.size());
}

std::vector<PrintStatusWidget::NozzleToolOption> PrintStatusWidget::build_nozzle_tool_options(
    const std::unordered_map<std::string, helix::ExtruderInfo>& extruders) {
    std::vector<NozzleToolOption> options;
    options.reserve(extruders.size());
    for (const auto& [name, info] : extruders) {
        // Anything that is not a Klipper extruder object cannot be pinned — the
        // formatter resolves the row's key straight to a per-extruder subject.
        if (!helix::is_extruder_name(name)) {
            continue;
        }
        NozzleToolOption opt;
        opt.extruder_name = name;
        // Use the display name PrinterTemperatureState already assigned —
        // "Nozzle 1", "Nozzle 2", ... — for parity with the temp_graph config
        // modal and the multi-tool nozzle_temps widget.
        if (!info.display_name.empty()) {
            opt.label = info.display_name;
        } else {
            const int index = helix::tool_number_for_extruder(name).value_or(0);
            opt.label = index == 0 ? std::string(lv_tr("Nozzle"))
                                   : std::string(lv_tr("Nozzle")) + " " + std::to_string(index + 1);
        }
        options.push_back(std::move(opt));
    }
    // extruders() is an unordered_map, so impose the printer's own order:
    // "extruder" first, then extruder1, extruder2, ... A plain name sort would
    // put extruder10 ahead of extruder2.
    std::sort(options.begin(), options.end(), [](const auto& a, const auto& b) {
        return helix::tool_number_for_extruder(a.extruder_name).value_or(0) <
               helix::tool_number_for_extruder(b.extruder_name).value_or(0);
    });
    return options;
}

bool PrintStatusWidget::apply_nozzle_tool_override(const std::string& tool_key) {
    const bool accepted = !s_formatter_ || s_formatter_->set_nozzle_tool_override(tool_key);
    // A refused pin leaves the formatter bound to the active extruder, so the
    // widget has to record "auto" too. Writing the rejected name instead left
    // config disagreeing with what is on screen until the next attach() repaired
    // it (see attach()'s set_nozzle_tool_override fallback).
    const std::string effective = accepted ? tool_key : std::string("auto");
    nozzle_tool_override_ = effective;
    config_["nozzle_tool_override"] = effective;
    if (!accepted) {
        spdlog::info("[PrintStatusWidget] nozzle pin '{}' has no matching extruder — kept auto",
                     tool_key);
    }
    return accepted;
}

// ============================================================================
// Static Trampolines
// ============================================================================

static PrintStatusWidget* recover_widget_from_event(lv_event_t* e) {
    // Walk up from the clicked element to find the widget root with user_data
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* obj = target;
    while (obj) {
        auto* self = static_cast<PrintStatusWidget*>(lv_obj_get_user_data(obj));
        if (self) {
            // Validate widget is still alive — non-matching pointers may be
            // other user_data types (e.g., UiButtonData on ui_button children),
            // so keep walking up rather than treating as stale
            if (PrintStatusWidget::live_instances().count(self) != 0) {
                return self;
            }
        }
        obj = lv_obj_get_parent(obj);
    }
    return nullptr;
}

void PrintStatusWidget::print_card_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] print_card_clicked_cb");

    // Defense in depth — even if a child swallows the event with bubble=off,
    // a stray bubble path still routes through here. Skip when the click
    // originated inside the named nozzle click target; the chevron cb
    // handles it. Name must match the lv_obj name in
    // ui_xml/components/print_status_detailed_active.xml.
    constexpr std::string_view NOZZLE_CLICK_TARGET_NAME = "detailed_nozzle_click_target";
    bool from_nozzle_group = false;
    if (auto* target = lv_event_get_target_obj(e)) {
        for (lv_obj_t* o = target; o; o = lv_obj_get_parent(o)) {
            const char* name = lv_obj_get_name(o);
            if (name && std::string_view(name) == NOZZLE_CLICK_TARGET_NAME) {
                from_nozzle_group = true;
                break;
            }
        }
    }

    if (!from_nozzle_group) {
        auto* self = recover_widget_from_event(e);
        if (self) {
            self->record_interaction();
            self->handle_print_card_clicked();
        } else {
            spdlog::warn(
                "[PrintStatusWidget] print_card_clicked_cb: could not recover widget instance");
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::library_files_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] library_files_cb");
    lv_event_stop_bubbling(e);

    auto* self = recover_widget_from_event(e);
    if (self) {
        self->handle_library_files();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::library_last_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] library_last_cb");
    lv_event_stop_bubbling(e);

    auto* self = recover_widget_from_event(e);
    if (self) {
        self->handle_library_last();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::library_recent_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] library_recent_cb");
    lv_event_stop_bubbling(e);

    auto* self = recover_widget_from_event(e);
    if (self) {
        self->handle_library_recent();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::library_queue_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] library_queue_cb");
    lv_event_stop_bubbling(e);

    auto* self = recover_widget_from_event(e);
    if (self) {
        self->handle_library_queue();
    }

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// DetailedFormatter — first-instance singleton lifecycle
// ============================================================================

namespace {
// "Decidegrees" in this codebase is a long-standing misnomer — temperature
// subjects actually store decidegrees (1 unit = 0.1°C; see L021 + the
// helix::units::to_decidegrees implementation in unit_conversions.h). Convert
// to rounded °C by dividing by 10.
int cd_to_c(int cd) {
    if (cd >= 0)
        return (cd + 5) / 10;
    return (cd - 5) / 10;
}
} // namespace

void PrintStatusWidget::DetailedFormatter::update_layer_text() {
    auto& ps = get_printer_state();
    int cur = lv_subject_get_int(ps.get_print_layer_current_subject());
    int tot = lv_subject_get_int(ps.get_print_layer_total_subject());
    std::string text = helix::ui::format_layer_progress(
        cur, tot, ps.layer_is_accurate(), lv_subject_get_int(ps.get_gcode_position_z_subject()));
    snprintf(layer_text_buf_, sizeof(layer_text_buf_), "%s", text.c_str());
    lv_subject_copy_string(&layer_text_subject_, layer_text_buf_);
}

void PrintStatusWidget::DetailedFormatter::update_time_text() {
    auto& ps = get_printer_state();
    int elapsed = lv_subject_get_int(ps.get_print_elapsed_subject());
    int remain = lv_subject_get_int(ps.get_print_time_left_subject());
    int total = elapsed + remain;
    std::string text =
        helix::format::duration_padded(elapsed) + " / " + helix::format::duration_padded(total);
    snprintf(time_text_buf_, sizeof(time_text_buf_), "%s", text.c_str());
    lv_subject_copy_string(&time_text_subject_, time_text_buf_);
}

void PrintStatusWidget::DetailedFormatter::update_filament_text() {
    int used_mm = lv_subject_get_int(get_printer_state().get_print_filament_used_subject());
    if (used_mm <= 0) {
        filament_text_buf_[0] = '\0';
    } else {
        std::string text = std::string(lv_tr("Filament")) + ": " +
                           helix::format::format_filament_length(static_cast<double>(used_mm));
        snprintf(filament_text_buf_, sizeof(filament_text_buf_), "%s", text.c_str());
    }
    lv_subject_copy_string(&filament_text_subject_, filament_text_buf_);

    // Keep the show_filament_active gate honest as filament accumulates.
    // on_size_changed handles the width-band side; this side handles the
    // used-mm transition (e.g., first extrusion of the print).
    int width_band = lv_subject_get_int(&PrintStatusWidget::width_band_subject_);
    int show = (width_band >= 2 && used_mm > 0) ? 1 : 0;
    if (lv_subject_get_int(&PrintStatusWidget::show_filament_active_subject_) != show) {
        lv_subject_set_int(&PrintStatusWidget::show_filament_active_subject_, show);
    }
}

void PrintStatusWidget::DetailedFormatter::update_nozzle_text() {
    auto& ps = get_printer_state();
    lv_subject_t* temp_sub;
    lv_subject_t* tgt_sub;
    if (current_nozzle_override_ == "auto") {
        temp_sub = ps.get_active_extruder_temp_subject();
        tgt_sub = ps.get_active_extruder_target_subject();
    } else {
        temp_sub = ps.get_extruder_temp_subject(current_nozzle_override_);
        tgt_sub = ps.get_extruder_target_subject(current_nozzle_override_);
    }
    int temp_dd = temp_sub ? lv_subject_get_int(temp_sub) : 0;
    int tgt_dd = tgt_sub ? lv_subject_get_int(tgt_sub) : 0;
    // Mirror into proxy subjects (decidegrees) — temp_display in the detailed
    // XML binds to these and gets heating-color rendering for free, including
    // when pinned to a specific tool.
    if (lv_subject_get_int(&nozzle_current_subject_) != temp_dd) {
        lv_subject_set_int(&nozzle_current_subject_, temp_dd);
    }
    if (lv_subject_get_int(&nozzle_target_subject_) != tgt_dd) {
        lv_subject_set_int(&nozzle_target_subject_, tgt_dd);
    }
    // String form kept for the (unused-by-XML but test-asserted) nozzle_text
    // subject, so test_print_status_widget_tool_override.cpp still verifies
    // the pinning + auto-mode dispatch.
    helix::ui::temperature::format_temperature_pair(cd_to_c(temp_dd), cd_to_c(tgt_dd),
                                                    nozzle_text_buf_, sizeof(nozzle_text_buf_));
    lv_subject_copy_string(&nozzle_text_subject_, nozzle_text_buf_);
}

bool PrintStatusWidget::DetailedFormatter::set_nozzle_tool_override(
    const std::string& override_name) {
    using helix::ui::observe_int_sync;
    auto& ps = get_printer_state();

    // Skip rebind when the override hasn't changed — avoids needless
    // observer churn on every layout/state event that funnels through here.
    std::string normalized = (override_name.empty() ? std::string("auto") : override_name);
    if (normalized == current_nozzle_override_ && static_cast<bool>(nozzle_temp_observer_)) {
        return true;
    }

    // [L084] Clear lifetimes BEFORE observers to expire weak_ptr first.
    nozzle_temp_lifetime_.reset();
    nozzle_target_lifetime_.reset();
    // [L085] reset(), never release()
    nozzle_temp_observer_.reset();
    nozzle_target_observer_.reset();

    auto bind_auto = [&]() {
        current_nozzle_override_ = "auto";
        nozzle_temp_observer_ = observe_int_sync<DetailedFormatter>(
            ps.get_active_extruder_temp_subject(), this,
            [](DetailedFormatter* self, int) { self->update_nozzle_text(); },
            ps.get_subjects_lifetime());
        nozzle_target_observer_ = observe_int_sync<DetailedFormatter>(
            ps.get_active_extruder_target_subject(), this,
            [](DetailedFormatter* self, int) { self->update_nozzle_text(); },
            ps.get_subjects_lifetime());
        update_nozzle_text();
        update_tool_label();
    };

    if (override_name.empty() || override_name == "auto") {
        bind_auto();
        return true;
    }

    // Pinned: resolve dynamic per-tool subjects
    auto* temp_sub = ps.get_extruder_temp_subject(override_name, nozzle_temp_lifetime_);
    auto* tgt_sub = ps.get_extruder_target_subject(override_name, nozzle_target_lifetime_);
    if (!temp_sub || !tgt_sub) {
        spdlog::info("[DetailedFormatter] nozzle override '{}' not found, falling back to auto",
                     override_name);
        // Clear any half-bound lifetimes before falling back
        nozzle_temp_lifetime_.reset();
        nozzle_target_lifetime_.reset();
        bind_auto();
        return false;
    }

    current_nozzle_override_ = override_name;
    // Per-extruder subjects are dynamic — the lifetime token must be handed to the
    // observer too, or its guard never learns the subject was deinitialized when
    // PrinterTemperatureState::init_extruders() re-runs on heater rediscovery (#705).
    nozzle_temp_observer_ = observe_int_sync<DetailedFormatter>(
        temp_sub, this, [](DetailedFormatter* self, int) { self->update_nozzle_text(); },
        nozzle_temp_lifetime_);
    nozzle_target_observer_ = observe_int_sync<DetailedFormatter>(
        tgt_sub, this, [](DetailedFormatter* self, int) { self->update_nozzle_text(); },
        nozzle_target_lifetime_);
    update_nozzle_text();
    update_tool_label();
    return true;
}

void PrintStatusWidget::DetailedFormatter::update_multi_tool() {
    // Gated on physical extruders, not tool_count(): set_ams_topology() expands
    // ToolState's tool list to one entry per filament SLOT, so a 4-lane AMS or a
    // 16-wide AD5X tool map reports many "tools" behind a single hotend. Naming
    // which nozzle you are looking at only means something when there is more
    // than one nozzle. Matches the nozzle_icon badge gate in ui_ams_tool_text.
    const bool multi = ToolState::instance().has_multiple_extruders();
    lv_subject_set_int(&PrintStatusWidget::multi_tool_subject_, multi ? 1 : 0);
}

void PrintStatusWidget::DetailedFormatter::update_tool_label() {
    auto& tools = ToolState::instance();
    if (!tools.has_multiple_extruders()) {
        nozzle_tool_label_buf_[0] = '\0';
    } else {
        // Label tracks what the user is VIEWING — the pinned tool when one
        // is set, otherwise the currently active tool. Anything else looks
        // broken right after a pin ("I picked Nozzle 2 but it still says T0").
        int idx = -1;
        // Defend against hand-edited config — the name has to parse as a
        // Klipper extruder AND name an extruder this printer has.
        if (const auto parsed = helix::tool_number_for_extruder(current_nozzle_override_)) {
            if (*parsed < tools.extruder_count()) {
                idx = *parsed;
            }
        }
        if (idx < 0) {
            // "auto", unrecognized, or out-of-range → follow active tool.
            idx = tools.active_tool_index();
        }
        snprintf(nozzle_tool_label_buf_, sizeof(nozzle_tool_label_buf_), "T%d", idx);
    }
    lv_subject_copy_string(&nozzle_tool_label_subject_, nozzle_tool_label_buf_);
}

void PrintStatusWidget::DetailedFormatter::update_idle_fields() {
    auto* hm = get_print_history_manager();
    // The tile's whole job is to offer a reprint, so it describes the newest
    // print that can still be reprinted. When the newest history entry names a
    // file the user has since deleted, that entry is not it - and when nothing
    // survives, the tile presents as never-printed (empty name, disabled
    // Reprint via print_status_idle_has_last).
    const PrintHistoryJob* newest = hm ? hm->get_newest_existing_job() : nullptr;
    if (!newest) {
        lv_subject_copy_string(&idle_filename_subject_, "");
        lv_subject_copy_string(&idle_when_subject_, "Never printed");
        lv_subject_copy_string(&idle_meta_subject_, "");
        lv_subject_set_int(&idle_has_last_subject_, 0);
        return;
    }
    const PrintHistoryJob& job = *newest;
    snprintf(idle_filename_buf_, sizeof(idle_filename_buf_), "%s", job.filename.c_str());
    lv_subject_copy_string(&idle_filename_subject_, idle_filename_buf_);

    double now_s =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    long delta_s = static_cast<long>(now_s - job.end_time);
    // Each branch is a whole sentence with the number as a placeholder. The unit
    // stays inside the key rather than being appended, because a locale may put
    // it before the number or attach a particle to it.
    std::string when;
    if (delta_s < 60) {
        when = lv_tr("Completed just now");
    } else if (delta_s < 3600) {
        when = fmt::format(lv_tr("Completed {}m ago"), delta_s / 60);
    } else if (delta_s < 86400) {
        when = fmt::format(lv_tr("Completed {}h ago"), delta_s / 3600);
    } else {
        when = fmt::format(lv_tr("Completed {}d ago"), delta_s / 86400);
    }
    snprintf(idle_when_buf_, sizeof(idle_when_buf_), "%s", when.c_str());
    lv_subject_copy_string(&idle_when_subject_, idle_when_buf_);

    if (!job.filament_str.empty() && !job.duration_str.empty()) {
        const std::string meta =
            fmt::format(lv_tr("{} filament • {}"), job.filament_str, job.duration_str);
        snprintf(idle_meta_buf_, sizeof(idle_meta_buf_), "%s", meta.c_str());
    } else if (!job.duration_str.empty()) {
        snprintf(idle_meta_buf_, sizeof(idle_meta_buf_), "%s", job.duration_str.c_str());
    } else if (job.total_duration > 0) {
        int d = static_cast<int>(job.total_duration);
        snprintf(idle_meta_buf_, sizeof(idle_meta_buf_), "%dh %02dm", d / 3600, (d % 3600) / 60);
    } else {
        idle_meta_buf_[0] = '\0';
    }
    lv_subject_copy_string(&idle_meta_subject_, idle_meta_buf_);
    lv_subject_set_int(&idle_has_last_subject_, 1);
}

PrintStatusWidget::DetailedFormatter::DetailedFormatter() {
    UI_MANAGED_SUBJECT_STRING(layer_text_subject_, layer_text_buf_, "", "print_status_layer_text",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(time_text_subject_, time_text_buf_, "0h 00m / 0h 00m",
                              "print_status_time_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(filament_text_subject_, filament_text_buf_, "",
                              "print_status_filament_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(nozzle_text_subject_, nozzle_text_buf_, "0 / 0°C",
                              "print_status_nozzle_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(nozzle_tool_label_subject_, nozzle_tool_label_buf_, "",
                              "print_status_nozzle_tool_label", subjects_);
    UI_MANAGED_SUBJECT_INT(nozzle_current_subject_, 0, "print_status_nozzle_current", subjects_);
    UI_MANAGED_SUBJECT_INT(nozzle_target_subject_, 0, "print_status_nozzle_target", subjects_);
    UI_MANAGED_SUBJECT_STRING(idle_filename_subject_, idle_filename_buf_, "",
                              "print_status_idle_filename", subjects_);
    UI_MANAGED_SUBJECT_STRING(idle_when_subject_, idle_when_buf_, "Never printed",
                              "print_status_idle_when", subjects_);
    UI_MANAGED_SUBJECT_STRING(idle_meta_subject_, idle_meta_buf_, "", "print_status_idle_meta",
                              subjects_);
    UI_MANAGED_SUBJECT_INT(idle_has_last_subject_, 0, "print_status_idle_has_last", subjects_);

    // Tear down formatter-owned subjects + observers under lv_deinit() (while
    // spdlog and PrintHistoryManager are still alive). ~DetailedFormatter runs
    // at C++ atexit, which is too late: spdlog sinks may be gone (UAF in log
    // calls) and helix-xml's scope is destroyed, so deinit_all() at atexit is
    // a no-op but the dangling pointer window between lv_deinit and atexit is
    // closed by this earlier teardown. Safe to fire multiple times since
    // subjects_.deinit_all() and observer .reset() are idempotent.
    StaticSubjectRegistry::instance().register_deinit("PrintStatusWidgetDetailedFormatter", []() {
        if (!s_formatter_)
            return;
        if (auto* hm = get_print_history_manager()) {
            hm->remove_observer(&s_formatter_->history_cb_);
        }
        s_formatter_->layer_current_observer_.reset();
        s_formatter_->layer_total_observer_.reset();
        s_formatter_->elapsed_observer_.reset();
        s_formatter_->time_left_observer_.reset();
        s_formatter_->filament_used_observer_.reset();
        s_formatter_->nozzle_temp_observer_.reset();
        s_formatter_->nozzle_target_observer_.reset();
        s_formatter_->tools_version_observer_.reset();
        s_formatter_->active_tool_observer_.reset();
        s_formatter_->arc_value_observer_.reset();
        s_formatter_->nozzle_temp_lifetime_.reset();
        s_formatter_->nozzle_target_lifetime_.reset();
        s_formatter_->subjects_.deinit_all();
    });

    using helix::ui::observe_int_sync;
    auto& ps = get_printer_state();
    layer_current_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_layer_current_subject(), this,
        [](DetailedFormatter* self, int) { self->update_layer_text(); },
        ps.get_subjects_lifetime());
    layer_total_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_layer_total_subject(), this,
        [](DetailedFormatter* self, int) { self->update_layer_text(); },
        ps.get_subjects_lifetime());
    elapsed_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_elapsed_subject(), this,
        [](DetailedFormatter* self, int) { self->update_time_text(); }, ps.get_subjects_lifetime());
    time_left_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_time_left_subject(), this,
        [](DetailedFormatter* self, int) { self->update_time_text(); }, ps.get_subjects_lifetime());
    filament_used_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_filament_used_subject(), this,
        [](DetailedFormatter* self, int) { self->update_filament_text(); },
        ps.get_subjects_lifetime());

    // Auto-tool nozzle: the active_extruder subjects are static members of
    // PrinterState, but its lifetime token still has to be handed over — the
    // guard is what learns the subjects died when PrinterState deinits, instead
    // of leaving that to StaticSubjectRegistry ordering.
    nozzle_temp_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_active_extruder_temp_subject(), this,
        [](DetailedFormatter* self, int) { self->update_nozzle_text(); },
        ps.get_subjects_lifetime());
    nozzle_target_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_active_extruder_target_subject(), this,
        [](DetailedFormatter* self, int) { self->update_nozzle_text(); },
        ps.get_subjects_lifetime());
    // Bed and chamber temp_display widgets bind directly to bed_temp / bed_target /
    // chamber_temp / chamber_target in the XML — no formatter mirroring needed
    // for those. The nozzle path still mirrors because pinning rebinds it.

    // Seed initial values from current subject state
    update_layer_text();
    update_time_text();
    update_filament_text();
    update_nozzle_text();

    // Multi-extruder: observe the tool-list version + active_tool to drive the
    // gate and the T<n> label. tools_version bumps on every tool-list rebuild,
    // including the ones that leave the count alone.
    tools_version_observer_ = observe_int_sync<DetailedFormatter>(
        ToolState::instance().get_tools_version_subject(), this,
        [](DetailedFormatter* self, int) {
            self->update_multi_tool();
            self->update_tool_label();
        },
        ToolState::instance().get_subjects_lifetime());
    active_tool_observer_ = observe_int_sync<DetailedFormatter>(
        ToolState::instance().get_active_tool_subject(), this,
        [](DetailedFormatter* self, int) { self->update_tool_label(); },
        ToolState::instance().get_subjects_lifetime());
    update_multi_tool();
    update_tool_label();

    // Arc value observer — keeps lv_arc value in sync with print progress.
    // arc_widget_ is nulled by an LV_EVENT_DELETE callback registered in
    // attach_arc(), so a non-null pointer here is always live (L075: no
    // lv_obj_is_valid in observer cbs).
    arc_value_observer_ = observe_int_sync<DetailedFormatter>(
        ps.get_print_progress_subject(), this,
        [](DetailedFormatter* self, int pct) {
            if (self->arc_widget_) {
                lv_arc_set_value(self->arc_widget_, pct);
            }
        },
        ps.get_subjects_lifetime());

    // Idle hero — populate from print history and refresh on history-changed notifications.
    // PrintHistoryManager fires observers on the main thread (defer-wrapped in on_history_fetched),
    // so direct lv_subject_* writes here are safe; no AsyncLifetimeGuard needed.
    history_cb_ = [this]() { update_idle_fields(); };
    if (auto* hm = get_print_history_manager()) {
        hm->add_observer(&history_cb_);
        // ensure_loaded(), not fetch(): the response already in flight serves
        // this caller through the observer above, and fetch() would read the
        // ask as an invalidation and queue a second identical request.
        hm->ensure_loaded(HistoryScope::RECENT);
    }
    update_idle_fields();

    spdlog::debug("[DetailedFormatter] subjects initialized");
}

PrintStatusWidget::DetailedFormatter::~DetailedFormatter() {
    // Two destruction paths:
    //   1. Production: never destructs at runtime (s_formatter_ lives forever).
    //      At C++ atexit the dtor runs after spdlog teardown — so no spdlog
    //      calls here. By that point register_deinit has already torn things
    //      down under lv_deinit; the calls below are safe idempotent no-ops.
    //   2. Tests: release_formatter_for_test() destroys the formatter between
    //      runs to keep subjects from leaking across PrinterState resets.
    //      In this path lv_deinit has NOT fired, so register_deinit has NOT
    //      run; the cleanup below is the only thing that detaches observers
    //      before subjects get reset by the next test.
    if (auto* hm = get_print_history_manager()) {
        hm->remove_observer(&history_cb_);
    }
    layer_current_observer_.reset();
    layer_total_observer_.reset();
    elapsed_observer_.reset();
    time_left_observer_.reset();
    filament_used_observer_.reset();
    nozzle_temp_observer_.reset();
    nozzle_target_observer_.reset();
    tools_version_observer_.reset();
    active_tool_observer_.reset();
    arc_value_observer_.reset();
    nozzle_temp_lifetime_.reset();
    nozzle_target_lifetime_.reset();
    subjects_.deinit_all();
}

void PrintStatusWidget::DetailedFormatter::attach_arc(lv_obj_t* arc) {
    arc_widget_ = arc;
    if (arc) {
        // Range + angles + styling come from helix_progress_arc; just seed the
        // initial value.
        int pct = lv_subject_get_int(get_printer_state().get_print_progress_subject());
        lv_arc_set_value(arc, pct);
        // Null arc_widget_ when LVGL destroys the arc — lets the progress
        // observer null-check without lv_obj_is_valid (L075). Guard against
        // the layout-rebuild race: the home panel attaches widget A → detaches A
        // → attaches B in quick succession; A's deferred LV_EVENT_DELETE fires
        // AFTER B has already overwritten arc_widget_ with its own arc. An
        // unconditional null here clobbers B's live arc and leaves the
        // progress observer with no widget to update — the arc renders the
        // grey track only, forever. Only clear when the deleted object is
        // still the one we're tracking.
        lv_obj_add_event_cb(
            arc,
            [](lv_event_t* e) {
                if (!s_formatter_)
                    return;
                lv_obj_t* deleted = lv_event_get_target_obj(e);
                if (s_formatter_->arc_widget_ == deleted) {
                    s_formatter_->arc_widget_ = nullptr;
                }
            },
            LV_EVENT_DELETE, nullptr);
        // Auto-resize + diameter-driven thickness via the shared helper.
        // It hooks LV_EVENT_SIZE_CHANGED on the parent and publishes the
        // tier to our class-level subject, which the XML bind_styles
        // (in helix_progress_arc.xml) react to.
        helix::ui::attach_progress_arc(arc, lv_obj_get_parent(arc),
                                       &PrintStatusWidget::arc_thickness_tier_subject_);
    }
}

void PrintStatusWidget::DetailedFormatter::resize_arc() {
    // Outer grid relayouts may not propagate SIZE_CHANGED to the arc's
    // direct parent immediately. Force a refresh through the shared helper.
    helix::ui::refresh_progress_arc(arc_widget_);
}

// ============================================================================
// Nozzle Chevron Callback
// ============================================================================

void PrintStatusWidget::print_status_nozzle_chevron_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("print_status_nozzle_chevron_cb");
    auto* anchor = lv_event_get_current_target_obj(e);
    if (!anchor)
        return;
    // Walk up to find the owning widget instance
    PrintStatusWidget* owner = nullptr;
    for (lv_obj_t* o = anchor; o; o = lv_obj_get_parent(o)) {
        auto* candidate = static_cast<PrintStatusWidget*>(lv_obj_get_user_data(o));
        if (candidate && live_instances().count(candidate)) {
            owner = candidate;
            break;
        }
    }
    if (owner)
        owner->show_nozzle_tool_picker(anchor);
    LVGL_SAFE_EVENT_CB_END();
}

namespace {
void open_temp_graph_for(lv_event_t* e, TempGraphOverlay::Mode mode) {
    // Stop bubbling so the click doesn't also trigger print_card_clicked_cb
    // on the surrounding card and navigate away.
    lv_event_stop_bubbling(e);
    auto* owner = recover_widget_from_event(e);
    if (!owner)
        return;
    lv_obj_t* parent_screen = owner->get_parent_screen();
    if (!parent_screen)
        parent_screen = lv_screen_active();
    get_global_temp_graph_overlay().open(mode, parent_screen);
}
} // namespace

void PrintStatusWidget::on_print_status_nozzle_temp_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("on_print_status_nozzle_temp_clicked");
    open_temp_graph_for(e, TempGraphOverlay::Mode::Nozzle);
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::on_print_status_bed_temp_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("on_print_status_bed_temp_clicked");
    open_temp_graph_for(e, TempGraphOverlay::Mode::Bed);
    LVGL_SAFE_EVENT_CB_END();
}

void PrintStatusWidget::on_print_status_chamber_temp_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("on_print_status_chamber_temp_clicked");
    open_temp_graph_for(e, TempGraphOverlay::Mode::Chamber);
    LVGL_SAFE_EVENT_CB_END();
}
