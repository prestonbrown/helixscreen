// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_edit_overlay.h"

#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_hsv_picker.h"
#include "ui_nav_manager.h"
#include "ui_swatch.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_state.h"
#include "app_globals.h"
#include "color_utils.h"
#include "filament_database.h"
#include "filament_display_name.h"
#include "filament_mapper.h"
#include "format_utils.h"
#include "spoolman_manager.h"
#include "static_panel_registry.h"
#include "ui/ui_lazy_panel_helper.h"
#if HELIX_HAS_LABEL_PRINTER
#include "ipp_print_modal.h"
#include "label_printer_settings.h"
#include "label_printer_utils.h"
#endif
#include "ui_breakpoint.h"
#include "ui_overlay_qr_scanner.h"
#include "ui_toast_manager.h"

#include "i_moonraker_api.h"
#include "printer_state.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace helix::ui {

// Static member initialization
bool AmsEditOverlay::callbacks_registered_ = false;

// Fire-and-forget: notify Moonraker of the active spool so other clients
// (Mainsail, Fluidd) see the change and filament tracking works.
// Pass 0 to clear the active spool (unlink).
static void sync_active_spool(IMoonrakerAPI* api, int spool_id) {
    spdlog::info("[AmsEditOverlay] Syncing active spool to {} on server", spool_id);
    api->spoolman().set_active_spool(
        spool_id,
        [spool_id]() {
            spdlog::debug("[AmsEditOverlay] Active spool synced to {} on server", spool_id);
        },
        [spool_id](const MoonrakerError& err) {
            spdlog::warn("[AmsEditOverlay] Failed to sync active spool to {}: {}", spool_id,
                         err.message);
        });
}

// ============================================================================
// Construction / Destruction
// ============================================================================

namespace {
std::unique_ptr<AmsEditOverlay> g_ams_edit_overlay;
} // namespace

AmsEditOverlay& get_ams_edit_overlay() {
    if (!g_ams_edit_overlay) {
        g_ams_edit_overlay = std::make_unique<AmsEditOverlay>();
        StaticPanelRegistry::instance().register_destroy("AmsEditOverlay",
                                                         []() { g_ams_edit_overlay.reset(); });
    }
    return *g_ams_edit_overlay;
}

AmsEditOverlay::AmsEditOverlay() {
    spdlog::debug("[AmsEditOverlay] Constructed");
}

AmsEditOverlay::~AmsEditOverlay() {
    // Deinitialize subjects first to disconnect observers [L041]
    deinit_subjects();
    spdlog::trace("[AmsEditOverlay] Destroyed");
}

lv_obj_t* AmsEditOverlay::find_widget(const char* name) const {
    return overlay_root_ ? lv_obj_find_by_name(overlay_root_, name) : nullptr;
}

// ============================================================================
// Public API
// ============================================================================

bool AmsEditOverlay::show_for_slot(lv_obj_t* parent, int slot_index, const SlotInfo& initial_info,
                                   IMoonrakerAPI* api, CompletionCallback on_complete,
                                   bool open_on_picker) {
    // A previous widget tree may have died with its screen (display rebuild,
    // test teardown) without the destroy-on-close path running — drop the
    // stale cache so lazy_create_and_push_overlay rebuilds from XML.
    if (cached_overlay_widget_ && !lv_obj_is_valid(cached_overlay_widget_)) {
        spdlog::debug("[AmsEditOverlay] Cached overlay widget is stale - rebuilding");
        overlay_root_ = nullptr;
        on_ui_destroyed();
    }

    // Store per-invocation state (QrScannerOverlay pattern: params + callback
    // stored on the singleton before push)
    slot_index_ = slot_index;
    original_info_ = initial_info;
    working_info_ = initial_info;
    save_to_spoolman_opt_in_ = false; // explicit toggle decides Spoolman writes
    // Picker-entry mode: a selection commits + closes (task #13). Cleared when
    // the picker is later re-entered via Change Filament (switch_to_picker).
    opened_on_picker_ = open_on_picker;
    api_ = api ? api : get_moonraker_api();
    completion_callback_ = std::move(on_complete);
    completion_fired_ = false;
    cached_spools_.clear();

    // Always prefer the active screen so the overlay renders above everything
    lv_obj_t* screen = lv_screen_active();
    bool ok = lazy_create_and_push_overlay<AmsEditOverlay>(
        get_ams_edit_overlay, cached_overlay_widget_, screen ? screen : parent, "AMS Slot Editor",
        "AmsEditOverlay");
    if (!ok) {
        spdlog::error("[AmsEditOverlay] Failed to push overlay for slot {}", slot_index);
        return false;
    }

    // Safety net + memory reclaim on close. A dismissal that bypasses our
    // handlers (backdrop tap, external go_back) must still complete as "not
    // saved" — fire_completion is idempotent, so Save/back paths that already
    // fired are unaffected. We ALSO tear the widget tree down here: this overlay
    // is large (~180 widgets — catalog selector + 60 preset swatches + product
    // list + logistics fields + color view) and is opened only to edit a spool,
    // so keeping it resident for the whole app lifetime wastes memory on 111MB
    // devices (CC1, AD5M). Subjects and overlay state survive; the next open
    // rebuilds via the stale-cache path above + lazy_create_and_push_overlay.
    // It has to be ONE combined callback: NavigationManager keeps a single close
    // callback per widget, so a separate destroy_on_close registration would
    // just overwrite this one (or be overwritten by it).
    NavigationManager::instance().register_overlay_close_callback(cached_overlay_widget_, []() {
        auto& overlay = get_ams_edit_overlay();
        overlay.fire_completion(false);
        overlay.destroy_overlay_ui(overlay.cached_overlay_widget_);
    });

    // Reset per-session view state HERE (covered-safe — on_deactivate must not
    // touch it, since it also fires when the QR scanner merely covers us).
    set_view(open_on_picker ? VIEW_SPOOL_PICKER : VIEW_OVERVIEW);
    if (open_on_picker) {
        populate_picker();
    }

    // If linked to Spoolman, fetch authoritative filament data (vendor, material, color)
    // so the form shows current Spoolman state, not stale backend data. Gate on
    // Spoolman availability: a slot can carry a stale spoolman_id even when
    // Spoolman itself is unavailable, and firing the fetch on a Spoolman-less
    // printer is one doomed RPC + warn log per overlay open (mirrors the
    // enter_spool_edit() gating).
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    if (has_spoolman && working_info_.spoolman_id > 0 && api_) {
        const int spool_id = working_info_.spoolman_id;
        auto token = lifetime_.token();
        api_->spoolman().get_spoolman_spool(
            spool_id,
            [this, token, spool_id](const std::optional<SpoolInfo>& spool) {
                // No bare token.expired() here: that is the L081 Mechanism C
                // TOCTOU shape (checked on a bg thread, acted on afterwards) and
                // it is what the bg_tok_expired_check telemetry recorded on
                // editor open. token.defer() below already gates on liveness.
                if (!spool)
                    return;
                // Capture Spoolman's authoritative data for the spool
                int fetched_filament_id = spool->filament_id;
                int fetched_vendor_id = spool->vendor_id;
                std::string fetched_vendor = spool->vendor;
                std::string fetched_material = spool->material;
                std::string fetched_color_hex = spool->color_hex;
                token.defer([this, spool_id, fetched_filament_id, fetched_vendor_id,
                             fetched_vendor = std::move(fetched_vendor),
                             fetched_material = std::move(fetched_material),
                             fetched_color_hex = std::move(fetched_color_hex)]() {
                    if (fetched_filament_id > 0) {
                        original_info_.spoolman_filament_id = fetched_filament_id;
                        working_info_.spoolman_filament_id = fetched_filament_id;
                    }
                    if (fetched_vendor_id > 0) {
                        original_info_.spoolman_vendor_id = fetched_vendor_id;
                        working_info_.spoolman_vendor_id = fetched_vendor_id;
                    }
                    // Update brand/material from Spoolman (authoritative source)
                    if (!fetched_vendor.empty() && working_info_.brand != fetched_vendor) {
                        spdlog::debug("[AmsEditOverlay] Updating vendor from '{}' to '{}' "
                                      "(Spoolman spool {})",
                                      working_info_.brand, fetched_vendor, spool_id);
                        original_info_.brand = fetched_vendor;
                        working_info_.brand = fetched_vendor;
                    }
                    if (!fetched_material.empty() && working_info_.material != fetched_material) {
                        spdlog::debug("[AmsEditOverlay] Updating material from '{}' to '{}' "
                                      "(Spoolman spool {})",
                                      working_info_.material, fetched_material, spool_id);
                        original_info_.material = fetched_material;
                        working_info_.material = fetched_material;
                    }
                    if (!fetched_color_hex.empty()) {
                        uint32_t rgb = 0;
                        if (helix::parse_hex_color(fetched_color_hex.c_str(), rgb) &&
                            working_info_.color_rgb != rgb) {
                            spdlog::debug("[AmsEditOverlay] Updating color from {:#08x} to {:#08x} "
                                          "(Spoolman spool {})",
                                          working_info_.color_rgb, rgb, spool_id);
                            original_info_.color_rgb = rgb;
                            working_info_.color_rgb = rgb;
                        }
                    }
                    spdlog::debug("[AmsEditOverlay] Synced spool {} from Spoolman: vendor='{}', "
                                  "material='{}', filament_id={}, vendor_id={}",
                                  spool_id, working_info_.brand, working_info_.material,
                                  fetched_filament_id, fetched_vendor_id);
                    update_ui();
                });
            },
            [spool_id](const MoonrakerError& err) {
                spdlog::warn("[AmsEditOverlay] Failed to fetch spool {}: {}", spool_id,
                             err.message);
            });
    }

    spdlog::info("[AmsEditOverlay] Shown for slot {} (spoolman_id={}, brand={}, material={})",
                 slot_index, initial_info.spoolman_id, initial_info.brand, initial_info.material);
    return true;
}

// ============================================================================
// OverlayBase Hooks
// ============================================================================

lv_obj_t* AmsEditOverlay::create(lv_obj_t* parent) {
    if (!create_overlay_from_xml(parent, "ams_edit_overlay")) {
        return nullptr;
    }

    // Bind labels to subjects ONCE per widget tree (the tree is cached across
    // opens — binding in on_activate would stack duplicate observers).
    // Header title comes from the shared header_bar ("header_title").
    // The bindings are owned by their labels and torn down with the widget
    // tree; no observer handles are retained (LVGL cleans them on delete).
    lv_obj_t* header_title = find_widget("header_title");
    if (header_title) {
        lv_label_bind_text(header_title, &slot_indicator_subject_, nullptr);
    }

    // header_bar's optional trailing badge. The image and the group's visibility
    // are wired from XML; only the label text is bound here, so the shared
    // component needs no bind_text of its own (an unset one warns on every
    // header without a badge). Tracked slots therefore show the Spoolman mark
    // and spool number together, on every view of the editor.
    lv_obj_t* badge_text = find_widget("header_title_badge_text");
    if (badge_text) {
        lv_label_bind_text(badge_text, &spoolman_id_subject_, nullptr);
    }

    lv_obj_t* temp_nozzle_label = find_widget("temp_nozzle_label");
    if (temp_nozzle_label) {
        lv_label_bind_text(temp_nozzle_label, &temp_nozzle_subject_, nullptr);
    }

    lv_obj_t* temp_bed_label = find_widget("temp_bed_label");
    if (temp_bed_label) {
        lv_label_bind_text(temp_bed_label, &temp_bed_subject_, nullptr);
    }

    lv_obj_t* remaining_pct_label = find_widget("remaining_pct_label");
    if (remaining_pct_label) {
        lv_label_bind_text(remaining_pct_label, &remaining_pct_subject_, nullptr);
    }

    lv_obj_t* card_identity_label = find_widget("card_identity_label");
    if (card_identity_label) {
        lv_label_bind_text(card_identity_label, &chip_text_subject_, nullptr);
    }
    lv_obj_t* hsv = find_widget("ams_color_hsv");
    if (hsv) {
        ui_hsv_picker_set_callback(
            hsv,
            [](uint32_t rgb, void* /*user_data*/) {
                get_ams_edit_overlay().handle_custom_color_changed(rgb);
            },
            nullptr);
    }

    spdlog::info("[AmsEditOverlay] Overlay created");
    return overlay_root_;
}

void AmsEditOverlay::on_ui_destroyed() {
    // The catalog fragment lives in the destroyed tree: detach so the
    // selector's static registry drops its (now-dangling) widget key. Without
    // this, the singleton's destructor erases from the registry map during
    // static destruction — after the map itself is gone — corrupting the heap
    // at process exit (and leaving a stale key at runtime).
    details_selector_.detach();
    cached_overlay_widget_ = nullptr;
}

void AmsEditOverlay::on_activate() {
    OverlayBase::on_activate();

    // Refresh the UI with current slot data
    update_ui();
    update_sync_button_state();
    update_spoolman_button_state();
}

void AmsEditOverlay::on_deactivate() {
    // Fires when POPPED **and** when COVERED (e.g. QR scanner pushed on top).
    // Must NOT fire completion or reset the view subject — session state
    // resets happen in show_for_slot(). Base invalidates lifetime_ (pending
    // Spoolman fetches for this activation are dropped; token() re-arms).
    OverlayBase::on_deactivate();
    spdlog::debug("[AmsEditOverlay] on_deactivate()");
}

// ============================================================================
// Subject Management
// ============================================================================

void AmsEditOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        // Initialize string subjects with empty/default buffers (bound in
        // create(), not XML-registered)
        slot_indicator_buf_[0] = '-';
        slot_indicator_buf_[1] = '-';
        slot_indicator_buf_[2] = '\0';
        temp_nozzle_buf_[0] = '\0';
        temp_bed_buf_[0] = '\0';
        snprintf(remaining_pct_buf_, sizeof(remaining_pct_buf_), "\xE2\x80\x94"); // "—"

        lv_subject_init_string(&slot_indicator_subject_, slot_indicator_buf_, nullptr,
                               sizeof(slot_indicator_buf_), "--");
        subjects_.register_subject(&slot_indicator_subject_);

        lv_subject_init_string(&temp_nozzle_subject_, temp_nozzle_buf_, nullptr,
                               sizeof(temp_nozzle_buf_), "");
        subjects_.register_subject(&temp_nozzle_subject_);

        lv_subject_init_string(&temp_bed_subject_, temp_bed_buf_, nullptr, sizeof(temp_bed_buf_),
                               "");
        subjects_.register_subject(&temp_bed_subject_);

        lv_subject_init_string(&remaining_pct_subject_, remaining_pct_buf_, nullptr,
                               sizeof(remaining_pct_buf_), "\xE2\x80\x94");
        subjects_.register_subject(&remaining_pct_subject_);

        // View state (VIEW_OVERVIEW..VIEW_COLOR) - registered globally
        UI_MANAGED_SUBJECT_INT(view_mode_subject_, 0, "ams_edit_view", subjects_);

        // Picker state (0=loading, 1=empty, 2=content) - registered globally
        UI_MANAGED_SUBJECT_INT(picker_state_subject_, 0, "edit_picker_state", subjects_);

        // Header Save button dirty gate (1=disabled). Starts disabled — nothing
        // is dirty when the editor opens.
        UI_MANAGED_SUBJECT_INT(save_disabled_subject_, 1, "ams_edit_save_disabled", subjects_);

        // Header Save button visibility gate (1=hidden). Save only applies to
        // the overview form; non-overview views (picker, details, color) hide
        // it entirely. Written exclusively from set_view().
        UI_MANAGED_SUBJECT_INT(save_hidden_subject_, 0, "ams_edit_save_hidden", subjects_);

        // Managed-vs-untracked signal: drives the Spoolman mark, the Spool
        // details row, and (Phase 5) the Save-to-Spoolman toggle default.
        UI_MANAGED_SUBJECT_INT(is_managed_subject_, 0, "ams_edit_is_managed", subjects_);

        chip_text_buf_[0] = '\0';
        lv_subject_init_string(&chip_text_subject_, chip_text_buf_, nullptr, sizeof(chip_text_buf_),
                               "");
        subjects_.register_subject(&chip_text_subject_);

        // Spoolman spool number shown beside the tracked mark on the overview
        // card ("#19"). Named so the label binds via bind_text in XML rather than
        // an imperative lv_label_set_text from update_ui().
        UI_MANAGED_SUBJECT_STRING(spoolman_id_subject_, spoolman_id_buf_, "",
                                  "ams_edit_spoolman_id", subjects_);

#if HELIX_HAS_LABEL_PRINTER
        // Expose the label-printer readiness flag to XML so
        // btn_detail_print_label can bind its `hidden` flag declaratively and
        // track pairing/unpairing while the overlay is open. The subject is
        // owned by LabelPrinterSettingsManager (NOT registered into subjects_ —
        // it must outlive this overlay). Builds without HELIX_HAS_LABEL_PRINTER
        // leave the subject unregistered; the XML binding then never installs
        // and the button keeps its static hidden="true".
        helix::LabelPrinterSettingsManager::instance().init_subjects();
        lv_xml_register_subject(
            nullptr, "label_printer_configured",
            helix::LabelPrinterSettingsManager::instance().subject_printer_configured());
#endif
    });
}

void AmsEditOverlay::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

// ============================================================================
// View Switching
// ============================================================================

void AmsEditOverlay::set_view(int view) {
    lv_subject_set_int(&view_mode_subject_, view);
    // Header Save is visible on the overview AND the spool-edit view (where it
    // finishes the whole edit and closes). The picker and color views hide it.
    lv_subject_set_int(&save_hidden_subject_,
                       (view == VIEW_OVERVIEW || view == VIEW_SPOOL_EDIT) ? 0 : 1);
    // Refresh the enabled/disabled gate for the new view: spool-edit forces it
    // enabled (edits live in widgets, not yet staged); the overview keeps the
    // is_dirty() gating. update_sync_button_state() reads the view subject we
    // just wrote, so this stays the sole writer of save_disabled_subject_.
    update_sync_button_state();
}

void AmsEditOverlay::switch_to_picker() {
    if (!subjects_initialized_) {
        spdlog::warn("[AmsEditOverlay] switch_to_picker() aborted: subjects not initialized");
        return;
    }
    spdlog::debug("[AmsEditOverlay] Switching to picker view (overlay_root_={}, api_={})",
                  static_cast<void*>(overlay_root_), static_cast<void*>(api_));
    // Reaching the picker via Change Filament is the normal two-step flow — a
    // selection returns to the overview for review, so drop the picker-entry
    // one-tap-commit shortcut (task #13).
    opened_on_picker_ = false;
    set_view(VIEW_SPOOL_PICKER);
    populate_picker();
}

void AmsEditOverlay::switch_to_form() {
    if (!subjects_initialized_) {
        return;
    }
    set_view(VIEW_OVERVIEW);
    spdlog::debug("[AmsEditOverlay] Switched to form view");
}

void AmsEditOverlay::populate_picker() {
    // Resolve API: prefer stored api_, fall back to global (matches SpoolmanPanel pattern)
    if (!api_) {
        api_ = get_moonraker_api();
    }
    if (!overlay_root_ || !api_) {
        spdlog::warn("[AmsEditOverlay] populate_picker() aborted: overlay_root_={}, api_={}",
                     static_cast<void*>(overlay_root_), static_cast<void*>(api_));
        lv_subject_set_int(&picker_state_subject_, 1);
        return;
    }

    // Show loading state
    lv_subject_set_int(&picker_state_subject_, 0);

    // Clear search input
    lv_obj_t* search = find_widget("picker_search");
    if (search) {
        lv_textarea_set_text(search, "");
    }

    auto token = lifetime_.token();

    spdlog::debug("[AmsEditOverlay] populate_picker() fetching spools from Spoolman...");

    api_->spoolman().get_spoolman_spools(
        [this, token](const std::vector<SpoolInfo>& spools) {
            // Liveness is token.defer()'s job — a bare expired() check here is
            // the L081 Mechanism C anti-pattern.
            spdlog::debug("[AmsEditOverlay] Spoolman returned {} spools", spools.size());
            token.defer([this, spools]() {
                if (!overlay_root_) {
                    spdlog::warn(
                        "[AmsEditOverlay] populate_picker callback dropped: overlay_root_ null");
                    return;
                }
                if (!subjects_initialized_) {
                    spdlog::warn("[AmsEditOverlay] populate_picker callback dropped: subjects not "
                                 "initialized");
                    return;
                }

                if (spools.empty()) {
                    spdlog::debug("[AmsEditOverlay] Spoolman returned empty spool list");
                    lv_subject_set_int(&picker_state_subject_, 1);
                    return;
                }

                // Spools arrive ordered by most recent activity first, where a
                // spool's rank is max(last_used, registered) — sort_spools_by_recency()
                // is applied once in the API layer on fetch (#1071). filter_spools()
                // preserves order, so filtering doesn't need to re-sort.
                cached_spools_ = spools;
                render_spool_list("");
            });
        },
        [this, token](const MoonrakerError& err) {
            spdlog::warn("[AmsEditOverlay] Spoolman fetch error: {}", err.message);
            token.defer([this, msg = err.message]() {
                if (!overlay_root_ || !subjects_initialized_) {
                    spdlog::warn("[AmsEditOverlay] Error callback dropped: overlay_root_={}, "
                                 "subjects={}",
                                 static_cast<void*>(overlay_root_), subjects_initialized_);
                    return;
                }
                spdlog::warn("[AmsEditOverlay] Failed to fetch spools: {}", msg);
                lv_subject_set_int(&picker_state_subject_, 1);
            });
        });
}

void AmsEditOverlay::render_spool_list(const std::string& filter) {
    lv_obj_t* spool_list = find_widget("picker_spool_list");
    if (!spool_list) {
        return;
    }

    // Invoked from a token.defer() callback (UpdateQueue batch). Sync
    // lv_obj_clean in that context corrupts LVGL's event linked list (#776).
    helix::ui::safe_clean_children(spool_list);

    // Reuse shared filter_spools() from spoolman_types
    auto filtered = filter_spools(cached_spools_, filter);

    // Get spool IDs assigned to other tools (exclude current slot's tool)
    auto in_use = ToolState::instance().assigned_spool_ids(slot_index_);

    // Compact single-line rows on short panels (more rows visible)
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint bp = bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    const bool is_compact = bp <= UiBreakpoint::Medium;
    const char* attrs_plain[] = {"compact",     is_compact ? "true" : "false",
                                 "detail_flow", is_compact ? "row" : "column",
                                 nullptr,       nullptr};
    const char* attrs_current[] = {"compact",
                                   is_compact ? "true" : "false",
                                   "detail_flow",
                                   is_compact ? "row" : "column",
                                   "hide_edit_pencil",
                                   "false",
                                   nullptr,
                                   nullptr};

    // Pre-selection (spec §3.2, resolution §2.5): the current spool if linked,
    // otherwise the FIRST selectable row — so a single-candidate list is one
    // tap. Purely visual (LV_STATE_CHECKED); tapping a row selects it.
    bool have_preselection = false;

    for (const auto& spool : filtered) {
        const bool is_current_spool =
            (working_info_.spoolman_id > 0 && spool.id == working_info_.spoolman_id);
        lv_obj_t* item = static_cast<lv_obj_t*>(lv_xml_create(
            spool_list, "spoolman_spool_item", is_current_spool ? attrs_current : attrs_plain));
        if (!item) {
            continue;
        }

        lv_obj_set_user_data(item, reinterpret_cast<void*>(static_cast<intptr_t>(spool.id)));

        lv_obj_t* name_label = lv_obj_find_by_name(item, "spool_name");
        if (name_label) {
            std::string name = "#" + std::to_string(spool.id) + " ";
            name += spool.vendor.empty() ? spool.material : (spool.vendor + " " + spool.material);
            lv_label_set_text(name_label, name.c_str());
        }

        // The XML widget kept its "spool_color" name; the string is Spoolman's
        // filament.name, which is the only per-spool label Spoolman stores.
        lv_obj_t* color_label = lv_obj_find_by_name(item, "spool_color");
        if (color_label && !spool.filament_name.empty()) {
            lv_label_set_text(color_label, spool.filament_name.c_str());
        }

        lv_obj_t* weight_label = lv_obj_find_by_name(item, "spool_weight");
        if (weight_label && spool.remaining_weight_g > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0fg", spool.remaining_weight_g);
            lv_label_set_text(weight_label, buf);
        }

        lv_obj_t* swatch = lv_obj_find_by_name(item, "spool_swatch");
        if (swatch) {
            uint32_t rgb = helix::parse_hex_color(spool.color_hex).value_or(0x808080);
            helix::ui::apply_swatch_color(swatch, rgb, spool.multi_color_hexes);
        }

        // Disable spools already assigned to other tools
        const bool disabled = in_use.count(spool.id) > 0;
        if (disabled) {
            lv_obj_add_state(item, LV_STATE_DISABLED);
            lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE);
        }

        // Selection highlight: linked spool wins; else first selectable row.
        bool is_selected = false;
        if (working_info_.spoolman_id > 0) {
            is_selected = (spool.id == working_info_.spoolman_id);
        } else if (!have_preselection && !disabled) {
            is_selected = true;
        }
        if (is_selected) {
            have_preselection = true;
            lv_obj_set_state(item, LV_STATE_CHECKED, true);
            lv_obj_t* check_icon = lv_obj_find_by_name(item, "selected_icon");
            if (check_icon) {
                lv_obj_remove_flag(check_icon, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_set_state(item, LV_STATE_CHECKED, false);
        }
    }

    lv_subject_set_int(&picker_state_subject_, filtered.empty() ? 1 : 2);
    spdlog::debug("[AmsEditOverlay] Rendered {} spool items (filter='{}')", filtered.size(),
                  filter);
}

void AmsEditOverlay::handle_spool_selected(int spool_id) {
    spdlog::info("[AmsEditOverlay] Spool {} selected for slot {}", spool_id, slot_index_);

    // Look up SpoolInfo from cached spools
    for (const auto& spool : cached_spools_) {
        if (spool.id == spool_id) {
            // Auto-fill working_info_ from the selected spool. The field-by-
            // field copy this replaced had drifted from apply_spool_to_slot()
            // — it synthesized its own spool_name — so the same spool labelled
            // differently depending on whether it arrived here or through the
            // external-spool sync.
            apply_spool_to_slot(working_info_, spool);
            break;
        }
    }

    // Picker-entry shortcut (task #13): when the editor was opened directly on
    // the picker, a selection is a one-tap commit — apply the spool (done
    // above), then finish + close the whole overlay via the same commit path
    // header Save uses. commit_and_close() fires completion with the applied
    // spool (working_info_) and pops the overlay; the two-step flow's staging
    // is identical because the picker selection wrote working_info_ directly.
    if (opened_on_picker_) {
        spdlog::debug("[AmsEditOverlay] Picker-entry selection - committing and closing");
        commit_and_close();
        return;
    }

    // Normal two-step flow: return to the overview so the user can review, then
    // commit with the header Save.
    switch_to_form();
    update_ui();
    update_sync_button_state();
    update_spoolman_button_state();
}

void AmsEditOverlay::handle_card_clicked() {
    // The current-spool card is the direct edit affordance — tap opens the
    // unified spool-edit view (identity + color + logistics).
    spdlog::debug("[AmsEditOverlay] Spool card tapped - opening spool-edit");
    enter_spool_edit();
}

void AmsEditOverlay::handle_change_filament() {
    // "Change filament" row: with Spoolman, the user's inventory is their
    // spools — open the picker. Without Spoolman, go straight to spool-edit
    // (untracked setup) since there is no inventory to pick from.
    auto* subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = subj && lv_subject_get_int(subj) == 1;
    if (has_spoolman) {
        spdlog::debug("[AmsEditOverlay] Change filament tapped - opening Spoolman picker");
        switch_to_picker();
        return;
    }
    spdlog::debug("[AmsEditOverlay] Change filament tapped - opening spool-edit (no Spoolman)");
    enter_spool_edit();
}

bool AmsEditOverlay::populate_spool_edit_view() {
    // Everything the spool-edit view shows that XML alone cannot produce: the
    // catalog selector's vendor/type dropdown options and product rows, the
    // pending-color swatch, and the logistics field text. Reads only state this
    // object already holds, so it is safe to re-run against a rebuilt tree
    // without disturbing an edit in progress.
    if (!setup_details_selector()) {
        return false;
    }

    lv_obj_t* preview = find_widget("details_color_preview");
    if (preview) {
        helix::ui::apply_swatch_color(preview, details_color_, {});
    }

    populate_detail_fields();
    return true;
}

void AmsEditOverlay::repopulate() {
    // The view subject is C++-owned, so it keeps its value across the rebuild
    // and the fresh widgets bind to the right branch on their own. What does
    // not survive is the content each view populates on entry, so re-apply it
    // for whichever view is showing. The overview is entirely subject-bound and
    // on_activate() refreshes it.
    const int view = lv_subject_get_int(&view_mode_subject_);
    switch (view) {
    case VIEW_SPOOL_PICKER:
        populate_picker();
        break;
    case VIEW_SPOOL_EDIT:
        populate_spool_edit_view();
        break;
    case VIEW_COLOR:
        // Reached from spool-edit, which stays built underneath it.
        populate_spool_edit_view();
        populate_color_view();
        break;
    default:
        break;
    }
    spdlog::debug("[AmsEditOverlay] Repopulated view {}", view);
}

void AmsEditOverlay::enter_spool_edit() {
    // Single identity+color+logistics editor (spec §3.3): fresh untracked setup
    // and editing the current filament. Pre-fill from the working slot; the
    // toggle default (off=untracked / on=managed) rides ams_edit_is_managed via
    // bind_state_if_eq, and the logistics section hides for untracked slots.
    // Record tracked-ness at entry, before any unlink on Save can zero
    // spoolman_id. The untracked weight-commit branch in
    // handle_spool_edit_save() keys off this so a just-unlinked slot doesn't
    // stage Spoolman's core spool-weight over the real total (Finding 1).
    spool_edit_entered_tracked_ = working_info_.spoolman_id > 0;

    // Seed the pending color from the working slot so Save without a color tap
    // keeps the current color.
    details_color_ = working_info_.color_rgb;
    details_color_set_ = false;

    if (!api_) {
        api_ = get_moonraker_api();
    }

    // Seed the logistics fields from what we know. Meaningful only for managed
    // slots (the section is hidden otherwise), but seeding is harmless.
    detail_original_ = SpoolInfo{};
    detail_original_.id = working_info_.spoolman_id;
    detail_original_.filament_id = working_info_.spoolman_filament_id;
    detail_original_.vendor = working_info_.brand;
    detail_original_.material = working_info_.material;
    detail_original_.filament_name = working_info_.spool_name;
    detail_original_.remaining_weight_g = working_info_.remaining_weight_g;
    detail_original_.initial_weight_g = working_info_.total_weight_g;
    // The untracked "Spool weight" field maps to working_info_.total_weight_g on
    // save (see handle_spool_edit_save's untracked branch), so seed it from the
    // same source. Without this it defaults to 0 and renders "0" instead of
    // blank for an unknown-weight slot — which would stage 0 over the -1
    // sentinel on Save and make the open->save round-trip lossy. (Tracked slots
    // overwrite detail_original_/detail_working_ wholesale from the Spoolman
    // record below, so this seed is untracked-only.)
    detail_original_.spool_weight_g = working_info_.total_weight_g;
    if (working_info_.color_rgb != 0) {
        char hex_buf[8];
        snprintf(hex_buf, sizeof(hex_buf), "#%06X", working_info_.color_rgb);
        detail_original_.color_hex = hex_buf;
    }
    detail_working_ = detail_original_;
    // A missing catalog fragment means the view cannot be filled in, so stay
    // where we are rather than switching to a half-built editor.
    if (!populate_spool_edit_view()) {
        return;
    }

    // Print Label visibility is declarative — btn_detail_print_label binds its
    // hidden flag to `label_printer_configured` in ams_edit_overlay.xml, so
    // pairing a printer while this view is open reveals the button live.

    set_view(VIEW_SPOOL_EDIT);

    // Managed slots: refresh logistics from Spoolman (fresh data on view entry
    // rather than trusting cached_spools_ — review §3). Untracked slots, and
    // slots with a stale spoolman_id while Spoolman itself is unavailable,
    // skip the fetch but still enter the view (logistics section stays
    // hidden via is_managed_subject_, see update_ui()).
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    if (has_spoolman && working_info_.spoolman_id > 0 && api_) {
        const int spool_id = working_info_.spoolman_id;
        auto token = lifetime_.token();
        api_->spoolman().get_spoolman_spool(
            spool_id,
            [this, token, spool_id](const std::optional<SpoolInfo>& spool) {
                if (!spool) {
                    return;
                }
                // Liveness handled by token.defer(), not a bare expired() check.
                token.defer([this, spool = *spool]() {
                    detail_original_ = spool;
                    detail_working_ = spool;
                    populate_detail_fields();
                });
            },
            [spool_id](const MoonrakerError& err) {
                spdlog::warn("[AmsEditOverlay] Spool-details fetch for {} failed: {}", spool_id,
                             err.message);
            });
    }

    spdlog::debug("[AmsEditOverlay] Entered spool-edit view");
}

void AmsEditOverlay::handle_spool_edit_save(bool finish) {
    // finish=true (header Save on spool-edit): on a successful apply, finish the
    // whole edit via commit_and_close() instead of returning to the overview.
    // finish=false: apply the edits and return to the overview (used by tests
    // and any non-terminal caller). A validation failure stays on the view and
    // a tracked-slot PATCH error returns to the overview WITHOUT closing in both
    // modes.
    // --- Identity + color + toggle -> working_info_ (merged from the old
    //     filament-details apply path) ---
    // Apply catalog pick (if any) — brand/material/temps from the branded
    // catalog (spec §5: EffectiveFilament -> slot mapping reused).
    auto iequals = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size())
            return false;
        return std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
            return std::tolower(x) == std::tolower(y);
        });
    };
    const helix::printer::EffectiveFilament* ef = details_selector_.highlighted();
    if (ef) {
        working_info_.material = ef->type;
        // Preserve the user's stored brand string when the highlighted product
        // is the SAME vendor (case-insensitive). The selector is seeded to the
        // slot's brand, so an untouched Save re-highlights that vendor's first
        // product — adopting ef->brand would rewrite the user's "Sunlu" to the
        // catalog's canonical "SUNLU" casing (and, before the seed fix, clobber
        // it to "Generic"). A genuine vendor change still adopts the new brand.
        if (!iequals(ef->brand, working_info_.brand))
            working_info_.brand = ef->brand;
        working_info_.nozzle_temp_min = ef->nozzle_min;
        working_info_.nozzle_temp_max = ef->nozzle_max;
        working_info_.bed_temp = ef->bed_temp;
        // WHICH product, not just its material. ef->type collapses every PLA
        // product a vendor sells into one string, so without these the reopened
        // editor can only preselect-first and lands on whichever variant sorts
        // alphabetically first. Overwritten unconditionally (never merged) so a
        // stale id from a previous pick — or one that no longer resolves — is
        // replaced by whatever the user is actually confirming here.
        working_info_.catalog_id = ef->id;
        working_info_.product_name = ef->name;
        spdlog::info("[AmsEditOverlay] Spool-edit pick: '{} {}' [{}] ({}-{}/{}°C)", ef->brand,
                     ef->name, ef->id, ef->nozzle_min, ef->nozzle_max, ef->bed_temp);
    } else {
        // No product highlighted. With preselect-on-change enabled this only
        // happens when the rebuilt product list was empty — a type the firmware
        // whitelists but the catalog has no product for yet. If the user did
        // change the type, keep the vendor the dropdown still shows (it was
        // seeded to the slot's brand and the user didn't change it here) rather
        // than forcing Generic — only fall back to Generic when the selection
        // genuinely is Generic/empty. Temps are left as-is (no catalog data to
        // source them from). When the selected type still equals the slot's
        // material the identity is unchanged and the old skip behavior is right.
        std::string sel_type = details_selector_.current_type();
        if (!sel_type.empty() && !iequals(sel_type, working_info_.material)) {
            working_info_.material = sel_type; // material names are not translated (L070)
            std::string sel_vendor = details_selector_.current_vendor();
            if (iequals(sel_vendor, working_info_.brand)) {
                // Same vendor as the slot already had — preserve the user's
                // exact brand string (casing) rather than the dropdown's copy.
            } else if (!sel_vendor.empty()) {
                working_info_.brand = sel_vendor;
            } else {
                working_info_.brand = "Generic";
            }
            // The material genuinely changed and the catalog stocks nothing for
            // it, so any previously stored product identity now describes a
            // DIFFERENT material. Leaving it would make the next open navigate
            // the selector back to the old family and re-adopt the old product.
            working_info_.catalog_id.clear();
            working_info_.product_name.clear();
            spdlog::info("[AmsEditOverlay] Spool-edit type change with no catalog product: '{} {}'",
                         working_info_.brand, sel_type);
        }
    }

    // Apply pending color (catalog carries no color — spec §3.3).
    if (details_color_set_) {
        working_info_.color_rgb = details_color_;
        working_info_.color_name = helix::get_color_name_from_hex(details_color_);
        working_info_.multi_color_hexes.clear();
    }

    // Capture the explicit tracking decision.
    auto* subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    const bool has_spoolman = subj && lv_subject_get_int(subj) == 1;
    lv_obj_t* toggle = find_widget("save_to_spoolman_switch");
    const bool opted_in = toggle && lv_obj_has_state(toggle, LV_STATE_CHECKED);
    save_to_spoolman_opt_in_ = has_spoolman && opted_in;

    if (has_spoolman && !opted_in && working_info_.spoolman_id > 0) {
        // Toggle off on a managed slot = unlink: spoolman_id -> 0, identity kept
        // as local slot values; no Spoolman delete/write (resolution §2.1). This
        // is the single unlink path (the picker's unlink entry was retired).
        spdlog::info("[AmsEditOverlay] Save-to-Spoolman off — unlinking spool {}",
                     working_info_.spoolman_id);
        working_info_.clear_spoolman_link();
    }

    // --- Read + validate quantity/logistics fields BEFORE detaching the
    //     catalog selector. Every early return that keeps the user on this
    //     view (the negative-value check below) must leave brand/material
    //     selection intact, so this has to run ahead of detach()/
    //     clear_catalog() — an early return after detaching left the
    //     selector inert while the user was still on the view.
    read_detail_fields();

    // Reject negative numerics (same rule as SpoolEditModal::validate_fields).
    if (detail_working_.remaining_weight_g < 0 || detail_working_.spool_weight_g < 0 ||
        detail_working_.price < 0) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Values must not be negative"),
                                      3000);
        return; // stay on the spool-edit view so the user can fix it
    }

    details_selector_.detach();
    details_selector_.clear_catalog();

    // --- Logistics two-PATCH for slots that stay managed (merged from the old
    //     spool-details save path) ---
    if (working_info_.spoolman_id > 0 && !may_write_spoolman_now(original_info_, working_info_)) {
        // This save is going to raise "Different filament?" in commit_and_close().
        // Write NOTHING to Spoolman until the user answers: Cancel is a true abort
        // and cannot retract a PATCH that has already gone out. The identity
        // outcome (update / create-new / unlink) owns the write from here.
        //
        // Logistics-only fields typed in the same save (price, lot, notes,
        // location) are not carried through the prompt yet — Wave C's LinkIntent
        // refactor makes the whole decision one ordered plan.
        spdlog::info("[AmsEditOverlay] Withholding Spoolman write for slot {} pending "
                     "identity confirmation",
                     slot_index_);

        // Still stage the edited weights LOCALLY. The logistics block was doing
        // double duty — sending the PATCH and copying detail_working_ into
        // working_info_ — so withholding it alone would silently drop the user's
        // weight edit and leave do_spoolman_save() with no delta to write on
        // confirm. Deliberately do NOT touch original_info_: the delta is what
        // makes the post-confirm save PATCH the weight exactly once.
        working_info_.remaining_weight_g = static_cast<float>(detail_working_.remaining_weight_g);
        if (detail_working_.initial_weight_g > 0) {
            working_info_.total_weight_g = static_cast<float>(detail_working_.initial_weight_g);
        }
    } else if (working_info_.spoolman_id > 0) {
        nlohmann::json spool_patch;
        nlohmann::json filament_patch;
        SpoolmanSlotSaver::build_spool_patches(detail_original_, detail_working_, spool_patch,
                                               filament_patch);

        if (!spool_patch.empty() || !filament_patch.empty()) {
            if (!api_) {
                api_ = get_moonraker_api();
            }
            if (!api_) {
                ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("API not available"),
                                              3000);
            } else {
                const int spool_id = detail_working_.id;
                const int filament_id = detail_working_.filament_id;
                auto token = lifetime_.token();

                // Immediate Spoolman write on Save, then return to the overview
                // with refreshed working_info_. The overview's Back does NOT roll
                // these writes back — identical to SpoolEditModal.
                auto on_all_saved = [this, token, finish]() {
                    token.defer("AmsEditOverlay::on_logistics_saved", [this, finish]() {
                        ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Spool saved"),
                                                      2000);
                        working_info_.remaining_weight_g =
                            static_cast<float>(detail_working_.remaining_weight_g);
                        if (detail_working_.initial_weight_g > 0) {
                            working_info_.total_weight_g =
                                static_cast<float>(detail_working_.initial_weight_g);
                        }
                        // Keep the pre-save baseline in sync so the header Save
                        // dirty state doesn't light up from a logistics-only edit.
                        original_info_.remaining_weight_g = working_info_.remaining_weight_g;
                        original_info_.total_weight_g = working_info_.total_weight_g;
                        if (finish) {
                            // Header Save: finish the whole edit and close.
                            commit_and_close();
                            return;
                        }
                        switch_to_form();
                        update_ui();
                        update_temp_display();
                        update_sync_button_state();
                        update_spoolman_button_state();
                    });
                };
                auto on_error = [this, token, spool_id](const MoonrakerError& err) {
                    spdlog::error("[AmsEditOverlay] Failed to save spool {}: {}", spool_id,
                                  err.message);
                    // Toast + return to the overview. The catalog selector was
                    // already detached above, so staying on the now-inert
                    // spool-edit view would strand the user with a dead
                    // brand/material selector — land somewhere coherent, matching
                    // the old view-4 behavior (Finding 2). Runs on a bg thread;
                    // the switch must go through token.defer like on_all_saved.
                    token.defer("AmsEditOverlay::on_logistics_save_error", [this]() {
                        ToastManager::instance().show(ToastSeverity::ERROR,
                                                      lv_tr("Failed to save spool"), 3000);
                        switch_to_form();
                        update_ui();
                        update_temp_display();
                        update_sync_button_state();
                        update_spoolman_button_state();
                    });
                };

                if (!spool_patch.empty()) {
                    api_->spoolman().update_spoolman_spool(
                        spool_id, spool_patch,
                        [this, token, filament_id, filament_patch, on_all_saved, on_error]() {
                            token.defer(
                                "AmsEditOverlay::after_spool_patch",
                                [this, filament_id, filament_patch, on_all_saved, on_error]() {
                                    if (!filament_patch.empty() && filament_id > 0) {
                                        api_->spoolman().update_spoolman_filament(
                                            filament_id, filament_patch, on_all_saved, on_error);
                                    } else {
                                        on_all_saved();
                                    }
                                });
                        },
                        on_error);
                    return; // async path returns to overview from the callback
                }
                if (!filament_patch.empty() && filament_id > 0) {
                    api_->spoolman().update_spoolman_filament(filament_id, filament_patch,
                                                              on_all_saved, on_error);
                    return; // async path
                }
            }
        }
    } else {
        // Untracked slot: Remaining/Spool-weight are local overrides — there
        // is no Spoolman record to PATCH, so commit them straight into
        // working_info_ the way the rest of spool-edit's fields already do.
        // The overview header Save is the existing commit path for
        // working_info_ (this handler doesn't write settings directly) — so
        // we must NOT sync original_info_ here, or is_dirty() would go blind
        // and the header Save (the only commit path) would never light up.
        //
        // Blank-means-unchanged: read the textareas' emptiness directly
        // (read_detail_fields() collapses blank -> 0, which is a legitimate
        // explicit value we can't distinguish after the fact). A blank field
        // leaves the existing value — possibly the -1 "unknown" sentinel —
        // untouched: this both keeps an untouched unknown-weight slot from
        // going spuriously dirty (-1 -> 0) and preserves the sentinel.
        //
        // Per-field decision (decide_weight_staging): remaining always stages
        // when filled, because it means the same thing linked or not. Only
        // total_weight_g is withheld on an unlink-in-place, where the on-screen
        // "Spool wt" came from Spoolman's spool_weight (empty-spool CORE weight)
        // and would clobber a correct filament total.
        lv_obj_t* remaining_w = find_widget("detail_field_remaining");
        lv_obj_t* spool_wt_w = find_widget("detail_field_spool_weight");
        const char* remaining_t = remaining_w ? lv_textarea_get_text(remaining_w) : nullptr;
        const char* spool_wt_t = spool_wt_w ? lv_textarea_get_text(spool_wt_w) : nullptr;

        const WeightStaging staging = decide_weight_staging(spool_edit_entered_tracked_,
                                                            remaining_t && remaining_t[0] != '\0',
                                                            spool_wt_t && spool_wt_t[0] != '\0');

        if (staging.stage_remaining) {
            working_info_.remaining_weight_g =
                static_cast<float>(detail_working_.remaining_weight_g);
        }
        if (staging.stage_total) {
            working_info_.total_weight_g = static_cast<float>(detail_working_.spool_weight_g);
        }
    }

    // --- No logistics write (untracked, unchanged, or API missing) ---
    if (finish) {
        // Header Save: identity/color/weights are staged into working_info_;
        // finish the whole edit through the overview commit path and close.
        commit_and_close();
        return;
    }
    switch_to_form();
    update_ui();
    update_temp_display();
    update_sync_button_state();
    update_spoolman_button_state();
}

void AmsEditOverlay::handle_quick_swatch(lv_obj_t* swatch) {
    if (!swatch) {
        return;
    }
    lv_color_t c = lv_obj_get_style_bg_color(swatch, LV_PART_MAIN);
    details_color_ = (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) |
                     static_cast<uint32_t>(c.blue);
    details_color_set_ = true;
    lv_obj_t* preview = find_widget("details_color_preview");
    if (preview) {
        helix::ui::apply_swatch_color(preview, details_color_, {});
    }
    spdlog::debug("[AmsEditOverlay] Quick swatch picked: {:#08x}", details_color_);
}

void AmsEditOverlay::on_quick_swatch_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_quick_swatch(static_cast<lv_obj_t*>(lv_event_get_target(e)));
    }
}

void AmsEditOverlay::on_custom_color_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->open_color_view();
    }
}

void AmsEditOverlay::handle_picker_search(const char* text) {
    if (cached_spools_.empty()) {
        return;
    }
    render_spool_list(text ? text : "");
}

void AmsEditOverlay::populate_detail_fields() {
    struct FieldMap {
        const char* widget;
        std::string value;
    };
    char buf[16];
    // Negative values are the "unknown" sentinel (SlotInfo/SpoolInfo default
    // -1) — render blank rather than a literal "-1" (matches the
    // display-only unknown rendering already used for the overview's weight
    // input). A blank field also can't trip the negative-value Save
    // validation on an untouched, weight-unknown untracked slot.
    std::string remaining;
    if (detail_working_.remaining_weight_g >= 0) {
        snprintf(buf, sizeof(buf), "%.0f", detail_working_.remaining_weight_g);
        remaining = buf;
    }
    std::string spool_wt;
    if (detail_working_.spool_weight_g >= 0) {
        snprintf(buf, sizeof(buf), "%.0f", detail_working_.spool_weight_g);
        spool_wt = buf;
    }
    std::string price;
    if (detail_working_.price > 0) {
        snprintf(buf, sizeof(buf), "%.2f", detail_working_.price);
        price = buf;
    }
    const FieldMap fields[] = {
        {"detail_field_remaining", remaining},
        {"detail_field_spool_weight", spool_wt},
        {"detail_field_price", price},
        {"detail_field_location", detail_working_.location},
        {"detail_field_lot_nr", detail_working_.lot_nr},
        {"detail_field_comment", detail_working_.comment},
    };
    for (const auto& f : fields) {
        lv_obj_t* w = find_widget(f.widget);
        if (w) {
            lv_textarea_set_text(w, f.value.c_str());
        }
    }
}

void AmsEditOverlay::read_detail_fields() {
    auto text_of = [this](const char* name) -> std::string {
        lv_obj_t* w = find_widget(name);
        const char* t = w ? lv_textarea_get_text(w) : nullptr;
        return t ? t : "";
    };
    std::string s = text_of("detail_field_remaining");
    detail_working_.remaining_weight_g = s.empty() ? 0 : std::atof(s.c_str());
    s = text_of("detail_field_spool_weight");
    detail_working_.spool_weight_g = s.empty() ? 0 : std::atof(s.c_str());
    s = text_of("detail_field_price");
    detail_working_.price = s.empty() ? 0 : std::atof(s.c_str());
    detail_working_.location = text_of("detail_field_location");
    detail_working_.lot_nr = text_of("detail_field_lot_nr");
    detail_working_.comment = text_of("detail_field_comment");
}

void AmsEditOverlay::on_detail_field_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->read_detail_fields();
    }
}

void AmsEditOverlay::handle_scan_qr() {
#if HELIX_HAS_CAMERA
    spdlog::info("[AmsEditOverlay] Scan QR requested for slot {}", slot_index_);

    // The scanner overlay pushes ON TOP of the editor (spec §13.5). Our
    // on_deactivate treats that as "covered": session state and the view
    // subject survive; on_activate refreshes the widgets when we resurface.
    auto& scanner = helix::ui::get_qr_scanner_overlay();
    scanner.show(
        lv_screen_active(), slot_index_,
        [](const SpoolInfo& spool) {
            // Scanner result arrives outside our activation (we were covered;
            // lifetime_ was invalidated on cover) — so a pre-cover token would
            // be expired by design. Instead marshal via the queue and
            // re-validate on the singleton, which lives for the process. No
            // direct backend write anymore: the live form repopulates and the
            // user confirms with Save.
            SlotInfo scanned;
            apply_spool_to_slot(scanned, spool);
            helix::ui::queue_update([scanned = std::move(scanned)]() {
                auto& editor = get_ams_edit_overlay();
                if (!editor.get_root()) {
                    return; // editor tree gone (shutdown) — drop silently
                }
                int keep_slot = editor.working_info_.slot_index;
                int keep_global = editor.working_info_.global_index;
                int keep_tool = editor.working_info_.mapped_tool;
                editor.working_info_ = scanned;
                editor.working_info_.slot_index = keep_slot;
                editor.working_info_.global_index = keep_global;
                editor.working_info_.mapped_tool = keep_tool;
                editor.switch_to_form();
                editor.update_ui();
                editor.update_temp_display();
                editor.update_sync_button_state();
                editor.update_spoolman_button_state();
                NOTIFY_INFO("{} {} scanned — review and Save", scanned.brand, scanned.material);
            });
        },
        []() {
            // Cancel: scanner pops, editor resurfaces via on_activate. Nothing
            // to do — session state was never torn down.
            spdlog::debug("[AmsEditOverlay] QR scan cancelled - editor resumes");
        });
#else
    spdlog::debug("[AmsEditOverlay] Scan QR unavailable (no camera) for slot {}", slot_index_);
#endif // HELIX_HAS_CAMERA
}

#if HELIX_HAS_LABEL_PRINTER
void AmsEditOverlay::handle_print_label() {
    auto& settings = helix::LabelPrinterSettingsManager::instance();

    if (!settings.is_configured()) {
        ToastManager::instance().show(ToastSeverity::INFO,
                                      lv_tr("Set up your label printer in Settings"), 3000);
        return;
    }

    // Build SpoolInfo from AMS slot data
    SpoolInfo spool_info;
    bool found = false;
    for (const auto& spool : cached_spools_) {
        if (spool.id == working_info_.spoolman_id) {
            spool_info = spool;
            found = true;
            break;
        }
    }

    if (!found) {
        spool_info.id = working_info_.spoolman_id;
        spool_info.vendor = working_info_.brand;
        spool_info.material = working_info_.material;
        spool_info.filament_name = working_info_.spool_name;
        spool_info.remaining_weight_g = working_info_.remaining_weight_g;
        spool_info.initial_weight_g = working_info_.total_weight_g;
    }

    // Use the standard print flow (handles all printer types including IPP modal)
    auto print_cb = [](bool success, const std::string& error) {
        if (success) {
            ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Label printed"), 2000);
        } else {
            spdlog::error("[AmsEditOverlay] Print failed: {}", error);
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          helix::friendly_label_printer_error(error).c_str(), 5000);
        }
    };

    if (!helix::maybe_show_ipp_print_modal(spool_info, print_cb)) {
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Printing label..."), 2000);
        helix::print_spool_label(spool_info, print_cb);
    }
}
#endif

void AmsEditOverlay::update_spoolman_button_state() {
    if (!overlay_root_) {
        return;
    }

    // Fresh synchronous read — the XML binding fires asynchronously (#311).
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;

    // The action row itself is always visible — Change Filament works with and
    // without Spoolman. Only the Scan QR button is gated on Spoolman: it has no
    // offline analogue. When hidden it drops out of the flex row, leaving the
    // lone Change Filament button centered.
    lv_obj_t* scan_btn = find_widget("btn_scan_qr_code");
    if (scan_btn) {
#if defined(HELIX_PLATFORM_ESP32)
        // No camera on the v1 Core+AMS cut — Scan QR has no offline analogue,
        // so keep it hidden regardless of Spoolman availability.
        lv_obj_add_flag(scan_btn, LV_OBJ_FLAG_HIDDEN);
        (void)has_spoolman;
#else
        if (has_spoolman) {
            lv_obj_remove_flag(scan_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(scan_btn, LV_OBJ_FLAG_HIDDEN);
        }
#endif
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

void AmsEditOverlay::update_ui() {
    if (!overlay_root_) {
        return;
    }

    // Managed-vs-untracked signal (drives mark, details row, toggle default).
    // Fresh synchronous read — the XML binding fires asynchronously (#311);
    // mirrors update_spoolman_button_state(). A slot can carry a stale
    // spoolman_id even when Spoolman itself is unavailable, so gate on both.
    // Computed before the title because the title carries the spool number.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    const bool managed = has_spoolman && working_info_.spoolman_id > 0;
    lv_subject_set_int(&is_managed_subject_, managed ? 1 : 0);

    // Header title via subject
    if (slot_index_ < 0) {
        snprintf(slot_indicator_buf_, sizeof(slot_indicator_buf_), "%s",
                 lv_tr("External Filament"));
    } else {
        snprintf(slot_indicator_buf_, sizeof(slot_indicator_buf_), lv_tr("Slot %d Filament"),
                 slot_index_ + 1);
    }
    lv_subject_copy_string(&slot_indicator_subject_, slot_indicator_buf_);

    // Identity chip: tracked = spool name (+ mark via binding);
    // untracked = "Brand · Material" (spec §3.8 locked format).
    //
    // spool_name carries the filament name alone ("Ambrosia Pink"), so printing
    // it verbatim would drop the vendor and the material and say less than the
    // untracked branch does. compose_filament_label() joins the three the same
    // way the AMS card does, and drops a brand or material the name already
    // contains — so a Spoolman name that reads "Bambu Lab ASA" still prints
    // once, not three times.
    //
    // The name is not always on the slot. AFC only publishes filament_name from
    // v1.2.0, so on an older unit a Spoolman-linked lane has an empty spool_name
    // while the identity cache holds the real one — the same split the loaded
    // card resolves through resolve_filament_label(). Consult it here too, or
    // this card silently degrades to "Elegoo · PLA" for a spool we can name.
    std::string chip_brand = working_info_.brand;
    std::string chip_name = working_info_.spool_name;
    if (working_info_.spoolman_id > 0 && (chip_name.empty() || chip_brand.empty())) {
        if (const auto identity = SpoolmanManager::find_identity(working_info_.spoolman_id)) {
            if (chip_name.empty()) {
                chip_name = identity->filament_name;
            }
            if (chip_brand.empty()) {
                chip_brand = identity->vendor;
            }
        }
    }

    if (managed && !chip_name.empty()) {
        const std::string chip =
            helix::compose_filament_label(chip_brand, chip_name, working_info_.material);
        snprintf(chip_text_buf_, sizeof(chip_text_buf_), "%s", chip.c_str());
    } else {
        const char* brand = working_info_.brand.empty() ? "Generic" : working_info_.brand.c_str();
        const char* material =
            working_info_.material.empty() ? "—" : working_info_.material.c_str();
        snprintf(chip_text_buf_, sizeof(chip_text_buf_), "%s \xC2\xB7 %s", brand, material);
    }
    lv_subject_copy_string(&chip_text_subject_, chip_text_buf_);

    // Spool number beside the tracked mark. Empty for untracked slots; the label's
    // own hidden-flag binding on ams_edit_is_managed keeps it off screen there.
    if (managed) {
        snprintf(spoolman_id_buf_, sizeof(spoolman_id_buf_), "#%d", working_info_.spoolman_id);
    } else {
        spoolman_id_buf_[0] = '\0';
    }
    lv_subject_copy_string(&spoolman_id_subject_, spoolman_id_buf_);

    // Card color swatch (grandfathered dynamic bg-color write, same as the old
    // big overview swatch).
    lv_obj_t* card_color_swatch = find_widget("card_color_swatch");
    if (card_color_swatch) {
        helix::ui::apply_swatch_color(card_color_swatch, working_info_.color_rgb,
                                      working_info_.multi_color_hexes);
    }

    // Remaining bar + label — display-only. Unknown weight data
    // (total_weight_g <= 0) renders "—" and never mutates working_info_: the
    // old code fabricated a synthetic 1000g total/remaining here, which made
    // every weightless slot dirty-on-open and persisted fake 1000/1000g on
    // Save even when the user made no edits.
    bool has_weight = working_info_.total_weight_g > 0;
    int remaining_pct = 0;
    if (has_weight) {
        float rem = working_info_.remaining_weight_g >= 0 ? working_info_.remaining_weight_g : 0;
        remaining_pct = static_cast<int>(std::lround(100.0f * rem / working_info_.total_weight_g));
        remaining_pct = std::max(0, std::min(100, remaining_pct));
        snprintf(remaining_pct_buf_, sizeof(remaining_pct_buf_), "%.0f / %.0fg (%d%%)", rem,
                 working_info_.total_weight_g, remaining_pct);
    } else {
        snprintf(remaining_pct_buf_, sizeof(remaining_pct_buf_), "\xE2\x80\x94"); // "—"
    }
    lv_subject_copy_string(&remaining_pct_subject_, remaining_pct_buf_);

    // Progress bar (inside the card): only meaningful when total weight is
    // known. This imperative hide is the SOLE writer of the container's hidden
    // state — the XML edit_remaining_mode binding was retired with the inline
    // remaining editor.
    lv_obj_t* progress_container = find_widget("remaining_progress_container");
    lv_obj_t* progress_fill = find_widget("remaining_progress_fill");
    if (has_weight) {
        if (progress_container) {
            lv_obj_remove_flag(progress_container, LV_OBJ_FLAG_HIDDEN);
        }
        if (progress_fill) {
            lv_obj_set_width(progress_fill, lv_pct(remaining_pct));
        }
    } else if (progress_container) {
        lv_obj_add_flag(progress_container, LV_OBJ_FLAG_HIDDEN);
    }

    // Temperature display based on material/spool data
    update_temp_display();

    // Tool remap dropdown (backends that support it)
    lv_obj_t* tool_remap_row = find_widget("tool_remap_row");
    lv_obj_t* tool_dropdown = find_widget("tool_dropdown");
    auto* backend = AmsState::instance().get_backend();
    bool can_remap = backend && backend->get_system_info().supports_tool_mapping;

    if (tool_remap_row) {
        if (can_remap) {
            lv_obj_remove_flag(tool_remap_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tool_remap_row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (tool_dropdown && can_remap) {
        int tool_count = static_cast<int>(backend->get_system_info().tool_to_slot_map.size());
        std::string tool_options;
        for (int i = 0; i < tool_count; i++) {
            if (!tool_options.empty()) {
                tool_options += '\n';
            }
            tool_options += "T" + std::to_string(i);
        }
        lv_dropdown_set_options(tool_dropdown, tool_options.c_str());

        int tool_idx = std::max(0, working_info_.mapped_tool);
        tool_idx = std::min(tool_idx, tool_count - 1);
        lv_dropdown_set_selected(tool_dropdown, tool_idx);
    }
}

void AmsEditOverlay::update_temp_display() {
    if (!overlay_root_) {
        return;
    }

    // Get temperature range from slot info (populated from Spoolman or material defaults)
    int nozzle_min = working_info_.nozzle_temp_min;
    int nozzle_max = working_info_.nozzle_temp_max;
    int bed_temp = working_info_.bed_temp;

    // Fall back to material-based defaults from filament database if not set
    if (nozzle_min == 0 && nozzle_max == 0 && !working_info_.material.empty()) {
        auto mat_info = filament::find_material(working_info_.material);
        if (mat_info) {
            nozzle_min = mat_info->nozzle_min;
            nozzle_max = mat_info->nozzle_max;
            bed_temp = mat_info->bed_temp;
            spdlog::debug("[AmsEditOverlay] Using filament database temps for {}: {}-{}°C nozzle, "
                          "{}°C bed",
                          working_info_.material, nozzle_min, nozzle_max, bed_temp);
        } else {
            // Fallback to PLA defaults for unknown materials
            auto pla_info = filament::find_material("PLA");
            if (pla_info) {
                nozzle_min = pla_info->nozzle_min;
                nozzle_max = pla_info->nozzle_max;
                bed_temp = pla_info->bed_temp;
            } else {
                // Ultimate fallback (should never happen - PLA is in database)
                nozzle_min = 200;
                nozzle_max = 230;
                bed_temp = 60;
            }
            spdlog::debug("[AmsEditOverlay] Material '{}' not in database, using PLA defaults",
                          working_info_.material);
        }
    }

    // Full label strings (prefix included) — the card temps row has no separate
    // caption labels. En-dash between the nozzle range.
    snprintf(temp_nozzle_buf_, sizeof(temp_nozzle_buf_), "%s %d\xE2\x80\x93%d°C", lv_tr("Nozzle"),
             nozzle_min, nozzle_max);
    lv_subject_copy_string(&temp_nozzle_subject_, temp_nozzle_buf_);

    snprintf(temp_bed_buf_, sizeof(temp_bed_buf_), "%s %d°C", lv_tr("Bed"), bed_temp);
    lv_subject_copy_string(&temp_bed_subject_, temp_bed_buf_);
}

bool AmsEditOverlay::is_dirty() const {
    // Compare relevant fields that can be edited.
    //
    // catalog_id / product_name are deliberately NOT compared, for the same
    // reason the temps aren't: the spool-edit view AUTO-highlights a product
    // (preselect_first / preselect_on_change), and Save copies whatever is
    // highlighted. Including them would make an untouched open-and-Save of a
    // slot that never had a catalog pick report itself dirty. Nothing is lost —
    // handle_spool_edit_save(finish=true) is the only production caller and it
    // routes straight into commit_and_close(), which never consults is_dirty().
    return working_info_.color_rgb != original_info_.color_rgb ||
           working_info_.material != original_info_.material ||
           working_info_.brand != original_info_.brand ||
           working_info_.spoolman_id != original_info_.spoolman_id ||
           working_info_.mapped_tool != original_info_.mapped_tool ||
           std::abs(working_info_.remaining_weight_g - original_info_.remaining_weight_g) > 0.1f ||
           std::abs(working_info_.total_weight_g - original_info_.total_weight_g) > 0.1f;
}

void AmsEditOverlay::update_sync_button_state() {
    if (!subjects_initialized_) {
        return;
    }
    // Spool-edit view: Save is always enabled — the edits live in the on-screen
    // widgets and aren't staged into working_info_ until Save runs, so is_dirty()
    // can't see them. Overview: dirty-gated (replaces the modal's Save/Close text
    // morph). Reads (never writes) the view subject.
    const int view = lv_subject_get_int(&view_mode_subject_);
    const bool disabled = (view == VIEW_SPOOL_EDIT) ? false : !is_dirty();
    lv_subject_set_int(&save_disabled_subject_, disabled ? 1 : 0);
}

void AmsEditOverlay::open_color_view() {
    // Seed custom sub-state from the spool-edit view's pending color (the only
    // entry point).
    custom_color_ = details_color_;
    if (custom_color_ == 0) {
        custom_color_ = 0x808080;
    }
    populate_color_view();
    set_view(VIEW_COLOR);
    spdlog::debug("[AmsEditOverlay] Color view opened (returns to spool-edit)");
}

void AmsEditOverlay::populate_color_view() {
    lv_obj_t* hsv = find_widget("ams_color_hsv");
    if (hsv) {
        ui_hsv_picker_set_color_rgb(hsv, custom_color_);
    }
    lv_obj_t* preview = find_widget("ams_color_preview");
    if (preview) {
        helix::ui::apply_swatch_color(preview, custom_color_, {});
    }
    lv_obj_t* hex_input = find_widget("ams_color_hex_input");
    if (hex_input) {
        char buf[10];
        snprintf(buf, sizeof(buf), "#%06X", custom_color_);
        lv_textarea_set_text(hex_input, buf);
    }
}

void AmsEditOverlay::apply_color(uint32_t rgb) {
    // Color staging always goes to the spool-edit working state — committed on
    // the spool-edit Save. The overview swatch entry point was retired, so
    // there is no direct-slot color edit path anymore.
    details_color_ = rgb;
    details_color_set_ = true;
    lv_obj_t* preview = find_widget("details_color_preview");
    if (preview) {
        helix::ui::apply_swatch_color(preview, rgb, {});
    }
    set_view(VIEW_SPOOL_EDIT);
}

void AmsEditOverlay::handle_color_swatch(lv_obj_t* swatch) {
    if (!swatch) {
        return;
    }
    lv_color_t c = lv_obj_get_style_bg_color(swatch, LV_PART_MAIN);
    uint32_t rgb = (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) |
                   static_cast<uint32_t>(c.blue);
    apply_color(rgb); // preset tap applies immediately and returns
}

void AmsEditOverlay::handle_custom_color_changed(uint32_t rgb) {
    custom_color_ = rgb;
    lv_obj_t* preview = find_widget("ams_color_preview");
    if (preview) {
        helix::ui::apply_swatch_color(preview, rgb, {});
    }
    lv_obj_t* hex_input = find_widget("ams_color_hex_input");
    if (hex_input) {
        char buf[10];
        snprintf(buf, sizeof(buf), "#%06X", rgb);
        lv_textarea_set_text(hex_input, buf);
    }
}

void AmsEditOverlay::handle_color_hex_changed() {
    lv_obj_t* hex_input = find_widget("ams_color_hex_input");
    if (!hex_input) {
        return;
    }
    const char* text = lv_textarea_get_text(hex_input);
    uint32_t rgb = 0;
    if (text && helix::parse_hex_color(text, rgb)) {
        custom_color_ = rgb;
        lv_obj_t* hsv = find_widget("ams_color_hsv");
        if (hsv) {
            ui_hsv_picker_set_color_rgb(hsv, rgb);
        }
        lv_obj_t* preview = find_widget("ams_color_preview");
        if (preview) {
            helix::ui::apply_swatch_color(preview, rgb, {});
        }
    }
}

void AmsEditOverlay::handle_color_apply() {
    apply_color(custom_color_);
}

void AmsEditOverlay::on_color_swatch_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_color_swatch(static_cast<lv_obj_t*>(lv_event_get_target(e)));
    }
}

void AmsEditOverlay::on_color_apply_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_color_apply();
    }
}

void AmsEditOverlay::on_color_hex_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_color_hex_changed();
    }
}

// ============================================================================
// Save Orchestration
// ============================================================================

void AmsEditOverlay::fire_completion(bool saved) {
    if (completion_fired_) {
        return; // Save/back already completed; safety-net close callback is a no-op
    }
    completion_fired_ = true;
    spdlog::info("[AmsEditOverlay] fire_completion saved={} slot={} spoolman_id={} material={}",
                 saved, slot_index_, working_info_.spoolman_id, working_info_.material);
    if (completion_callback_) {
        EditResult result;
        result.saved = saved;
        result.slot_index = slot_index_;
        result.slot_info = working_info_;
        completion_callback_(result);
    }
}

void AmsEditOverlay::close_editor(bool saved) {
    fire_completion(saved);
    NavigationManager::instance().go_back();
}

// ============================================================================
// Event Handlers
// ============================================================================

void AmsEditOverlay::handle_back() {
    int view = lv_subject_get_int(&view_mode_subject_);
    switch (view) {
    case VIEW_SPOOL_PICKER:
        switch_to_form();
        break;
    case VIEW_SPOOL_EDIT:
        // Leave without applying (Save is the apply path)
        details_selector_.detach();
        details_selector_.clear_catalog();
        switch_to_form();
        break;
    case VIEW_COLOR:
        // Leave without applying; the color view is only reachable from
        // spool-edit, so it always pops back there.
        set_view(VIEW_SPOOL_EDIT);
        break;
    case VIEW_OVERVIEW:
    default:
        // Back on the overview = Cancel: discard changes, close (spec §13.3)
        spdlog::debug("[AmsEditOverlay] Back on overview - cancelling");
        working_info_ = original_info_;
        close_editor(false);
        break;
    }
}

void AmsEditOverlay::handle_tool_changed(int index) {
    // No "None" — index 0 = T0, index 1 = T1, etc.
    working_info_.mapped_tool = index;
    working_info_.tool_mapping_override = (index != original_info_.mapped_tool);
    spdlog::debug("[AmsEditOverlay] Tool changed to: T{} (override={})", index,
                  working_info_.tool_mapping_override);
    update_sync_button_state();
}

bool AmsEditOverlay::should_create_new_spool(const SlotInfo& working_info, bool save_to_spoolman) {
    return working_info.spoolman_id == 0 && save_to_spoolman &&
           helix::SpoolmanSlotSaver::is_filament_complete(working_info);
}

bool AmsEditOverlay::needs_identity_confirmation(const SlotInfo& original, const SlotInfo& edited) {
    if (edited.spoolman_id <= 0) {
        return false; // nothing linked to clobber
    }
    auto changes = helix::SpoolmanSlotSaver::detect_changes(original, edited);
    if (!changes.any()) {
        return false;
    }
    return is_material_identity_change(original, edited);
}

void AmsEditOverlay::handle_save() {
    // Header Save on the spool-edit view finishes the whole edit: apply the
    // view's identity/color/logistics edits first (may go async for a tracked
    // slot's two-PATCH), then the apply path routes into commit_and_close().
    // Validation failure keeps the user on the view.
    if (lv_subject_get_int(&view_mode_subject_) == VIEW_SPOOL_EDIT) {
        handle_spool_edit_save(/*finish=*/true);
        return;
    }
    commit_and_close();
}

void AmsEditOverlay::commit_and_close() {
    spdlog::info("[AmsEditOverlay] Saving edits for slot {}", slot_index_);

    if (!api_) {
        api_ = get_moonraker_api();
    }

    // Switching the linked spool (A>0 -> B>0, or 0 -> B>0) is a pure RELINK, not
    // an edit of the linked spool's record. The newly linked spool already owns
    // its identity/weight in Spoolman, so there is nothing to confirm and nothing
    // to PATCH — prompting to "update the linked spool anyway" or repointing a
    // spool here would corrupt the wrong record (task #16). The completion
    // consumer's commit_slot_edit()/commit_external_spool_edit() call registers
    // the new active spool server-side; commit the link locally and close.
    // Unlink (working id == 0) is intentionally NOT treated as a relink — it
    // falls through to the existing new-spool / local-close logic.
    const bool relinked_to_existing_spool =
        working_info_.spoolman_id > 0 && working_info_.spoolman_id != original_info_.spoolman_id;
    if (relinked_to_existing_spool) {
        spdlog::info("[AmsEditOverlay] Relink slot {} to spool {} (was {}) — no identity confirm, "
                     "no Spoolman PATCH of the old spool",
                     slot_index_, working_info_.spoolman_id, original_info_.spoolman_id);
        close_editor(true);
        return;
    }

    if (api_ && get_printer_state().is_spoolman_available()) {
        const bool has_linked_spool = working_info_.spoolman_id > 0;
        auto changes = helix::SpoolmanSlotSaver::detect_changes(original_info_, working_info_);

        // Explicit opt-in replaces the silent auto-create (spec §4.2): an
        // untracked filament stays local unless the toggle said otherwise.
        const bool can_create_new =
            should_create_new_spool(working_info_, save_to_spoolman_opt_in_);

        // §6: identity change on a linked spool confirms on EVERY Spoolman
        // backend now (the AD5X-only gate is gone).
        if (needs_identity_confirmation(original_info_, working_info_)) {
            prompt_identity_change_then_save();
            return;
        }

        if ((has_linked_spool && changes.any()) || can_create_new) {
            do_spoolman_save();
            return; // Async path - close_editor called from callback
        }
    }

    // No Spoolman changes (or no Spoolman) - save locally immediately
    close_editor(true);
}

void AmsEditOverlay::do_spoolman_save(helix::SpoolmanSlotSaver::LinkIntent intent) {
    auto token = lifetime_.token();
    auto saver = std::make_shared<helix::SpoolmanSlotSaver>(api_);
    saver->save(original_info_, working_info_, intent,
                [this, token, saver](const helix::SaveResult& result) {
                    // Spoolman callback arrives on a background thread — defer
                    // to the UI thread before touching LVGL subjects/widgets.
                    token.defer([this, result]() {
                        if (!result.success) {
                            // Local save still proceeds; only the Spoolman mirror failed.
                            spdlog::error("[AmsEditOverlay] Spoolman save failed, saving locally");
                            ToastManager::instance().show(
                                ToastSeverity::ERROR,
                                lv_tr("Couldn't update Spoolman — saved locally"), 3000);
                        } else if (result.created_new_spool || result.repointed_filament) {
                            // Persist new Spoolman IDs into working_info_ so the
                            // completion callback's backend->set_slot_info() writes
                            // the link back to the slot. Without this, a subsequent
                            // edit would not know the spool exists and would create
                            // a duplicate.
                            if (result.new_spool_id != 0) {
                                working_info_.spoolman_id = result.new_spool_id;
                            }
                            if (result.new_filament_id != 0) {
                                working_info_.spoolman_filament_id = result.new_filament_id;
                            }
                            if (result.new_vendor_id != 0) {
                                working_info_.spoolman_vendor_id = result.new_vendor_id;
                            }
                            // Deliberate async exception to "the commit owns
                            // the active-spool sync": a newly created spool's
                            // id only exists HERE, after the completion
                            // consumer already ran with id 0. Notify Moonraker
                            // now so Mainsail/Fluidd show the new spool as
                            // active and filament tracking starts.
                            if (result.created_new_spool && result.new_spool_id != 0 && api_) {
                                sync_active_spool(api_, result.new_spool_id);
                            }
                            if (result.created_new_spool) {
                                ToastManager::instance().show(ToastSeverity::INFO,
                                                              lv_tr("Added to Spoolman"), 2500);
                            }
                            // Repoint is silent — IDs change but no toast.
                        }
                        close_editor(true);
                    });
                });
}

bool AmsEditOverlay::may_write_spoolman_now(const SlotInfo& original, const SlotInfo& edited) {
    return !needs_identity_confirmation(original, edited);
}

AmsEditOverlay::WeightStaging AmsEditOverlay::decide_weight_staging(bool entered_tracked,
                                                                    bool remaining_filled,
                                                                    bool total_filled) {
    WeightStaging staging;
    staging.stage_remaining = remaining_filled;
    staging.stage_total = total_filled && !entered_tracked;
    return staging;
}

bool AmsEditOverlay::is_material_identity_change(const SlotInfo& original, const SlotInfo& edited) {
    if (!helix::FilamentMapper::materials_match(original.material, edited.material)) {
        return true;
    }
    return !helix::FilamentMapper::colors_match(original.color_rgb, edited.color_rgb);
}

void AmsEditOverlay::prompt_identity_change_then_save() {
    // Nothing can detect a spool swap on these systems — no RFID, no colour
    // sensing — so the user's answer is the only signal, and the dialog has to
    // offer the outcome they actually want.
    //
    // PRIMARY is "It's a new spool": create a new Spoolman spool and rebind the
    // lane, leaving the linked one untouched. That is the case a lane keeps its
    // link across an eject for, and it used to be unreachable — the save
    // silently patched the OLD spool instead, which is the reported corruption.
    //
    // Cancel stays a TRUE ABORT: nothing written, locally or remotely. That
    // guarantee is worth more than a third button, so "update the linked spool
    // to match" is deliberately NOT offered here — correcting a mislabelled
    // spool belongs in the Spoolman panel's own edit, where it is not one
    // mis-tap away from overwriting a different spool's identity.
    lv_obj_t* dlg = modal_show_confirmation(
        lv_tr("Different filament?"),
        lv_tr("This doesn't match the linked Spoolman spool. Add it as a new spool, or update "
              "the linked one to match?"),
        ModalSeverity::Warning, lv_tr("It's a new spool"), on_identity_confirm_cb,
        on_identity_cancel_cb, nullptr);
    if (!dlg) {
        // Couldn't show the dialog — abort rather than guess. Falling through to
        // a write here would pick one of two destructive outcomes on the user's
        // behalf, which is exactly what this gate exists to prevent.
        spdlog::warn("[AmsEditOverlay] identity confirmation failed to show; aborting save");
        close_editor(false);
    }
}

void AmsEditOverlay::on_identity_confirm_cb(lv_event_t* /*e*/) {
    // Dismiss the confirmation FIRST — modal_dialog has no auto-close, and
    // leaving it up would keep the buttons re-tappable, double-firing the
    // Spoolman write. Confirmation modals still stack above the overlay (§13.6).
    Modal::hide(Modal::get_top());
    // "It's a new spool" — create and rebind; the previously linked spool is
    // left exactly as it was.
    get_ams_edit_overlay().do_spoolman_save(helix::SpoolmanSlotSaver::LinkIntent::CreateAndRebind);
}

void AmsEditOverlay::on_identity_cancel_cb(lv_event_t* /*e*/) {
    // TRUE ABORT: dismiss the confirmation and do NOTHING else — no
    // close_editor(), no completion. The staged edits (working_info_,
    // details_color_, ...) stay exactly as they were so the user can tweak
    // and re-Save (the dialog reappears — same diff) or Back out (Back
    // already discards via working_info_ = original_info_). Committing the
    // slot locally here would leak the unconfirmed "different filament"
    // identity into the AMS panel while Spoolman still shows the old one —
    // exactly the outcome this confirmation exists to gate.
    Modal::hide(Modal::get_top());
    auto& overlay = get_ams_edit_overlay();
    // handle_spool_edit_save()'s header-Save "finish" path unconditionally
    // detaches + clears the catalog selector before reaching here (see
    // reattach_details_selector()'s doc comment for why). Abort leaves the
    // user ON the spool-edit view, so revive the selector or the vendor/
    // type/product picker goes dead.
    if (lv_subject_get_int(&overlay.view_mode_subject_) == VIEW_SPOOL_EDIT) {
        overlay.reattach_details_selector();
    }
}

void AmsEditOverlay::reattach_details_selector() {
    // Revive the selector after the identity-confirm Cancel abort. Deliberately
    // does NOT re-seed the pending color (setup_details_selector() leaves color
    // alone) — re-seeding would clear the staged color diff and make the
    // "Different filament?" dialog vanish on a subsequent re-Save.
    setup_details_selector();
}

bool AmsEditOverlay::setup_details_selector() {
    lv_obj_t* fragment = find_widget("details_catalog_selector");
    if (!fragment) {
        spdlog::warn("[AmsEditOverlay] details_catalog_selector fragment missing");
        return false;
    }
    auto* backend = AmsState::instance().get_backend();
    auto allowed = backend ? backend->get_supported_materials() : std::nullopt;
    std::optional<std::string> seed = working_info_.material.empty()
                                          ? std::nullopt
                                          : std::optional<std::string>(working_info_.material);
    // Seed the Vendor dropdown from the slot's existing brand so an untouched
    // Save round-trips it (the selector otherwise snaps vendor to Generic and
    // preselect_first() would then paint a Generic product over the user's
    // saved brand). Empty brand -> nullopt -> Generic default, unchanged.
    std::optional<std::string> vendor_seed = working_info_.brand.empty()
                                                 ? std::nullopt
                                                 : std::optional<std::string>(working_info_.brand);
    details_selector_.attach(fragment);
    details_selector_.configure(std::move(seed), std::move(allowed), std::move(vendor_seed));
    // A vendor/type dropdown change must always leave a product checked so a
    // subsequent header Save can't silently drop the identity change (the
    // rebuilt list would otherwise be all-unchecked and Save would skip
    // identity). Opt in before populate; the standalone picker stays opt-out.
    details_selector_.set_preselect_on_change(true);
    details_selector_.populate();
    // Restore the EXACT product the user last saved, when we have one. Vendor +
    // material family alone are not enough: preselect_first() takes
    // ordered_products_for().front(), and a vendor whose products all share one
    // material (SUNLU's six PLAs) has no variant/rank tiebreak left, so it falls
    // through to lowercased-name alphabetical — "PLA Marble" wins every time and
    // a saved "PLA+ 2.0" comes back relabelled.
    //
    // preselect_product_id() returns false for an id that no longer resolves (a
    // custom overlay product the user deleted, an id retired by an app update);
    // preselect_first() then keeps the list from opening all-unchecked. The
    // stored product_name is still on working_info_ either way, so nothing
    // downstream forgets what was chosen until the user confirms a replacement.
    if (!details_selector_.preselect_product_id(working_info_.catalog_id)) {
        details_selector_.preselect_first();
    }
    // A Spoolman-only vendor (present on the server but absent from the bundled
    // catalog) isn't in the catalog brand list, so the seed above snapped it to
    // Generic. Fetch the live vendor list and merge it in so the seed resolves.
    maybe_merge_spoolman_vendors();
    return true;
}

void AmsEditOverlay::maybe_merge_spoolman_vendors() {
    // Same availability gate the rest of this overlay uses — skip the RPC (and
    // its "method not found" warn) on a Spoolman-less printer, where the catalog-
    // only vendor list is the accepted behavior.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    if (!spoolman_subj || lv_subject_get_int(spoolman_subj) != 1) {
        return;
    }
    if (!api_) {
        api_ = get_moonraker_api();
    }
    if (!api_) {
        return;
    }

    auto tok = lifetime_.token();
    api_->spoolman().get_spoolman_vendors(
        [this, tok](const std::vector<VendorInfo>& vendors) {
            // Background thread: build a plain name list. No `this`/member access
            // here — L081-safe (the defer wrapper does the atomic liveness check
            // on the main thread).
            std::vector<std::string> names;
            names.reserve(vendors.size());
            for (const auto& v : vendors) {
                if (!v.name.empty()) {
                    names.push_back(v.name);
                }
            }
            tok.defer("AmsEditOverlay::merge_spoolman_vendors_apply",
                      [this, names = std::move(names)]() mutable {
                          // Only meaningful while the spool-edit view is still up.
                          if (lv_subject_get_int(&view_mode_subject_) != VIEW_SPOOL_EDIT) {
                              return;
                          }
                          details_selector_.set_additional_vendors(std::move(names));
                          // set_additional_vendors() rebuilds the dropdowns and
                          // drops both the highlight and the anchor, so the
                          // saved product has to be re-seeded here or the merge
                          // undoes setup_details_selector()'s restore. Same
                          // id-then-first order for the same reason.
                          //
                          // For a pure-Spoolman vendor the catalog has no
                          // product at all, so both calls no-op and the list
                          // stays empty — handle_spool_edit_save() then keeps
                          // the dropdown's vendor string, preserving the brand.
                          if (!details_selector_.preselect_product_id(working_info_.catalog_id)) {
                              details_selector_.preselect_first();
                          }
                      });
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[AmsEditOverlay] Spoolman vendor merge skipped: {}", err.message);
        });
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void AmsEditOverlay::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    // The details fragment needs catalog_select_* registered even if the
    // standalone picker never opened.
    FilamentCatalogSelector::register_callbacks();

    register_xml_callbacks({
        {"ams_edit_back_cb", on_back_cb},
        {"ams_edit_card_clicked_cb", on_card_clicked_cb},
        {"ams_edit_change_filament_cb", on_change_filament_cb},
        {"ams_edit_setup_entry_cb", on_setup_entry_cb},
        {"ams_edit_quick_swatch_cb", on_quick_swatch_cb},
        {"ams_edit_custom_color_cb", on_custom_color_cb},
        {"ams_edit_color_swatch_cb", on_color_swatch_cb},
        {"ams_edit_color_apply_cb", on_color_apply_cb},
        {"ams_edit_color_hex_changed_cb", on_color_hex_changed_cb},
        {"ams_edit_detail_field_changed_cb", on_detail_field_changed_cb},
        {"ams_edit_save_cb", on_save_cb},
        {"ams_edit_print_label_cb", on_print_label_cb},
        {"ams_edit_scan_qr_cb", on_scan_qr_cb},
        {"ams_edit_picker_search_cb", on_picker_search_cb},
        {"ams_edit_picker_retry_cb", on_picker_retry_cb},
        // Shared spool_item component uses this callback name
        {"spoolman_spool_item_clicked_cb", on_spool_item_cb},
        {"spoolman_spool_item_edit_cb", on_spool_item_edit_cb},
        {"ams_edit_tool_changed_cb", on_tool_changed_cb},
    });

    callbacks_registered_ = true;
    spdlog::debug("[AmsEditOverlay] Callbacks registered");
}

// ============================================================================
// Static Callbacks
// ============================================================================

AmsEditOverlay* AmsEditOverlay::get_instance_from_event(lv_event_t* /*e*/) {
    // Process-lifetime singleton — the accessor IS the instance resolution.
    return &get_ams_edit_overlay();
}

void AmsEditOverlay::on_back_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_back();
    }
}

void AmsEditOverlay::handle_setup_entry() {
    spdlog::debug("[AmsEditOverlay] Setup entry tapped - opening spool-edit");
    enter_spool_edit();
}

void AmsEditOverlay::on_setup_entry_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_setup_entry();
    }
}

void AmsEditOverlay::on_card_clicked_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_card_clicked();
    }
}

void AmsEditOverlay::on_change_filament_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_change_filament();
    }
}

void AmsEditOverlay::on_save_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_save();
    }
}

void AmsEditOverlay::on_print_label_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (!self)
        return;
#if HELIX_HAS_LABEL_PRINTER
    self->handle_print_label();
#endif
}

void AmsEditOverlay::on_scan_qr_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        self->handle_scan_qr();
    }
}

void AmsEditOverlay::on_picker_search_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
        const char* text = lv_textarea_get_text(ta);
        self->handle_picker_search(text);
    }
}

void AmsEditOverlay::on_picker_retry_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        spdlog::info("[AmsEditOverlay] Picker retry requested by user");
        self->populate_picker();
    }
}

void AmsEditOverlay::on_tool_changed_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (self) {
        auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
        int index = lv_dropdown_get_selected(dropdown);
        self->handle_tool_changed(index);
    }
}

void AmsEditOverlay::on_spool_item_edit_cb(lv_event_t* e) {
    // Pencil on the current spool's row: edit THIS filament's identity
    // (spec §3.3 reachability) instead of picking a different spool.
    auto* self = get_instance_from_event(e);
    if (self) {
        spdlog::debug("[AmsEditOverlay] Edit pencil tapped - editing current spool identity");
        self->enter_spool_edit();
    }
}

void AmsEditOverlay::on_spool_item_cb(lv_event_t* e) {
    auto* self = get_instance_from_event(e);
    if (!self) {
        return;
    }

    // Use current_target (the button with the handler), not target (the clicked child)
    lv_obj_t* item = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!item) {
        return;
    }
    auto spool_id = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(item)));
    if (spool_id <= 0) {
        spdlog::warn("[AmsEditOverlay] Spool item clicked with invalid spool_id={}", spool_id);
        return;
    }
    self->handle_spool_selected(spool_id);
}

} // namespace helix::ui
