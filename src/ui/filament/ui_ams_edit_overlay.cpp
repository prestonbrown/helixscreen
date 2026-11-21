// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_edit_overlay.h"

#include "ui_ams_context_menu.h"
#include "ui_ams_detail.h"
#include "ui_ams_lane_settings.h"
#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_color_picker.h"
#include "ui_component_keypad.h"
#include "ui_error_reporting.h"
#include "ui_filament_catalog_picker.h"
#include "ui_hsv_picker.h"
#include "ui_nav.h"
#include "ui_row_text.h"
#include "ui_swatch.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_tag_reading.h"
#include "app_globals.h"
#include "color_utils.h"
#include "core/observer_factory.h"
#include "core/static_panel_registry.h"
#include "core/text_io.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "filament_database.h"
#include "filament_display_name.h"
#include "filament_variants.h"
#include "format_utils.h"
#include "label_printer_settings.h"
#include "lane_spool_history.h"
#include "lane_translation.h"
#include "spool_assign.h"
#include "spoolman_manager.h"
#include "ui/ui_widget_helpers.h"
#if HELIX_HAS_LABEL_PRINTER
#include "ipp_print_modal.h"
#include "label_printer_utils.h"
#endif
#include "ui_breakpoint.h"
#include "ui_overlay_qr_scanner.h"
#include "ui_spool_canvas.h"
#include "ui_toast_manager.h"

#include "core/subject.h"
#include "core/temperature_units.h"
#include "display_format.h"
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
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <set>

namespace helix::ui {

namespace {

/// Move every element of @p recents that appears in @p base (case-insensitive)
/// to the front, keeping both lists' own orders otherwise.
std::vector<std::string> hoist_recents(const std::vector<std::string>& base,
                                       const std::vector<std::string>& recents) {
    std::vector<std::string> out;
    out.reserve(base.size());
    std::set<std::string> taken;
    for (const auto& recent : recents) {
        for (const auto& item : base) {
            if (helix::text_io::equals_ignore_case(item, recent) &&
                taken.insert(helix::text_io::to_lower(item)).second) {
                out.push_back(item);
            }
        }
    }
    for (const auto& item : base) {
        if (taken.insert(helix::text_io::to_lower(item)).second) {
            out.push_back(item);
        }
    }
    return out;
}

} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsEditOverlay::AmsEditOverlay()
    : picker_search_debounce_(
          [this](const std::string& query) { handle_picker_search(query.c_str()); }) {}

AmsEditOverlay::~AmsEditOverlay() {
    // Deinitialize subjects first to disconnect observers [L041]
    deinit_subjects();
}

lv_obj_t* AmsEditOverlay::find_widget(const char* name) const {
    return overlay_root_ ? lv_obj_find_by_name(overlay_root_, name) : nullptr;
}

// ============================================================================
// Pure selection decisions
// ============================================================================

AmsEditOverlay::SpoolEditSelection AmsEditOverlay::selection_from_slot(const SlotInfo& info) {
    SpoolEditSelection sel;
    sel.family = filament::display_family(info.material);
    if (sel.family.empty()) {
        sel.family = "PLA"; // a slot with no material at all opens on the common case
    }
    sel.variant = filament::variant_label(info.material, sel.family);
    sel.brand = info.brand.empty() ? "Generic" : info.brand;
    sel.product_id = info.catalog_id;
    return sel;
}

AmsEditOverlay::SpoolEditSelection
AmsEditOverlay::apply_family_change(const SpoolEditSelection& sel, const std::string& new_family,
                                    const printer::FilamentCatalog& catalog) {
    SpoolEditSelection out;
    out.family = new_family;
    out.variant = "Basic";
    // Keep the brand when it makes the new material, else Generic.
    const auto makers = catalog.brands_in_family(new_family);
    bool maker = helix::text_io::equals_ignore_case(sel.brand, "Generic");
    for (const auto& brand : makers) {
        if (helix::text_io::equals_ignore_case(brand, sel.brand)) {
            maker = true;
            break;
        }
    }
    out.brand = maker ? sel.brand : "Generic";
    // Any previously chosen product described the old material.
    out.product_id = "";
    return out;
}

std::string AmsEditOverlay::selected_type(const SpoolEditSelection& sel,
                                          const printer::FilamentCatalog& catalog) {
    for (const auto& variant : catalog.variants_in_family(sel.family)) {
        if (helix::text_io::equals_ignore_case(variant.label, sel.variant)) {
            return variant.sample_type;
        }
    }
    return filament::compose_family_type(sel.family, sel.variant);
}

// ============================================================================
// Public API
// ============================================================================

bool AmsEditOverlay::show_for_slot(lv_obj_t* parent, int slot_index, const SlotInfo& initial_info,
                                   IMoonrakerAPI* api, CompletionCallback on_complete,
                                   bool open_on_picker) {
    // A save for this slot that outlived its editor has not committed yet;
    // an editor opened now would show, and could save, the pre-save slot.
    if (save_pending_for(slot_index)) {
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Still saving this spool…"), 2500);
        return false;
    }
    // Every piece of per-session state starts from THIS slot. A picker-entry
    // session never runs enter_editor() up front, so nothing may be left for
    // it to inherit from the previous slot.
    // A session still owed its completion ends here, unsaved: reopened while
    // it was sliding out, the overlay stays open and that close never runs.
    if (completion_callback_) {
        fire_completion(false);
    }
    ++session_;
    slot_index_ = slot_index;
    original_info_ = initial_info;
    working_info_ = initial_info;
    note_action_form_.reset();
    api_ = api;
    completion_callback_ = std::move(on_complete);
    completion_fired_ = false;
    save_in_flight_ = false;
    set_save_state(kSaveIdle);
    editor_entered_ = false;
    sel_ = selection_from_slot(working_info_);
    selection_moved_ = false;
    identity_reset_ = false;
    details_color_ = working_info_.color_rgb;
    details_color_set_ = false;
    custom_color_ = working_info_.color_rgb;
    tag_.reset();
    linked_fetch_pending_ = false;
    recents_pending_ = false;
    recents_loaded_ = false;
    recent_families_.clear();
    recent_brands_.clear();
    reseed_details_for_spool();
    cached_spools_.clear();
    cached_searchables_.clear();
    listed_.clear();
    history_entries_.clear();
    listed_query_.reset();
    picker_pending_ = false;
    edited_spoolman_record_ = false;
    spools_from_spoolman_ = false;
    picker_search_debounce_.cancel();
    size_chip_ = 0;
    size_remaining_g_ = spool_assign::kDefaultSpoolGrams;
    size_total_g_ = spool_assign::kDefaultSpoolGrams;
    created_spool_id_ = 0;

    // Always prefer the active screen so the overlay renders above everything
    lv_obj_t* screen = lv_screen_active();
    if (!show(screen ? screen : parent)) {
        spdlog::error("[AmsEditOverlay] Failed to push overlay for slot {}", slot_index);
        return false;
    }
    lv_subject_set_int(&more_open_subject_, 0);
    lv_subject_set_int(&lane_open_subject_, 0);
    // After show(): the first one initialises the subjects this writes.
    load_lane_settings();

    // Reset per-session view state HERE (covered-safe — on_deactivate must not
    // touch it, since it also fires when the QR scanner merely covers us). A
    // lane holding a spool opens on Edit Spool; an empty one, or a request to
    // pick a spool, on Assign Spool.
    const bool edit = !open_on_picker &&
                      spool_assign::screen_for_lane(initial_info) == spool_assign::LaneScreen::Edit;
    lv_subject_set_int(&mode_subject_, edit ? kModeEdit : kModeAssign);
    lv_subject_set_int(&size_chip_subject_, size_chip_);
    if (!edit && spool_assign::screen_for_lane(initial_info) == spool_assign::LaneScreen::Edit) {
        // Asked to pick a spool for a lane that holds one.
        begin_fresh_assign();
    }
    enter_editor();
    if (!edit) {
        populate_picker();
    }

    spdlog::info("[AmsEditOverlay] Shown for slot {} (spoolman_id={}, brand={}, material={})",
                 slot_index, initial_info.spoolman_id, initial_info.brand, initial_info.material);
    return true;
}

void AmsEditOverlay::fetch_linked_spool() {
    // Gate on Spoolman availability: firing the fetch on a Spoolman-less
    // printer is one doomed RPC + warn log per overlay open.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    const bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    if (!has_spoolman || working_info_.spoolman_id <= 0 || !api_) {
        return;
    }
    const int spool_id = working_info_.spoolman_id;
    linked_fetch_pending_ = true;
    auto token = lifetime_.token();
    api_->spoolman().get_spoolman_spool(
        spool_id,
        [this, token, spool_id](const std::optional<SpoolInfo>& spool) {
            // token.defer() gates liveness; no bare expired() check (L081).
            token.defer([this, spool_id, spool]() {
                linked_fetch_pending_ = false;
                if (spool) {
                    apply_linked_spool(spool_id, *spool);
                }
            });
        },
        [this, token, spool_id](const MoonrakerError& err) {
            spdlog::warn("[AmsEditOverlay] Failed to fetch spool {}: {}", spool_id, err.message);
            token.defer([this]() { linked_fetch_pending_ = false; });
        });
}

void AmsEditOverlay::apply_linked_spool(int spool_id, const SpoolInfo& spool) {
    if (working_info_.spoolman_id != spool_id) {
        return; // the slot was relinked while this was in flight
    }

    // Identity: the server's record is authoritative for the spool the editor
    // opened on, so it feeds the original (the diff base) and, unless the
    // user already picked or reset an identity, the working copy. A relinked
    // spool already carries its record's identity.
    if (original_info_.spoolman_id == spool_id) {
        const bool keep_working = selection_moved_ || identity_reset_;
        // Where the lane's firmware owns its identity and this record
        // contradicts it, the lane's values stand and the note names the spool.
        const AmsBackend* backend = AmsState::instance().get_backend();
        const bool lane_values_stand = backend && backend->firmware_owns_lane_identity() &&
                                       contradiction_with_spool(original_info_, spool).any();
        if (spool.filament_id > 0) {
            working_info_.spoolman_filament_id = spool.filament_id;
        }
        if (spool.vendor_id > 0) {
            working_info_.spoolman_vendor_id = spool.vendor_id;
        }
        // The record states the filament whole, so an id it no longer carries
        // goes too.
        working_info_.spoolman_orca_filament_id = spool.orca_filament_id;
        std::vector<SlotInfo*> targets{&original_info_};
        if (!keep_working) {
            targets.push_back(&working_info_);
        }
        for (SlotInfo* info : targets) {
            if (spool.filament_id > 0) {
                info->spoolman_filament_id = spool.filament_id;
            }
            if (spool.vendor_id > 0) {
                info->spoolman_vendor_id = spool.vendor_id;
            }
            info->spoolman_orca_filament_id = spool.orca_filament_id;
            if (lane_values_stand) {
                continue;
            }
            if (!spool.vendor.empty()) {
                info->brand = spool.vendor;
            }
            if (!spool.material.empty()) {
                info->material = spool.material;
            }
            uint32_t rgb = 0;
            if (!spool.color_hex.empty() && helix::parse_hex_color(spool.color_hex.c_str(), rgb)) {
                info->color_rgb = rgb;
            }
        }
        spdlog::debug("[AmsEditOverlay] Synced spool {} from Spoolman: vendor='{}', material='{}'",
                      spool_id, working_info_.brand, working_info_.material);
        // A pick or a reset made before the record landed stays the user's.
        if (!keep_working) {
            sel_ = selection_from_slot(working_info_);
            if (!details_color_set_) {
                details_color_ = working_info_.color_rgb;
            }
        }
    }

    // More fields: the record's values, except a field already typed into.
    const SpoolInfo seed = detail_original_;
    const SpoolInfo typed = detail_working_;
    detail_original_ = spool;
    detail_working_ = spool;
    if (typed.price != seed.price) {
        detail_working_.price = typed.price;
    }
    if (typed.location != seed.location) {
        detail_working_.location = typed.location;
    }
    if (typed.lot_nr != seed.lot_nr) {
        detail_working_.lot_nr = typed.lot_nr;
    }
    if (typed.comment != seed.comment) {
        detail_working_.comment = typed.comment;
    }
    details_spool_id_ = spool_id;

    if (editor_entered_) {
        rebuild_option_lists();
        update_summary();
        update_temp_display();
        populate_detail_fields();
        update_sync_button_state();
    }
}

void AmsEditOverlay::return_to_editor() {
    if (!editor_entered_) {
        enter_editor();
        return;
    }
    set_view(kViewEditor);
}

void AmsEditOverlay::apply_scanned_spool(const SpoolInfo& spool) {
    SlotInfo scanned;
    apply_spool_to_slot(scanned, spool);
    scanned.slot_index = working_info_.slot_index;
    scanned.global_index = working_info_.global_index;
    working_info_ = scanned;
    // A scanned spool is known whole: it opens on Edit Spool for review.
    set_mode(kModeEdit);
    load_swapped_spool();
}

void AmsEditOverlay::load_swapped_spool() {
    // The More section now describes the NEW spool: blank until its record
    // lands, so Save cannot write the previous spool's fields onto it.
    reseed_details_for_spool();
    sel_ = selection_from_slot(working_info_);
    selection_moved_ = false;
    details_color_ = working_info_.color_rgb;
    details_color_set_ = false;
    if (!editor_entered_) {
        enter_editor();
        return;
    }
    set_view(kViewEditor);
    update_ui();
    rebuild_option_lists();
    update_summary();
    populate_detail_fields();
    refresh_tag_state();
    fetch_linked_spool();
}

// ============================================================================
// OverlayBase Hooks
// ============================================================================

lv_obj_t* AmsEditOverlay::create(lv_obj_t* parent) {
    if (!OverlayBase::create(parent)) {
        return nullptr;
    }

    // The header title (the lane name, "Gate 3") is subject-written; bind it
    // here once per tree rather than through overlay_panel's title_subject
    // forward, which the shared header_bar applies at its own build time.
    lv_obj_t* header_title = find_widget("header_title");
    if (header_title) {
        lv_label_bind_text(header_title, &title_subject_, nullptr);
    }

    // header_bar's optional trailing badge: the Spoolman mark + "#19". Wired
    // from C++ like the title, so the shared header component carries no
    // bind_text of its own.
    lv_obj_t* badge_text = find_widget("header_title_badge_text");
    if (badge_text) {
        lv_label_bind_text(badge_text, &spoolman_id_subject_, nullptr);
    }

    // Bind the summary colour dot's painter to the pending colour: the dot is
    // a color_dot, painted imperatively from update_summary().
    lv_obj_t* hsv = helix::ui::find_required(overlay_root_, "ams_color_hsv", get_name());
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

void AmsEditOverlay::on_ui_destroyed() {}

void AmsEditOverlay::on_dismissed() {
    // Idempotent: Save and Back have already fired it on their own paths.
    fire_completion(false);
}

void AmsEditOverlay::on_activate() {
    OverlayBase::on_activate();
    update_ui();
    // The push sets the overlay's final width after the editor was built, so
    // the colour rows are counted again against the width they show at.
    size_color_grids();
    // A cover (keypad, scanner) dropped any open-time fetch still in flight;
    // re-arm the ones that never landed.
    if (editor_entered_) {
        if (!linked_fetch_pending_ && details_spool_id_ != working_info_.spoolman_id) {
            fetch_linked_spool();
        }
        if (!recents_pending_ && !recents_loaded_) {
            fetch_recents();
        }
        // Assign Spool's list load died with the cover too.
        if (picker_pending_ && lv_subject_get_int(&mode_subject_) == kModeAssign) {
            populate_picker();
        }
    }
}

void AmsEditOverlay::on_deactivating(DeactivateReason reason) {
    // Fires when POPPED **and** when COVERED (e.g. QR scanner pushed on top).
    // Must NOT fire completion or reset the view subject — session state
    // resets happen in show_for_slot(). A pending debounced search belongs to
    // the session being left.
    picker_search_debounce_.cancel();
    // Leaving for real, lifetime_ is invalidated right after this hook and its
    // fetches die here. Behind the screensaver they still land.
    if (reason != DeactivateReason::Suspended) {
        linked_fetch_pending_ = false;
        recents_pending_ = false;
    }
    spdlog::debug("[AmsEditOverlay] on_deactivating()");
}

// ============================================================================
// Subject Management
// ============================================================================

void AmsEditOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        title_subject_.init_string(title_buf_, nullptr, sizeof(title_buf_), "--");
        subjects_.register_subject(&title_subject_);
        UI_MANAGED_SUBJECT_STRING(subtitle_subject_, subtitle_buf_, "", "ams_edit_subtitle",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(mode_subject_, kModeAssign, "ams_edit_mode", subjects_);
        UI_MANAGED_SUBJECT_STRING(list_title_subject_, list_title_buf_, "", "ams_edit_list_title",
                                  subjects_);
        UI_MANAGED_SUBJECT_INT(common_visible_subject_, 0, "ams_edit_common_visible", subjects_);
        UI_MANAGED_SUBJECT_INT(bottom_action_subject_,
                               static_cast<int>(spool_assign::BottomAction::AssignLocal),
                               "ams_edit_bottom_action", subjects_);
        UI_MANAGED_SUBJECT_INT(size_chip_subject_, 0, "ams_edit_size_chip", subjects_);
        UI_MANAGED_SUBJECT_STRING(partial_text_subject_, partial_text_buf_, "",
                                  "ams_edit_partial_text", subjects_);
        UI_MANAGED_SUBJECT_STRING(saves_to_subject_, saves_to_buf_, "", "ams_edit_saves_to",
                                  subjects_);

        // View state (kViewEditor..kViewColor) - registered globally
        UI_MANAGED_SUBJECT_INT(view_mode_subject_, 0, "ams_edit_view", subjects_);

        // The spools list's state (picker_state_subject_) - registered globally
        UI_MANAGED_SUBJECT_INT(picker_state_subject_, 0, "edit_picker_state", subjects_);

        // Header Save button dirty gate (1=disabled). Starts disabled — nothing
        // is dirty when the editor opens.
        UI_MANAGED_SUBJECT_INT(save_disabled_subject_, 1, "ams_edit_save_disabled", subjects_);

        UI_MANAGED_SUBJECT_INT(save_state_subject_, kSaveIdle, "ams_edit_save_state", subjects_);
        UI_MANAGED_SUBJECT_STRING(save_error_subject_, save_error_buf_, "", "ams_edit_save_error",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(save_wait_text_subject_, save_wait_text_buf_, "Saving...",
                                  "ams_edit_save_wait_text", subjects_);

        // Managed-vs-untracked signal: drives the header Spoolman badge, the
        // More section's Spoolman-owned fields and the "Saves to" line.
        UI_MANAGED_SUBJECT_INT(is_managed_subject_, 0, "ams_edit_is_managed", subjects_);
        UI_MANAGED_SUBJECT_INT(spool_disagrees_subject_, 0, "ams_edit_spool_disagrees", subjects_);
        UI_MANAGED_SUBJECT_STRING(spool_disagreement_text_subject_, spool_disagreement_buf_, "",
                                  "ams_edit_spool_disagreement_text", subjects_);

        // Scan QR reachability: Spoolman availability AND a camera on this
        // build. The header button binds its hidden flag to this.
        UI_MANAGED_SUBJECT_INT(scan_enabled_subject_, 0, "ams_edit_scan_enabled", subjects_);

        // Row visibility for the Generic-gated product dropdown.
        UI_MANAGED_SUBJECT_INT(product_hidden_subject_, 1, "ams_edit_product_hidden", subjects_);
        UI_MANAGED_SUBJECT_INT(color_selected_subject_, -1, "ams_edit_color_selected", subjects_);
        UI_MANAGED_SUBJECT_INT(color_row_count_subject_, 7, "ams_edit_color_row_count", subjects_);
        // Colour view: both panes at once where they fit (update_color_fit),
        // else one at a time, switched by the custom swatch and Presets.
        UI_MANAGED_SUBJECT_INT(color_fits_subject_, 1, "ams_edit_color_fits", subjects_);
        UI_MANAGED_SUBJECT_INT(color_pane_subject_, 0, "ams_edit_color_pane", subjects_);

        // Temperatures, remaining weight, badges, disclosure.
        UI_MANAGED_SUBJECT_INT(tag_badge_visible_subject_, 0, "ams_edit_tag_badge_visible",
                               subjects_);
        // Per-field "from tag" badges: the value the form shows is the tag's.
        UI_MANAGED_SUBJECT_INT(tag_material_subject_, 0, "ams_edit_tag_material", subjects_);
        UI_MANAGED_SUBJECT_INT(tag_brand_subject_, 0, "ams_edit_tag_brand", subjects_);
        UI_MANAGED_SUBJECT_INT(tag_product_subject_, 0, "ams_edit_tag_product", subjects_);
        UI_MANAGED_SUBJECT_INT(tag_color_subject_, 0, "ams_edit_tag_color", subjects_);
        UI_MANAGED_SUBJECT_INT(tag_temps_subject_, 0, "ams_edit_tag_temps", subjects_);
        UI_MANAGED_SUBJECT_STRING(temp_text_subject_, temp_buf_, "", "ams_edit_temp_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(weight_text_subject_, weight_buf_, "", "ams_edit_weight_text",
                                  subjects_);
        UI_MANAGED_SUBJECT_STRING(weight_hint_subject_, weight_hint_buf_, "",
                                  "ams_edit_weight_hint", subjects_);
        UI_MANAGED_SUBJECT_INT(reset_tag_visible_subject_, 0, "ams_edit_reset_tag_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(clear_visible_subject_, 0, "ams_edit_clear_visible", subjects_);
        UI_MANAGED_SUBJECT_INT(more_open_subject_, 0, "ams_edit_more_open", subjects_);
        // The two smallest classes fold the lane's settings behind a disclosure.
        UI_MANAGED_SUBJECT_INT(lane_open_subject_, 0, "ams_edit_lane_open", subjects_);

        // Spoolman spool number for the header badge ("#19").
        UI_MANAGED_SUBJECT_STRING(spoolman_id_subject_, spoolman_id_buf_, "",
                                  "ams_edit_spoolman_id", subjects_);

        // The lane's Assigned Tool and Endless Spool rows (load_lane_settings).
        UI_MANAGED_SUBJECT_INT(lane_visible_subject_, 0, "ams_edit_lane_visible", subjects_);
        UI_MANAGED_SUBJECT_INT(tool_row_visible_subject_, 0, "ams_edit_tool_row_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(backup_row_visible_subject_, 0, "ams_edit_backup_row_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(tool_editable_subject_, 1, "ams_edit_tool_editable", subjects_);
        UI_MANAGED_SUBJECT_STRING(tool_refusal_subject_, tool_refusal_buf_, "",
                                  "ams_edit_tool_refusal", subjects_);
        UI_MANAGED_SUBJECT_INT(backup_editable_subject_, 0, "ams_edit_backup_editable", subjects_);
        UI_MANAGED_SUBJECT_STRING(backup_label_subject_, backup_label_buf_, "",
                                  "ams_edit_backup_label", subjects_);

        // Expose the label-printer readiness flag to XML so
        // btn_detail_print_label can bind its `hidden` flag declaratively and
        // track pairing/unpairing while the overlay is open.
        lv_xml_register_subject(nullptr, "label_printer_configured",
                                helix::label_printer_configured_subject());
    });
}

void AmsEditOverlay::deinit_subjects() {
    save_wait_action_observer_.reset();
    deinit_subjects_base(subjects_);
}

// ============================================================================
// Identity dropdowns
// ============================================================================

void AmsEditOverlay::fill_dropdown(const char* name, const std::vector<Option>& options_list,
                                   const std::string& selected_payload) {
    std::string options;
    int selected = -1;
    for (size_t i = 0; i < options_list.size(); ++i) {
        if (!options.empty()) {
            options += '\n';
        }
        options += options_list[i].label;
        if (!options_list[i].more && selected < 0 &&
            helix::text_io::equals_ignore_case(options_list[i].payload, selected_payload)) {
            selected = static_cast<int>(i);
        }
    }
    lv_obj_t* dd = find_widget(name);
    if (!dd) {
        return;
    }
    if (options_list.empty()) {
        // No lines for this brand: an empty list, and no phantom first-item
        // selection.
        lv_dropdown_set_options(dd, "");
        return;
    }
    lv_dropdown_set_options(dd, options.c_str());
    lv_dropdown_set_selected(dd, selected < 0 ? 0 : selected);
}

void AmsEditOverlay::refresh_dropdowns() {
    fill_dropdown("dropdown_material", material_options_, sel_.family);
    fill_dropdown("dropdown_variant", variant_options_, sel_.variant);
    fill_dropdown("dropdown_brand", brand_options_, sel_.brand);
    fill_dropdown("dropdown_product", product_options_, sel_.product_id);
}

std::optional<std::vector<std::string>> AmsEditOverlay::firmware_materials() {
    const AmsBackend* backend = AmsState::instance().get_backend();
    auto materials = backend ? backend->get_supported_materials() : std::nullopt;
    if (materials && materials->empty()) {
        materials.reset();
    }
    return materials;
}

void AmsEditOverlay::rebuild_option_lists() {
    if (!catalog_) {
        return;
    }
    // Firmware that validates the material against its own list is offered
    // only what it accepts; the slot's current value still shows below.
    const auto allowed = firmware_materials();

    // MATERIAL: recents hoisted over all families, then "Browse catalog...".
    std::vector<Option> material;
    std::vector<std::string> families =
        hoist_recents(allowed ? catalog_->families_allowing(*allowed) : catalog_->all_families(),
                      recent_families_);
    if (recent_families_.empty()) {
        // No recents yet: the common families lead so the row opens useful.
        const char* const common[] = {"PLA", "PETG", "ABS", "ASA", "PA", "TPU", "PC"};
        std::vector<std::string> lead;
        for (const char* fam : common) {
            for (const auto& f : families) {
                if (helix::text_io::equals_ignore_case(f, fam)) {
                    lead.push_back(f);
                }
            }
        }
        std::set<std::string> taken;
        for (const auto& f : lead) {
            taken.insert(helix::text_io::to_lower(f));
        }
        for (const auto& f : families) {
            if (taken.insert(helix::text_io::to_lower(f)).second) {
                lead.push_back(f);
            }
        }
        families = std::move(lead);
    }
    bool family_known = false;
    for (const auto& fam : families) {
        material.push_back({fam, fam, false});
        if (helix::text_io::equals_ignore_case(fam, sel_.family)) {
            family_known = true;
        }
    }
    if (!family_known) {
        material.insert(material.begin(), {sel_.family, sel_.family, false});
    }
    material.push_back({lv_tr("Browse catalog..."), {}, true});
    material_options_ = std::move(material);

    // VARIANT: the catalog's variants for the family; a variant the catalog
    // lacks leads, selected, like an unknown family.
    std::vector<Option> variants;
    bool variant_known = false;
    for (const auto& v : catalog_->variants_in_family(sel_.family)) {
        if (!printer::type_allowed(allowed, v.sample_type)) {
            continue;
        }
        variants.push_back({v.label, v.label, false});
        variant_known = variant_known || helix::text_io::equals_ignore_case(v.label, sel_.variant);
    }
    if (!variant_known && !sel_.variant.empty()) {
        variants.insert(variants.begin(), {sel_.variant, sel_.variant, false});
    }
    variant_options_ = std::move(variants);

    // BRAND: Generic, then recents, then the rest, then "Browse catalog...".
    std::vector<Option> brands;
    brands.push_back({"Generic", "Generic", false});
    std::vector<std::string> makers = catalog_->brands_in_family(sel_.family);
    std::vector<std::string> ordered = hoist_recents(makers, recent_brands_);
    bool brand_known = helix::text_io::equals_ignore_case(sel_.brand, "Generic");
    for (const auto& brand : ordered) {
        if (helix::text_io::equals_ignore_case(brand, "Generic")) {
            continue; // already leads the list
        }
        brands.push_back({brand, brand, false});
        if (helix::text_io::equals_ignore_case(brand, sel_.brand)) {
            brand_known = true;
        }
    }
    if (!brand_known) {
        brands.insert(brands.begin() + 1, {sel_.brand, sel_.brand, false});
    }
    brands.push_back({lv_tr("Browse catalog..."), {}, true});
    brand_options_ = std::move(brands);

    // PRODUCT: that brand's lines for the material. Hidden under Generic.
    std::vector<Option> products;
    if (!helix::text_io::equals_ignore_case(sel_.brand, "Generic")) {
        for (const auto* ef : catalog_->products_in_family(sel_.brand, sel_.family)) {
            products.push_back({ef->name, ef->id, false});
        }
        // Firmware that names the product without a catalog id (the CFS fork's
        // NAME) opens on the line of that name, or on the name itself when the
        // catalog has none. Only the slot's own product: once the user moves
        // the selection, the name describes what was there before.
        const bool named_without_id =
            !selection_moved_ && sel_.product_id.empty() && !working_info_.product_name.empty();
        bool name_listed = false;
        if (named_without_id) {
            for (const auto& option : products) {
                if (helix::text_io::equals_ignore_case(option.label, working_info_.product_name)) {
                    sel_.product_id = option.payload;
                    name_listed = true;
                    break;
                }
            }
            if (!name_listed) {
                products.insert(products.begin(), {working_info_.product_name, {}, false});
            }
        }
        // An unresolved saved product still shows (selected) so an untouched
        // open+save keeps it.
        bool product_known = sel_.product_id.empty();
        for (const auto& option : products) {
            if (option.payload == sel_.product_id) {
                product_known = true;
                break;
            }
        }
        if (!product_known) {
            const std::string label =
                working_info_.product_name.empty() ? sel_.product_id : working_info_.product_name;
            products.insert(products.begin(), {label, sel_.product_id, false});
        }
    }
    product_options_ = std::move(products);
    lv_subject_set_int(&product_hidden_subject_,
                       helix::text_io::equals_ignore_case(sel_.brand, "Generic") ? 1 : 0);

    refresh_dropdowns();
}

void AmsEditOverlay::handle_option_selected(int row, int index) {
    const std::vector<Option>* options_list = nullptr;
    switch (row) {
    case 0:
        options_list = &material_options_;
        break;
    case 1:
        options_list = &variant_options_;
        break;
    case 2:
        options_list = &brand_options_;
        break;
    case 3:
        options_list = &product_options_;
        break;
    default:
        return;
    }
    if (index < 0 || static_cast<size_t>(index) >= options_list->size()) {
        return;
    }
    const Option& option = (*options_list)[index];
    if (option.more) {
        // "Browse catalog...": the full catalog picker. A pick
        // there seeds family, variant, brand and product in one step.
        if (!catalog_sheet_) {
            catalog_sheet_ = std::make_unique<FilamentCatalogPickerModal>();
        }
        catalog_sheet_->show(
            lv_screen_active(), sel_.family, firmware_materials(),
            [](const helix::printer::EffectiveFilament& ef) {
                auto& editor = get_ams_edit_overlay();
                const std::string family = filament::display_family(ef.type);
                const std::string variant = filament::variant_label(ef.type, family);
                // Re-picking what is already selected is a no-op, like the
                // dropdowns.
                if (family == editor.sel_.family && variant == editor.sel_.variant &&
                    ef.brand == editor.sel_.brand && ef.id == editor.sel_.product_id) {
                    return;
                }
                editor.sel_.family = family;
                editor.sel_.variant = variant;
                editor.sel_.brand = ef.brand;
                editor.sel_.product_id = ef.id;
                editor.selection_moved_ = true;
                editor.rebuild_option_lists();
                editor.update_summary();
                editor.update_temp_display();
                editor.update_sync_button_state();
            });
        // The dropdown now reads "Browse catalog..."; put the real selection
        // back, so a cancelled sheet leaves the row as it was.
        refresh_dropdowns();
        return;
    }
    switch (row) {
    case 0:
        if (helix::text_io::equals_ignore_case(option.payload, sel_.family)) {
            return;
        }
        sel_ = apply_family_change(sel_, option.payload, *catalog_);
        break;
    case 1:
        if (helix::text_io::equals_ignore_case(option.payload, sel_.variant)) {
            return;
        }
        sel_.variant = option.payload;
        sel_.product_id.clear(); // the product described the old variant
        break;
    case 2:
        if (helix::text_io::equals_ignore_case(option.payload, sel_.brand)) {
            return;
        }
        sel_.brand = option.payload;
        sel_.product_id.clear(); // lines are per-brand
        break;
    case 3:
        if (option.payload == sel_.product_id) {
            return;
        }
        sel_.product_id = option.payload;
        break;
    default:
        return;
    }
    selection_moved_ = true;
    rebuild_option_lists();
    update_summary();
    update_temp_display();
    update_sync_button_state();
}

// ============================================================================
// View Switching
// ============================================================================

void AmsEditOverlay::set_view(int view) {
    lv_subject_set_int(&view_mode_subject_, view);
    update_sync_button_state();
}

void AmsEditOverlay::set_mode(int mode) {
    lv_subject_set_int(&mode_subject_, mode);
    update_header_and_panel();
}

void AmsEditOverlay::handle_different_spool() {
    if (!subjects_initialized_ || save_in_flight_) {
        return;
    }
    begin_fresh_assign();
    set_view(kViewEditor);
    set_mode(kModeAssign);
    update_ui();
    rebuild_option_lists();
    update_summary();
    update_temp_display();
    update_sync_button_state();
    populate_picker();
}

void AmsEditOverlay::begin_fresh_assign() {
    // Assign Spool describes the spool going in, not the one coming out: the
    // form starts empty, so the list opens on the recent spools. The lane's
    // own settings (tool, backup) are not the spool's and stay.
    SlotInfo fresh;
    fresh.slot_index = working_info_.slot_index;
    fresh.global_index = working_info_.global_index;
    fresh.color_rgb = kAmsDefaultSlotColor;
    fresh.remaining_weight_g = -1.0f;
    fresh.total_weight_g = -1.0f;
    working_info_ = fresh;
    sel_ = selection_from_slot(working_info_);
    selection_moved_ = false;
    details_color_ = working_info_.color_rgb;
    details_color_set_ = false;
    reseed_details_for_spool();
    apply_size_chip(0, spool_assign::kDefaultSpoolGrams, spool_assign::kDefaultSpoolGrams);
}

bool AmsEditOverlay::back_to_edit_spool() {
    // Assign Spool reached from a lane that holds a spool (Different Spool, or
    // a pick-a-spool entry): Back returns to that spool, unchanged.
    if (lv_subject_get_int(&mode_subject_) != kModeAssign ||
        spool_assign::screen_for_lane(original_info_) != spool_assign::LaneScreen::Edit) {
        return false;
    }
    working_info_ = original_info_;
    sel_ = selection_from_slot(working_info_);
    selection_moved_ = false;
    details_color_ = working_info_.color_rgb;
    details_color_set_ = false;
    reseed_details_for_spool();
    set_mode(kModeEdit);
    update_ui();
    rebuild_option_lists();
    update_summary();
    update_temp_display();
    populate_detail_fields();
    update_sync_button_state();
    fetch_linked_spool();
    return true;
}

void AmsEditOverlay::populate_picker() {
    if (!overlay_root_) {
        return;
    }
    picker_search_debounce_.cancel();
    cached_spools_.clear();
    cached_searchables_.clear();
    listed_.clear();
    history_entries_.clear();
    listed_query_.reset();

    if (lv_obj_t* search = find_widget("picker_search")) {
        lv_textarea_set_text(search, "");
    }

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    const bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    if (!api_) {
        api_ = get_moonraker_api();
    }
    if (!api_) {
        spools_from_spoolman_ = false;
        picker_pending_ = false;
        render_spool_list("");
        return;
    }
    picker_pending_ = true;
    if (!has_spoolman) {
        // Without Spoolman the list is what this printer's lanes have held.
        spools_from_spoolman_ = false;
        lv_subject_set_int(&picker_state_subject_, 0);
        auto token = lifetime_.token();
        lane_spool_history::load(
            *api_, [this, token](std::vector<lane_spool_history::Entry> entries) {
                token.defer("AmsEditOverlay::lane_history", [this, entries]() {
                    if (!overlay_root_ || !subjects_initialized_ || spools_from_spoolman_) {
                        return;
                    }
                    picker_pending_ = false;
                    cached_spools_.clear();
                    for (const auto& entry : entries) {
                        cached_spools_.push_back(lane_spool_history::as_spool(entry));
                    }
                    history_entries_ = entries;
                    render_spool_list("");
                });
            });
        return;
    }

    spools_from_spoolman_ = true;
    lv_subject_set_int(&picker_state_subject_, 0);
    auto token = lifetime_.token();
    api_->spoolman().get_spoolman_spools(
        [this, token](const std::vector<SpoolInfo>& spools) {
            spdlog::debug("[AmsEditOverlay] Spoolman returned {} spools", spools.size());
            token.defer([this, spools]() {
                if (!overlay_root_ || !subjects_initialized_) {
                    return;
                }
                picker_pending_ = false;
                // Spools arrive ordered by most recent activity first
                // (sort_spools_by_recency() in the API layer).
                cached_spools_ = spools;
                history_entries_.clear();
                cached_searchables_.clear();
                cached_searchables_.reserve(cached_spools_.size());
                for (const auto& spool : cached_spools_) {
                    cached_searchables_.push_back(build_searchable_text(spool));
                }
                render_spool_list("");
            });
        },
        [this, token](const MoonrakerError& err) {
            spdlog::warn("[AmsEditOverlay] Spoolman fetch error: {}", err.message);
            token.defer([this]() {
                if (!overlay_root_ || !subjects_initialized_) {
                    return;
                }
                picker_pending_ = false;
                lv_subject_set_int(&picker_state_subject_, 1);
                lv_subject_set_int(&bottom_action_subject_,
                                   static_cast<int>(spool_assign::bottom_action(true, 0)));
            });
        });
}

spool_assign::SpoolQuery AmsEditOverlay::current_query() const {
    spool_assign::SpoolQuery query;
    // An empty lane opened with nothing picked says nothing yet: the dropdowns'
    // resting values (PLA, Generic) are not a description.
    if (!selection_moved_ && !details_color_set_ && working_info_.material.empty()) {
        return query;
    }
    // Once the user says anything, the form says all it shows: a color pick
    // on an empty lane describes a spool of the material the dropdown names.
    const SlotInfo shown = shown_slot();
    query.material = shown.material;
    query.brand = shown.brand;
    query.product = shown.product_name;
    if (!selection_moved_ && shown.material.empty() && catalog_) {
        query.material = selected_type(sel_, *catalog_);
        query.brand = sel_.brand;
    }
    const bool colour_set =
        details_color_set_ || (shown.multi_color_hexes.empty() && slot_has_colour(shown));
    if (colour_set) {
        query.color = shown.color_rgb;
    }
    return query;
}

bool AmsEditOverlay::ListedQuery::operator==(const ListedQuery& o) const {
    return query == o.query && filter == o.filter && spool_count == o.spool_count &&
           in_use == o.in_use;
}

std::set<int> AmsEditOverlay::spools_in_other_lanes() const {
    // Spools another lane links are taken. The external spool feeds through no
    // lane, so every lane's spool is taken for it.
    std::set<int> in_use;
    if (AmsBackend* backend = AmsState::instance().get_backend()) {
        const AmsSystemInfo info = backend->get_system_info();
        for (int i = 0; i < info.total_slots; ++i) {
            const SlotInfo* lane = info.get_slot_global(i);
            if (lane && i != slot_index_ && lane->spoolman_id > 0) {
                in_use.insert(lane->spoolman_id);
            }
        }
    }
    return in_use;
}

void AmsEditOverlay::refresh_spool_list() {
    if (!overlay_root_ || lv_subject_get_int(&mode_subject_) != kModeAssign ||
        lv_subject_get_int(&picker_state_subject_) == 0 ||
        lv_subject_get_int(&picker_state_subject_) == 1) {
        return;
    }
    lv_obj_t* search = find_widget("picker_search");
    const char* text = search ? lv_textarea_get_text(search) : nullptr;
    // Most form taps (a chip, a weight) change nothing the list reads.
    if (listed_query_ &&
        *listed_query_ == ListedQuery{current_query(), text ? text : "", cached_spools_.size(),
                                      spools_in_other_lanes()}) {
        return;
    }
    render_spool_list(text ? text : "");
}

void AmsEditOverlay::render_spool_list(const std::string& filter) {
    lv_obj_t* spool_list = helix::ui::find_required(overlay_root_, "picker_spool_list", get_name());
    if (!spool_list) {
        return;
    }

    // Invoked from a token.defer() callback (UpdateQueue batch). Sync
    // lv_obj_clean in that context corrupts LVGL's event linked list (#776).
    helix::ui::safe_clean_children(spool_list);

    // The search narrows by text first; the form then ranks what is left.
    // A search keeps a copy of what it matched; no search reads the cache.
    std::vector<SpoolInfo> found;
    const std::vector<SpoolInfo>* searched = &cached_spools_;
    if (spools_from_spoolman_ && !filter.empty()) {
        found = filter_spools(cached_spools_, filter, cached_searchables_);
        searched = &found;
    }
    const spool_assign::SpoolQuery query = current_query();
    const auto ranked = spool_assign::rank_spools(query, *searched);
    listed_.clear();
    // Rows index cached_spools_. A search keeps Spoolman spools, found again
    // by id; the lane history is never searched, so its indices stand.
    for (const auto& r : ranked) {
        if (!spools_from_spoolman_) {
            listed_.push_back(r);
            continue;
        }
        const int id = (*searched)[r.index].id;
        for (size_t i = 0; i < cached_spools_.size(); ++i) {
            if (cached_spools_[i].id == id) {
                listed_.push_back({i, r.match});
                break;
            }
        }
    }

    const std::set<int> in_use = spools_in_other_lanes();
    listed_query_ = ListedQuery{query, filter, cached_spools_.size(), in_use};

    const std::string close_format = lv_tr("close: {}");
    const std::string close_color = lv_tr("similar color");
    for (size_t row = 0; row < listed_.size(); ++row) {
        const SpoolInfo& spool = cached_spools_[listed_[row].index];
        const bool close = listed_[row].match.kind == spool_assign::MatchKind::Close;
        const bool taken = spool.id > 0 && in_use.count(spool.id) > 0;
        const char* attrs[] = {"hide_close", close ? "false" : "true", "use_disabled",
                               taken ? "true" : "false", nullptr};
        lv_obj_t* item =
            static_cast<lv_obj_t*>(lv_xml_create(spool_list, "spool_assign_row", attrs));
        if (!item) {
            continue;
        }
        // DECLARATIVE_OK: per-row payload on a generated collection.
        lv_obj_set_user_data(item, reinterpret_cast<void*>(static_cast<intptr_t>(row)));

        helix::ui::set_row_label_text(item, "row_name", spool.display_name().c_str());
        std::string detail;
        if (spool.id > 0) {
            detail = "#" + std::to_string(spool.id);
        }
        if (spool.remaining_weight_g > 0) {
            detail += (detail.empty() ? "" : " · ") +
                      fmt::format(fmt::runtime(lv_tr("{:.0f} g left")), spool.remaining_weight_g);
        }
#if !defined(HELIX_PLATFORM_ESP32)
        // spoolman_types.cpp is scoped out of the ESP32 image; rows there
        // carry no age.
        if (const auto used = parse_spool_timestamp(spool.last_used)) {
            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            if (now > *used) {
                detail +=
                    (detail.empty() ? "" : " · ") +
                    display_format::format_relative_time(static_cast<uint64_t>(now - *used) * 1000);
            }
        }
#endif
        helix::ui::set_row_label_text(item, "row_detail", detail.c_str());
        if (close) {
            const std::string text = spool_assign::close_note(listed_[row].match.differs, spool,
                                                              close_format, close_color);
            helix::ui::set_row_label_text(item, "row_close", text.c_str());
        }
        if (lv_obj_t* canvas = lv_obj_find_by_name(item, "row_spool")) {
            const uint32_t rgb = helix::parse_hex_color(spool.color_hex).value_or(0x808080);
            spool_canvas_set_filament(canvas, rgb, spool.multi_color_hexes,
                                      helix::ui::swatch_finish(spool.finish()));
            const double total = spool.initial_weight_g;
            ui_spool_canvas_set_fill_level(
                canvas, total > 0 ? static_cast<float>(spool.remaining_weight_g / total) : 1.0f);
        }
    }

    const size_t spool_count = cached_spools_.size();
    int state = 2;
    if (spool_count == 0) {
        state = 4;
    } else if (listed_.empty()) {
        state = 3;
    }
    lv_subject_set_int(&picker_state_subject_, state);
    lv_subject_set_int(&common_visible_subject_,
                       spool_assign::show_common_spools(spool_count) ? 1 : 0);
    lv_subject_set_int(&bottom_action_subject_, static_cast<int>(spool_assign::bottom_action(
                                                    spools_from_spoolman_, listed_.size())));

    const char* title = nullptr;
    if (!query.empty()) {
        title = lv_tr("Matching Spools");
    } else if (spools_from_spoolman_) {
        title = lv_tr("Recent Spools");
    } else {
        title = lv_tr("Spools Used on This Printer");
    }
    snprintf(list_title_buf_, sizeof(list_title_buf_), "%s", title);
    lv_subject_set_string(&list_title_subject_, list_title_buf_);
}

bool AmsEditOverlay::commit_lane_settings_for_assign() {
    // Assign Spool has no Save of its own: each of its actions writes the
    // lane's settings first, as Edit Spool's Save does, or they would be lost
    // when it closes.
    if (commit_lane_settings()) {
        return true;
    }
    // The refusal has toasted; the field stays staged and the editor open.
    stay_open_after_failed_save();
    return false;
}

void AmsEditOverlay::handle_row_use(size_t row) {
    if (save_in_flight_ || row >= listed_.size()) {
        return;
    }
    if (!commit_lane_settings_for_assign()) {
        return;
    }
    const size_t index = listed_[row].index;
    const SpoolInfo spool = cached_spools_[index];
    spdlog::info("[AmsEditOverlay] Use spool {} ('{}') for slot {}", spool.id, spool.display_name(),
                 slot_index_);
    if (spool.id > 0) {
        apply_spool_to_slot(working_info_, spool);
    } else {
        // A lane-history spool: describe the lane with it, linked to nothing.
        working_info_.clear_spoolman_link();
        working_info_.material = spool.material;
        working_info_.brand = spool.vendor.empty() ? std::string("Generic") : spool.vendor;
        working_info_.product_name = spool.filament_name;
        working_info_.spool_name.clear();
        if (const auto rgb = helix::parse_hex_color(spool.color_hex)) {
            working_info_.color_rgb = *rgb;
            working_info_.color_name.clear();
            working_info_.multi_color_hexes.clear();
        }
        apply_material_defaults(spool.material);
        // The catalog product it named brings its own temperatures back.
        const std::string catalog_id =
            index < history_entries_.size() ? history_entries_[index].catalog_id : std::string();
        if (const auto* ef =
                (catalog_ && !catalog_id.empty()) ? catalog_->resolve_id(catalog_id) : nullptr) {
            working_info_.nozzle_temp_min = ef->nozzle_min;
            working_info_.nozzle_temp_max = ef->nozzle_max;
            working_info_.nozzle_temp_recommended = ef->nozzle_recommended;
            working_info_.bed_temp = ef->bed_temp;
            working_info_.catalog_id = ef->id;
        }
        working_info_.product_name = spool.filament_name;
        // A spool going in is the size the chips say.
        working_info_.total_weight_g = size_total_g_;
        working_info_.remaining_weight_g = size_remaining_g_;
    }
    commit_and_close();
}

void AmsEditOverlay::handle_common_spool(size_t index) {
    const auto& common = spool_assign::common_spools();
    if (save_in_flight_ || index >= common.size() || !catalog_) {
        return;
    }
    const auto& pick = common[index];
    SpoolEditSelection picked = apply_family_change(sel_, pick.material, *catalog_);
    picked.brand = "Generic";
    sel_ = picked;
    selection_moved_ = true;
    details_color_ = pick.rgb;
    details_color_set_ = true;
    rebuild_option_lists();
    update_summary();
    update_temp_display();
    update_sync_button_state();
}

void AmsEditOverlay::handle_size_chip(int chip) {
    if (chip == spool_assign::kPartialChip) {
        handle_weight_tap();
        return;
    }
    if (chip < 0 || chip >= spool_assign::kPartialChip) {
        return;
    }
    const float grams = spool_assign::kSizeChipGrams[chip];
    apply_size_chip(chip, grams, grams);
}

void AmsEditOverlay::apply_size_chip(int chip, float remaining_g, float total_g) {
    size_chip_ = chip;
    size_remaining_g_ = remaining_g;
    size_total_g_ = total_g;
    lv_subject_set_int(&size_chip_subject_, chip);
    update_summary();
}

void AmsEditOverlay::stage_assign_form() {
    // Assign Spool's form describes the spool going in, the values it rests
    // on included: an empty lane assigned with only a color picked is a spool
    // of the material the dropdown shows.
    if (lv_subject_get_int(&mode_subject_) == kModeAssign && working_info_.material.empty()) {
        selection_moved_ = true;
    }
    stage_selection_into_working();
    stage_pending_color_into_working();
    stage_size_into_working();
}

void AmsEditOverlay::stage_size_into_working() {
    if (lv_subject_get_int(&mode_subject_) != kModeAssign) {
        return;
    }
    working_info_.total_weight_g = size_total_g_;
    working_info_.remaining_weight_g = size_remaining_g_;
}

void AmsEditOverlay::handle_remaining_step(float delta_g) {
    if (save_in_flight_) {
        return;
    }
    const float from = std::max(0.0f, working_info_.remaining_weight_g);
    apply_weight(spool_assign::step_remaining(from, delta_g, working_info_.total_weight_g));
}

void AmsEditOverlay::handle_assign_local() {
    if (save_in_flight_ || !commit_lane_settings_for_assign()) {
        return;
    }
    stage_assign_form();
    // The lane leaves whatever Spoolman spool it held: it now holds a spool
    // Spoolman does not know.
    if (working_info_.spoolman_id > 0) {
        working_info_.clear_spoolman_link();
    }
    commit_and_close();
}

void AmsEditOverlay::show_assigned_toast(const SlotInfo& before, const SlotInfo& after,
                                         bool created, bool undoable) {
    struct Payload {
        int slot_index = -1;
        SlotInfo before;
        SlotInfo after;
        SpoolInfo label; ///< the spool Print Label prints
        /// AmsState::backend_generation() when the save closed: a backend
        /// change since means the slot index may name another printer's lane.
        uint64_t backend_generation = 0;
    };
    // Each toast gets its own payload: toasts stack, and Undo or Print Label on
    // an older one must act on its own lane. Payloads outlive their toasts (a
    // tap's action runs deferred); the oldest go once more are kept than the
    // toast stack can show.
    static std::deque<std::unique_ptr<Payload>> s_payloads;
    constexpr size_t kKeptPayloads = 8;
    s_payloads.push_back(std::make_unique<Payload>(Payload{slot_index_, before, after, {}}));
    while (s_payloads.size() > kKeptPayloads) {
        s_payloads.pop_front();
    }
    Payload* payload = s_payloads.back().get();
    payload->backend_generation = AmsState::instance().backend_generation();

    const std::string lane =
        slot_index_ == -2 ? std::string(lv_tr("External Filament"))
                          : helix::ui::lane_label(helix::ui::active_lane_noun(), slot_index_);
    const std::string name =
        helix::compose_filament_label(after.brand, after.spool_name, after.material, " ");
    const std::string message = lane + ": " + name;
    std::string detail;
    if (created && after.spoolman_id > 0) {
        detail = fmt::format(fmt::runtime(lv_tr("Added to Spoolman as #{}")), after.spoolman_id);
    }

    // Print Label, where a label printer is set up and the spool is Spoolman's
    // (a new one already printed its label; this is the reprint).
    helix::ToastAction print_label;
#if HELIX_HAS_LABEL_PRINTER
    if (after.spoolman_id > 0 && helix::LabelPrinterSettingsManager::instance().is_configured()) {
        payload->label = label_spool();
        print_label = helix::ToastAction{
            lv_tr("Print Label"),
            [](void* user_data) { print_label_for(static_cast<Payload*>(user_data)->label); },
            payload};
    }
#endif
    const char* detail_text = detail.empty() ? nullptr : detail.c_str();
    // A save that rewrote a Spoolman record cannot be undone from the lane
    // alone: putting the lane back would leave it contradicting its spool.
    if (!undoable) {
        if (print_label.callback) {
            ToastManager::instance().show_with_action(
                ToastSeverity::SUCCESS, message.c_str(), print_label.text, print_label.callback,
                print_label.user_data, 6000, nullptr, {}, detail_text);
        } else if (detail_text) {
            ToastManager::instance().show_with_detail(ToastSeverity::SUCCESS, message.c_str(),
                                                      detail_text, 6000);
        } else {
            ToastManager::instance().show(ToastSeverity::SUCCESS, message.c_str(), 6000);
        }
        return;
    }
    ToastManager::instance().show_with_action(
        ToastSeverity::SUCCESS, message.c_str(), lv_tr("Undo"),
        [](void* user_data) {
            const auto* undo = static_cast<Payload*>(user_data);
            AmsBackend* backend = AmsState::instance().get_backend();
            if (undo->slot_index == -1) {
                return;
            }
            if (undo->slot_index != -2 && (!backend || AmsState::instance().backend_generation() !=
                                                           undo->backend_generation)) {
                ToastManager::instance().show(
                    ToastSeverity::WARNING,
                    lv_tr("Not undone: the printer changed since that save."), 4000);
                return;
            }
            // Undo puts back what was there only while the lane still holds
            // what this save put in: anything that changed it since wins.
            const SlotInfo now =
                undo->slot_index == -2
                    ? AmsState::instance().get_external_spool_info().value_or(SlotInfo{})
                    : backend->get_slot_info(undo->slot_index);
            if (!spool_assign::undo_still_applies(now, undo->after)) {
                ToastManager::instance().show(
                    ToastSeverity::WARNING, lv_tr("Not undone: the lane changed since that save."),
                    4000);
                return;
            }
            spdlog::info("[AmsEditOverlay] Undo assign on slot {}", undo->slot_index);
            if (undo->slot_index == -2) {
                // The external spool has a commit of its own.
                AmsState::instance().commit_external_spool_edit(undo->before);
                return;
            }
            const AmsError err =
                AmsState::instance().commit_slot_edit(undo->slot_index, undo->after, undo->before);
            if (!err.success() && !err.partially_applied) {
                ToastManager::instance().show(
                    ToastSeverity::WARNING,
                    fmt::format(fmt::runtime(lv_tr("Not undone: {}")), err.user_msg).c_str(), 4000);
            }
        },
        payload, 6000, nullptr, print_label, detail_text);
}

void AmsEditOverlay::fetch_recents() {
    // Recents come from Spoolman history: the recency-sorted spool list. One
    // fetch on open; without Spoolman the rows stay in catalog order.
    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    if (!spoolman_subj || lv_subject_get_int(spoolman_subj) != 1) {
        return;
    }
    if (!api_) {
        return;
    }
    recents_pending_ = true;
    auto token = lifetime_.token();
    api_->spoolman().get_spoolman_spools(
        [this, token](const std::vector<SpoolInfo>& spools) {
            token.defer([this, spools]() {
                recents_pending_ = false;
                recents_loaded_ = true;
#if !defined(HELIX_PLATFORM_ESP32)
                // spoolman_types.cpp is scoped out of the ESP32 image; the
                // rows there stay in catalog order.
                recent_families_ = recent_material_families(spools);
                recent_brands_ = recent_brands(spools);
#endif
                // cached_spools_ doubles as the picker's warm cache; a later
                // populate_picker() refetches anyway.
                if (lv_subject_get_int(&view_mode_subject_) == kViewEditor) {
                    rebuild_option_lists();
                }
            });
        },
        [this, token](const MoonrakerError& err) {
            spdlog::debug("[AmsEditOverlay] recents fetch skipped: {}", err.message);
            token.defer([this]() { recents_pending_ = false; });
        });
}

void AmsEditOverlay::refresh_tag_state() {
    tag_.reset();
    auto* backend = AmsState::instance().get_backend();
    if (backend && slot_index_ >= 0) {
        tag_ = backend->tag_reading(slot_index_);
    }
    lv_subject_set_int(&reset_tag_visible_subject_, (tag_ && !tag_->empty()) ? 1 : 0);
    update_tag_badges();
}

SlotInfo AmsEditOverlay::shown_slot() const {
    // What the form shows right now: the staged selection, the pending
    // colour and the temperature line, over the working copy.
    SlotInfo shown = working_info_;
    if (selection_moved_ && catalog_) {
        shown.material = selected_type(sel_, *catalog_);
        shown.brand = sel_.brand;
        const auto* ef = sel_.product_id.empty() ? nullptr : catalog_->resolve_id(sel_.product_id);
        shown.product_name = ef ? ef->name : std::string();
    }
    if (details_color_set_) {
        shown.color_rgb = details_color_;
        shown.multi_color_hexes.clear();
    }
    shown.nozzle_temp_min = shown_nozzle_min_;
    shown.nozzle_temp_max = shown_nozzle_max_;
    shown.bed_temp = shown_bed_temp_;
    return shown;
}

void AmsEditOverlay::update_tag_badges() {
    if (!subjects_initialized_) {
        return;
    }
    ams::TagProvenance prov;
    if (tag_) {
        prov = ams::tag_provenance(*tag_, shown_slot());
    }
    using ams::TagField;
    lv_subject_set_int(&tag_badge_visible_subject_, prov.any() ? 1 : 0);
    lv_subject_set_int(&tag_material_subject_, prov.from_tag(TagField::Material) ? 1 : 0);
    lv_subject_set_int(&tag_brand_subject_, prov.from_tag(TagField::Brand) ? 1 : 0);
    lv_subject_set_int(&tag_product_subject_, prov.from_tag(TagField::Product) ? 1 : 0);
    lv_subject_set_int(&tag_color_subject_, prov.from_tag(TagField::Color) ? 1 : 0);
    lv_subject_set_int(
        &tag_temps_subject_,
        (prov.from_tag(TagField::NozzleTemp) || prov.from_tag(TagField::BedTemp)) ? 1 : 0);
}

void AmsEditOverlay::reseed_details_for_spool() {
    // Seed the logistics fields from what we know about the spool
    // working_info_ names (meaningful for managed slots; the section hides
    // otherwise, but seeding is harmless). A managed slot's open path then
    // refreshes these from the server record.
    detail_original_ = SpoolInfo{};
    detail_original_.id = working_info_.spoolman_id;
    detail_original_.filament_id = working_info_.spoolman_filament_id;
    detail_original_.vendor = working_info_.brand;
    detail_original_.material = working_info_.material;
    detail_original_.filament_name = working_info_.spool_name;
    detail_original_.remaining_weight_g = working_info_.remaining_weight_g;
    detail_original_.initial_weight_g = working_info_.total_weight_g;
    detail_original_.spool_weight_g = working_info_.total_weight_g;
    detail_working_ = detail_original_;
    details_spool_id_ = 0;
    logistics_pending_ = false;
}

void AmsEditOverlay::enter_editor() {
    catalog_ = printer::FilamentCatalog::load_full();
    editor_entered_ = true;

    set_view(kViewEditor);
    populate_editor_view();
    refresh_tag_state();
    if (!recents_loaded_ && !recents_pending_) {
        fetch_recents();
    }
    // Linked slots: one fetch refreshes the identity and the More fields.
    if (!linked_fetch_pending_ && details_spool_id_ != working_info_.spoolman_id) {
        fetch_linked_spool();
    }

    spdlog::debug("[AmsEditOverlay] Entered editor view");
}

bool AmsEditOverlay::slot_has_colour(const SlotInfo& info) {
    if (info.color_rgb == kAmsDefaultSlotColor) {
        return false;
    }
    // Black with nothing else known is the empty default some firmware
    // reports, not a colour anyone chose.
    return info.color_rgb != 0 || !info.color_name.empty() || !info.material.empty();
}

int AmsEditOverlay::color_ring_index(uint32_t rgb, bool colour_set, int visible_presets,
                                     const std::vector<uint32_t>& palette) {
    if (!colour_set) {
        return -1;
    }
    const int shown = std::min(visible_presets, static_cast<int>(palette.size()));
    for (int i = 0; i < shown; ++i) {
        if (palette[static_cast<size_t>(i)] == rgb) {
            return i;
        }
    }
    return kCustomColorIndex;
}

int AmsEditOverlay::visible_preset_count() {
    return lv_subject_get_int(&color_row_count_subject_);
}

int AmsEditOverlay::grid_preset_count(int grid_width, int tile, int gap, int rows,
                                      int palette_size) {
    if (tile <= 0 || rows <= 0) {
        return 0;
    }
    // The custom-colour swatch takes the first tile.
    const int per_row = std::max(1, (grid_width + gap) / (tile + gap));
    return std::clamp(per_row * rows - 1, 0, palette_size);
}

void AmsEditOverlay::size_color_grids() {
    // The grid is subject-counted (a literal <repeat> inside <if> is refused
    // by the engine), sized here from its measured width: one whole row, the
    // Custom swatch its last tile (Custom opens the rest).
    if (!overlay_root_) {
        return;
    }
    lv_obj_update_layout(overlay_root_);
    lv_obj_t* grid = find_widget("color_grid");
    lv_obj_t* custom = find_widget("btn_custom_color");
    if (!grid || !custom) {
        return;
    }
    const int palette = static_cast<int>(swatch_palette(ColorPicker::Palette::General).size());
    const int count =
        grid_preset_count(lv_obj_get_content_width(grid), lv_obj_get_width(custom),
                          lv_obj_get_style_pad_column(grid, LV_PART_MAIN), 1, palette);
    // A hidden grid measures 0 wide; it keeps the count it has.
    if (lv_obj_get_content_width(grid) > 0 &&
        lv_subject_get_int(&color_row_count_subject_) != count) {
        lv_subject_set_int(&color_row_count_subject_, count);
    }
    // The repeat rebuilds its swatches at the end of the grid; Custom closes it.
    lv_obj_move_to_index(custom, -1);
}

void AmsEditOverlay::populate_editor_view() {
    if (!catalog_) {
        catalog_ = printer::FilamentCatalog::load_full();
    }
    size_color_grids();
    rebuild_option_lists();
    update_summary();
    update_temp_display();
    populate_detail_fields();
    populate_lane_dropdowns();
}

void AmsEditOverlay::repopulate() {
    // The view subject is C++-owned, so it keeps its value across the rebuild
    // and the fresh widgets bind to the right branch on their own. What does
    // not survive is the content each view populates on entry.
    const int view = lv_subject_get_int(&view_mode_subject_);
    switch (view) {
    case kViewColor:
        populate_color_view();
        update_color_fit();
        break;
    case kViewEditor:
    default:
        populate_editor_view();
        refresh_tag_state();
        if (lv_subject_get_int(&mode_subject_) == kModeAssign) {
            refresh_spool_list();
        }
        break;
    }
    spdlog::debug("[AmsEditOverlay] Repopulated view {}", view);
}

// ============================================================================
// Summary / temps / weight
// ============================================================================

void AmsEditOverlay::update_summary() {
    update_spool_disagreement();

    // Ring the visible preset the pending colour matches. A real colour no
    // visible preset holds rings the custom swatch (index -2); no colour, or
    // a multi-colour split, rings nothing.
    const uint32_t pending = details_color_set_ ? details_color_ : working_info_.color_rgb;
    const bool colour_set = details_color_set_ || (working_info_.multi_color_hexes.empty() &&
                                                   slot_has_colour(working_info_));
    lv_subject_set_int(&color_selected_subject_,
                       color_ring_index(pending, colour_set, visible_preset_count(),
                                        swatch_palette(ColorPicker::Palette::General)));

    // Edit Spool: the remaining weight and the spool it is left on.
    if (working_info_.remaining_weight_g >= 0) {
        snprintf(weight_buf_, sizeof(weight_buf_), "%.0f g", working_info_.remaining_weight_g);
    } else {
        snprintf(weight_buf_, sizeof(weight_buf_), "—");
    }
    lv_subject_set_string(&weight_text_subject_, weight_buf_);
    if (lv_obj_t* spool = find_widget("edit_spool")) {
        SlotInfo drawn = working_info_;
        if (details_color_set_) {
            drawn.color_rgb = details_color_;
            drawn.multi_color_hexes.clear();
        }
        spool_canvas_set_filament(spool, drawn);
        const float total = working_info_.total_weight_g;
        ui_spool_canvas_set_fill_level(
            spool,
            total > 0 ? std::clamp(working_info_.remaining_weight_g / total, 0.0f, 1.0f) : 1.0f);
    }

    // Assign Spool: the Partial chip names a part-used spool's weight.
    if (size_chip_ == spool_assign::kPartialChip) {
        snprintf(partial_text_buf_, sizeof(partial_text_buf_), "%.0f g", size_remaining_g_);
    } else {
        snprintf(partial_text_buf_, sizeof(partial_text_buf_), "%s", lv_tr("Partial…"));
    }
    lv_subject_set_string(&partial_text_subject_, partial_text_buf_);

    update_header_and_panel();
    update_tag_badges();
    refresh_spool_list();
}

void AmsEditOverlay::update_header_and_panel() {
    if (!subjects_initialized_) {
        return;
    }
    const bool edit = lv_subject_get_int(&mode_subject_) == kModeEdit;
    snprintf(title_buf_, sizeof(title_buf_), "%s",
             edit ? lv_tr("Edit Spool") : lv_tr("Assign Spool"));
    lv_subject_set_string(&title_subject_, title_buf_);

    // "Lane 3 · Turtle 1", and on Edit Spool the spool: "· PolyTerra PLA · #12".
    std::string subtitle;
    if (slot_index_ == -2) {
        subtitle = lv_tr("External Filament");
    } else {
        subtitle = helix::ui::active_tool_lane_label(helix::ui::active_lane_noun(), slot_index_);
        if (const AmsBackend* backend = AmsState::instance().get_backend()) {
            const AmsSystemInfo info = backend->get_system_info();
            if (const AmsUnit* unit = info.get_unit_for_slot(slot_index_)) {
                const std::string& unit_name =
                    unit->display_name.empty() ? unit->name : unit->display_name;
                if (!unit_name.empty()) {
                    subtitle += " · " + unit_name;
                }
            }
        }
    }
    if (edit) {
        const std::string name = helix::compose_filament_label(
            working_info_.brand, working_info_.spool_name, working_info_.material, " ");
        if (!name.empty()) {
            subtitle += " · " + name;
        }
        if (working_info_.spoolman_id > 0) {
            subtitle += " · #" + std::to_string(working_info_.spoolman_id);
        }
    }
    snprintf(subtitle_buf_, sizeof(subtitle_buf_), "%s", subtitle.c_str());
    lv_subject_set_string(&subtitle_subject_, subtitle_buf_);

    // "of 1000 g · Nozzle 200–220 °C" under the remaining weight.
    std::string hint;
    if (working_info_.total_weight_g > 0) {
        hint = fmt::format("{} {:.0f} g", lv_tr("of"), working_info_.total_weight_g);
    }
    if (temp_buf_[0] != '\0') {
        hint += (hint.empty() ? "" : " · ") + std::string(temp_buf_);
    }
    snprintf(weight_hint_buf_, sizeof(weight_hint_buf_), "%s", hint.c_str());
    lv_subject_set_string(&weight_hint_subject_, weight_hint_buf_);

    if (working_info_.spoolman_id > 0) {
        const std::string saves = fmt::format(fmt::runtime(lv_tr("Saves to Spoolman spool #{}")),
                                              working_info_.spoolman_id);
        snprintf(saves_to_buf_, sizeof(saves_to_buf_), "%s", saves.c_str());
    } else {
        saves_to_buf_[0] = '\0';
    }
    lv_subject_set_string(&saves_to_subject_, saves_to_buf_);
}

void AmsEditOverlay::update_temp_display() {
    if (!overlay_root_) {
        return;
    }

    // Live per tap: the product's temperatures when a product is selected,
    // else the material database's range for the selected type, else the
    // slot's stored values.
    int nozzle_min = working_info_.nozzle_temp_min;
    int nozzle_max = working_info_.nozzle_temp_max;
    int bed_temp = working_info_.bed_temp;
    const std::string type = catalog_ ? selected_type(sel_, *catalog_) : working_info_.material;
    bool from_catalog = false;
    if (catalog_ && !sel_.product_id.empty()) {
        if (const auto* ef = catalog_->resolve_id(sel_.product_id)) {
            nozzle_min = ef->nozzle_min;
            nozzle_max = ef->nozzle_max;
            bed_temp = ef->bed_temp;
            from_catalog = true;
        }
    }
    // A moved selection names a material the stored temps may not describe
    // (a linked slot carries ITS spool's range), so the database answer wins
    // once the user has picked a family with no product attached.
    // A lane with no material holds no temperatures of its own either: any it
    // still carries belong to the spool that left.
    if (!from_catalog && !type.empty() &&
        (selection_moved_ || working_info_.material.empty() ||
         (nozzle_min == 0 && nozzle_max == 0 && bed_temp == 0))) {
        if (auto mat = filament::find_material(type)) {
            nozzle_min = mat->nozzle_min;
            nozzle_max = mat->nozzle_max;
            bed_temp = mat->bed_temp;
        }
    }

    // One read-only line under the summary: "Nozzle 230–250 °C · Bed 80 °C".
    if (nozzle_min > 0 || nozzle_max > 0 || bed_temp > 0) {
        const char* unit = helix::units::display_temp_symbol();
        snprintf(temp_buf_, sizeof(temp_buf_), "%s %d\xE2\x80\x93%d%s \xC2\xB7 %s %d%s",
                 lv_tr("Nozzle"), helix::units::to_display_temp_whole(nozzle_min),
                 helix::units::to_display_temp_whole(nozzle_max), unit, lv_tr("Bed"),
                 helix::units::to_display_temp_whole(bed_temp), unit);
    } else {
        temp_buf_[0] = '\0';
    }
    lv_subject_set_string(&temp_text_subject_, temp_buf_);
    update_header_and_panel();
    shown_nozzle_min_ = nozzle_min;
    shown_nozzle_max_ = nozzle_max;
    shown_bed_temp_ = bed_temp;
    update_tag_badges();
}

// ============================================================================
// Internal Methods
// ============================================================================

void AmsEditOverlay::update_ui() {
    if (!overlay_root_) {
        return;
    }

    auto* spoolman_subj = lv_xml_get_subject(nullptr, "printer_has_spoolman");
    bool has_spoolman = spoolman_subj && lv_subject_get_int(spoolman_subj) == 1;
    const bool managed = has_spoolman && working_info_.spoolman_id > 0;
    lv_subject_set_int(&is_managed_subject_, managed ? 1 : 0);

#if defined(HELIX_PLATFORM_ESP32)
    const bool scan_ok = false; // no camera on the v1 Core+AMS cut
#else
    const bool scan_ok = has_spoolman;
#endif
    lv_subject_set_int(&scan_enabled_subject_, scan_ok ? 1 : 0);

    // Clear acts on what the slot holds now, not on the edits staged over it.
    // A slot whose link another program owns has no Clear here; the slot menu
    // greys its own with the reason.
    const AmsBackend* clear_backend = AmsState::instance().get_backend();
    const bool clear_refused = slot_index_ >= 0 && clear_backend &&
                               !clear_backend->clear_spool_refusal(slot_index_).empty();
    lv_subject_set_int(
        &clear_visible_subject_,
        AmsContextMenu::should_show_clear_spool(original_info_) && !clear_refused ? 1 : 0);

    // Spool number beside the tracked mark.
    if (managed) {
        snprintf(spoolman_id_buf_, sizeof(spoolman_id_buf_), "#%d", working_info_.spoolman_id);
    } else {
        spoolman_id_buf_[0] = '\0';
    }
    lv_subject_set_string(&spoolman_id_subject_, spoolman_id_buf_);

    update_summary();
    update_temp_display();
}

bool AmsEditOverlay::selection_dirty() const {
    return selection_moved_ || details_color_set_;
}

bool AmsEditOverlay::is_dirty() const {
    const bool identity_dirty = working_info_.color_rgb != original_info_.color_rgb ||
                                working_info_.material != original_info_.material ||
                                working_info_.brand != original_info_.brand ||
                                working_info_.spoolman_id != original_info_.spoolman_id;
    const bool weight_dirty =
        std::abs(working_info_.remaining_weight_g - original_info_.remaining_weight_g) > 0.1f ||
        std::abs(working_info_.total_weight_g - original_info_.total_weight_g) > 0.1f;
    // The More-section fields ride the same save (build_spool_patches), so an
    // edit there alone must light Save and prompt on Back.
    const bool details_dirty = detail_working_.price != detail_original_.price ||
                               detail_working_.location != detail_original_.location ||
                               detail_working_.lot_nr != detail_original_.lot_nr ||
                               detail_working_.comment != detail_original_.comment;
    return identity_dirty || weight_dirty || details_dirty || selection_dirty() ||
           lane_settings_dirty();
}

void AmsEditOverlay::update_sync_button_state() {
    if (!subjects_initialized_) {
        return;
    }
    // The editor view's Save is dirty-gated; a save in flight holds it shut on
    // every view.
    const int view = lv_subject_get_int(&view_mode_subject_);
    lv_subject_set_int(&save_disabled_subject_,
                       save_is_disabled(view, save_in_flight_, is_dirty()) ? 1 : 0);
}

bool AmsEditOverlay::identity_locked(const SlotInfo& original, const SlotInfo& working,
                                     bool spoolman_available) {
    if (spoolman_available) {
        return false;
    }
    // Only the spool the editor opened on.
    return original.spoolman_id > 0 && working.spoolman_id == original.spoolman_id;
}

bool AmsEditOverlay::save_is_disabled(int view, bool save_in_flight, bool dirty) {
    if (save_in_flight) {
        return true;
    }
    return (view == kViewEditor) ? !dirty : false;
}

// ============================================================================
// Lane settings: Assigned Tool and Endless Spool backup
// ============================================================================

void AmsEditOverlay::load_lane_settings() {
    lane_original_ = {};
    backup_candidates_.clear();
    lane_tool_count_ = 0;
    AmsBackend* backend = slot_index_ >= 0 ? AmsState::instance().get_backend() : nullptr;
    bool tool_row = false;
    bool backup_row = false;
    bool backup_editable = false;
    std::string tool_refusal;
    if (backend) {
        const AmsSystemInfo info = backend->get_system_info();
        lane_tool_count_ = info.total_slots;
        backup_candidates_ = lane_settings::backup_candidates(info.present_slots(), slot_index_);
        tool_row = lane_settings::show_tool_row(backend);
        if (tool_row) {
            lane_original_.tool = lane_settings::assigned_tool(*backend, slot_index_);
            tool_refusal =
                lane_settings::tool_row_refusal(*backend, slot_index_, lane_original_.tool);
        }
        const auto caps = backend->get_endless_spool_capabilities();
        backup_row =
            lane_settings::show_backup_row(caps, backend->get_endless_spool_config(), slot_index_);
        if (backup_row) {
            lane_original_.backup = lane_settings::backup_of(*backend, slot_index_);
            backup_editable = caps.editable();
            lv_subject_set_string(&backup_label_subject_,
                                  backup_lane_text(backend->lane_noun()).c_str());
        }
    }
    lane_working_ = lane_original_;
    lv_subject_set_int(&lane_visible_subject_, backend ? 1 : 0);
    lv_subject_set_int(&tool_row_visible_subject_, tool_row ? 1 : 0);
    lv_subject_set_int(&tool_editable_subject_, tool_refusal.empty() ? 1 : 0);
    lv_subject_set_string(&tool_refusal_subject_, tool_refusal.c_str());
    lv_subject_set_int(&backup_row_visible_subject_, backup_row ? 1 : 0);
    lv_subject_set_int(&backup_editable_subject_, backup_editable ? 1 : 0);
}

void AmsEditOverlay::populate_lane_dropdowns() {
    if (lv_obj_t* dd = find_widget("tool_dropdown")) {
        lv_dropdown_set_options(dd, lane_settings::tool_options(lane_tool_count_).c_str());
        // None=0, T0=1, T1=2...
        lv_dropdown_set_selected(dd, static_cast<uint32_t>(lane_working_.tool + 1));
    }
    if (lv_obj_t* dd = find_widget("backup_dropdown")) {
        AmsBackend* backend = AmsState::instance().get_backend();
        lv_dropdown_set_options(
            dd, lane_settings::backup_options(backend ? backend->lane_noun() : active_lane_noun(),
                                              backup_candidates_, slot_index_,
                                              lane_settings::eligibility_of(backend))
                    .c_str());
        // None=0, then each candidate in order.
        const auto it =
            std::find(backup_candidates_.begin(), backup_candidates_.end(), lane_working_.backup);
        const int selected = it == backup_candidates_.end()
                                 ? 0
                                 : 1 + static_cast<int>(it - backup_candidates_.begin());
        lv_dropdown_set_selected(dd, static_cast<uint32_t>(selected));
    }
}

void AmsEditOverlay::handle_tool_selected(int index) {
    lane_working_.tool = index - 1;
    update_sync_button_state();
}

void AmsEditOverlay::handle_backup_selected(int index) {
    const int backup = index > 0 && index <= static_cast<int>(backup_candidates_.size())
                           ? backup_candidates_[static_cast<size_t>(index - 1)]
                           : -1;
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend && lane_settings::backup_refused(slot_index_, backup,
                                                 lane_settings::eligibility_of(backend))) {
        lane_settings::notify_backup_refused(*backend, slot_index_, backup);
        populate_lane_dropdowns(); // back to what is staged
        return;
    }
    lane_working_.backup = backup;
    update_sync_button_state();
}

bool AmsEditOverlay::lane_settings_dirty() const {
    return (lane_working_.tool >= 0 && lane_working_.tool != lane_original_.tool) ||
           lane_working_.backup != lane_original_.backup;
}

bool AmsEditOverlay::commit_lane_settings() {
    if (!lane_settings_dirty()) {
        return true;
    }
    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        // The lane went with its backend; there is nothing left to write to.
        spdlog::warn("[AmsEditOverlay] No backend for slot {}; lane settings not written",
                     slot_index_);
        return true;
    }
    bool all_written = true;
    if (lane_working_.tool >= 0 && lane_working_.tool != lane_original_.tool) {
        if (lane_settings::commit_assigned_tool(*backend, slot_index_, lane_working_.tool)) {
            lane_original_.tool = lane_working_.tool;
        } else {
            all_written = false;
        }
    }
    if (lane_working_.backup != lane_original_.backup) {
        if (lane_settings::commit_backup(*backend, slot_index_, lane_working_.backup)) {
            lane_original_.backup = lane_working_.backup;
        } else {
            all_written = false;
        }
    }
    return all_written;
}

// ============================================================================
// Colour staging (shared by the grid, the HSV view and Reset to tag)
// ============================================================================

void AmsEditOverlay::open_color_view() {
    // Seed custom sub-state from the pending colour. Black seeds as-is — it is
    // a real colour (#1608).
    custom_color_ = details_color_;
    lv_subject_set_int(&color_pane_subject_, 0);
    populate_color_view();
    set_view(kViewColor);
    update_color_fit();
}

bool AmsEditOverlay::color_panes_fit(int avail_w, int avail_h, int presets_w, int presets_h,
                                     int custom_w, int custom_h, int gap) {
    const bool side_by_side =
        presets_w + gap + custom_w <= avail_w && std::max(presets_h, custom_h) <= avail_h;
    const bool stacked =
        std::max(presets_w, custom_w) <= avail_w && presets_h + gap + custom_h <= avail_h;
    return side_by_side || stacked;
}

void AmsEditOverlay::update_color_fit() {
    lv_obj_t* view = find_widget("color_view");
    lv_obj_t* presets = find_widget("color_presets");
    lv_obj_t* custom = find_widget("color_custom");
    if (!view || !presets || !custom) {
        return;
    }
    lv_obj_update_layout(overlay_root_);
    lv_obj_t* area = lv_obj_get_parent(view);
    const int avail_h = lv_obj_get_content_height(area) -
                        lv_obj_get_style_pad_top(view, LV_PART_MAIN) -
                        lv_obj_get_style_pad_bottom(view, LV_PART_MAIN);
    const int avail_w = lv_obj_get_content_width(view);
    // Both panes have fixed sizes, so the verdict follows the room alone. Each
    // flip of ams_edit_color_fits rebuilds the preset grid's ~60 swatches, so
    // the subject moves only when the verdict changes.
    if (avail_w != color_fit_room_w_ || avail_h != color_fit_room_h_) {
        // Measured with both panes built and shown: the decision is whether
        // that form fits the overlay's content area without scrolling.
        if (lv_subject_get_int(&color_fits_subject_) != 1) {
            lv_subject_set_int(&color_fits_subject_, 1);
            lv_obj_update_layout(overlay_root_);
        }
        const int gap = lv_obj_get_style_pad_column(view, LV_PART_MAIN);
        color_fit_ =
            color_panes_fit(avail_w, avail_h, lv_obj_get_width(presets), lv_obj_get_height(presets),
                            lv_obj_get_width(custom), lv_obj_get_height(custom), gap);
        color_fit_room_w_ = avail_w;
        color_fit_room_h_ = avail_h;
        spdlog::debug("[AmsEditOverlay] Colour panes {}x{} + {}x{} gap {} in {}x{}: {}",
                      lv_obj_get_width(presets), lv_obj_get_height(presets),
                      lv_obj_get_width(custom), lv_obj_get_height(custom), gap, avail_w, avail_h,
                      color_fit_ ? "together" : "one at a time");
    }
    const int fits = color_fit_ ? 1 : 0;
    if (lv_subject_get_int(&color_fits_subject_) != fits) {
        lv_subject_set_int(&color_fits_subject_, fits);
    }
}

void AmsEditOverlay::show_color_pane(int pane) {
    lv_subject_set_int(&color_pane_subject_, pane);
}

void AmsEditOverlay::populate_color_view() {
    lv_obj_t* hsv = helix::ui::find_required(overlay_root_, "ams_color_hsv", get_name());
    if (hsv) {
        ui_hsv_picker_set_color_rgb(hsv, custom_color_);
    }
    lv_obj_t* preview = helix::ui::find_required(overlay_root_, "ams_color_preview", get_name());
    if (preview) {
        helix::ui::apply_swatch_color(preview, custom_color_, {});
    }
    lv_obj_t* hex_input =
        helix::ui::find_required(overlay_root_, "ams_color_hex_input", get_name());
    if (hex_input) {
        char buf[10];
        snprintf(buf, sizeof(buf), "#%06X", custom_color_);
        lv_textarea_set_text(hex_input, buf);
    }
}

void AmsEditOverlay::apply_color(uint32_t rgb) {
    details_color_ = helix::ams::picked_color(rgb);
    details_color_set_ = true;
    set_view(kViewEditor);
    update_summary();
    update_sync_button_state();
}

void AmsEditOverlay::handle_picker_search(const char* text) {
    if (cached_spools_.empty()) {
        return;
    }
    render_spool_list(text ? text : "");
}

void AmsEditOverlay::handle_quick_swatch(lv_obj_t* swatch) {
    if (!swatch) {
        return;
    }
    details_color_ = lv_color_to_u32(helix::ui::swatch_color_of(swatch)) & 0xFFFFFF;
    details_color_set_ = true;
    update_summary();
    update_sync_button_state();
}

void AmsEditOverlay::handle_reset_to_tag() {
    // Every field the tag states goes back to the tag's values - material,
    // brand, product line, colour, temperatures, total weight - through the
    // shared ams::reset_to_tag(). The editor's staging (selection, pending
    // colour) re-derives from the restored slot, and dirty state follows the
    // working/original diff: a slot that never left the tag's values resets
    // clean, one edited past them resets dirty.
    if (!tag_) {
        return;
    }
    working_info_ = ams::reset_to_tag(working_info_, *tag_);
    identity_reset_ = true;
    sel_ = selection_from_slot(working_info_);
    selection_moved_ = false;
    details_color_ = working_info_.color_rgb;
    details_color_set_ = false;
    rebuild_option_lists();
    update_summary();
    update_temp_display();
    refresh_tag_state();
    update_sync_button_state();
}

void AmsEditOverlay::handle_custom_color_changed(uint32_t rgb) {
    custom_color_ = rgb;
    lv_obj_t* preview = helix::ui::find_required(overlay_root_, "ams_color_preview", get_name());
    if (preview) {
        helix::ui::apply_swatch_color(preview, rgb, {});
    }
    lv_obj_t* hex_input =
        helix::ui::find_required(overlay_root_, "ams_color_hex_input", get_name());
    if (hex_input) {
        char buf[10];
        snprintf(buf, sizeof(buf), "#%06X", rgb);
        lv_textarea_set_text(hex_input, buf);
    }
}

void AmsEditOverlay::handle_color_swatch(lv_obj_t* swatch) {
    if (!swatch) {
        return;
    }
    const uint32_t rgb = lv_color_to_u32(helix::ui::swatch_color_of(swatch)) & 0xFFFFFF;
    apply_color(rgb); // preset tap applies immediately and returns
}

void AmsEditOverlay::handle_color_hex_changed() {
    lv_obj_t* hex_input =
        helix::ui::find_required(overlay_root_, "ams_color_hex_input", get_name());
    if (!hex_input) {
        return;
    }
    const char* text = lv_textarea_get_text(hex_input);
    uint32_t rgb = 0;
    if (text && helix::parse_hex_color(text, rgb)) {
        custom_color_ = rgb;
        lv_obj_t* hsv = helix::ui::find_required(overlay_root_, "ams_color_hsv", get_name());
        if (hsv) {
            ui_hsv_picker_set_color_rgb(hsv, rgb);
        }
        lv_obj_t* preview =
            helix::ui::find_required(overlay_root_, "ams_color_preview", get_name());
        if (preview) {
            helix::ui::apply_swatch_color(preview, rgb, {});
        }
    }
}

void AmsEditOverlay::handle_color_apply() {
    apply_color(custom_color_);
}

// ============================================================================
// More section / weight / logistics
// ============================================================================

void AmsEditOverlay::handle_more_toggle() {
    const int open = lv_subject_get_int(&more_open_subject_) ? 0 : 1;
    lv_subject_set_int(&more_open_subject_, open);
}

void AmsEditOverlay::handle_weight_tap() {
    if (save_in_flight_) {
        return; // the keypad would sit on top of the editor the save closes
    }
    const bool assign = lv_subject_get_int(&mode_subject_) == kModeAssign;
    const float shown = assign ? size_remaining_g_ : working_info_.remaining_weight_g;
    ui_keypad_config_t config = {.initial_value = shown > 0 ? shown : 0,
                                 .min_value = 0,
                                 .max_value = 100000,
                                 .title_label = lv_tr("Remaining weight"),
                                 .unit_label = "g",
                                 .allow_decimal = false,
                                 .allow_negative = false,
                                 .callback =
                                     [](float value, void* user_data) {
                                         static_cast<AmsEditOverlay*>(user_data)->apply_weight(
                                             value);
                                     },
                                 .user_data = this};
    ui_keypad_show(&config);
}

void AmsEditOverlay::apply_weight(float grams) {
    if (lv_subject_get_int(&mode_subject_) == kModeAssign) {
        // Assign Spool's Partial...: what is left of a spool of the size last
        // chosen (1 kg unless another chip was), never less than what is left.
        const float chip_total = size_chip_ == spool_assign::kPartialChip
                                     ? spool_assign::kDefaultSpoolGrams
                                     : size_total_g_;
        apply_size_chip(spool_assign::kPartialChip, grams, std::max(grams, chip_total));
        update_sync_button_state();
        return;
    }
    working_info_.remaining_weight_g = grams;
    // A spool weighed for the first time: the same number is the honest total
    // until something better is known.
    if (working_info_.total_weight_g <= 0 && grams > 0) {
        working_info_.total_weight_g = grams;
    }
    update_summary();
    update_sync_button_state();
}

void AmsEditOverlay::populate_detail_fields() {
    // Negative values are the "unknown" sentinel — render blank rather than a
    // literal "-1" (and a blank field can't trip the negative-value Save
    // validation on an untouched, weight-unknown untracked slot).
    const std::string price = format_spool_price(detail_working_.price);
    const struct {
        const char* widget;
        std::string value;
    } fields[] = {
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
    // The field shows the price at two decimals; text that still reads as the
    // stored price keeps it exactly, so an untouched open is not an edit.
    std::string s = text_of("detail_field_price");
    if (s == format_spool_price(detail_original_.price)) {
        detail_working_.price = detail_original_.price;
    } else {
        detail_working_.price = s.empty() ? 0 : std::atof(s.c_str());
    }
    detail_working_.location = text_of("detail_field_location");
    detail_working_.lot_nr = text_of("detail_field_lot_nr");
    detail_working_.comment = text_of("detail_field_comment");
}

// ============================================================================
// Scan QR / label printing
// ============================================================================

void AmsEditOverlay::handle_scan_qr() {
#if !defined(HELIX_PLATFORM_ESP32)
    if (save_in_flight_) {
        return;
    }
    spdlog::info("[AmsEditOverlay] Scan QR requested for slot {}", slot_index_);

    // The scanner overlay pushes ON TOP of the editor. Our on_deactivate
    // treats that as "covered": session state and the view subject survive.
    auto& scanner = helix::ui::get_qr_scanner_overlay();
    scanner.show(
        lv_screen_active(), slot_index_,
        [](const SpoolInfo& spool) {
            // Scanner result arrives outside our activation — marshal via the
            // queue and re-validate on the singleton, which lives for the
            // process. No direct backend write: the editor repopulates and the
            // user confirms with Save.
            helix::ui::queue_update("AmsEditOverlay::handle_scan_qr", [spool]() {
                auto& editor = get_ams_edit_overlay();
                if (!editor.get_root()) {
                    return;
                }
                editor.apply_scanned_spool(spool);
                NOTIFY_INFO("{} {} scanned — review and Save", spool.vendor, spool.material);
            });
        },
        []() { spdlog::debug("[AmsEditOverlay] QR scan cancelled - editor resumes"); });
#endif
}

#if HELIX_HAS_LABEL_PRINTER
void AmsEditOverlay::handle_print_label() {
    print_label_for(label_spool());
}

SpoolInfo AmsEditOverlay::label_spool() const {
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
    return spool_info;
}

void AmsEditOverlay::print_label_for(const SpoolInfo& spool_info) {
    auto& settings = helix::LabelPrinterSettingsManager::instance();
    if (!settings.is_configured()) {
        ToastManager::instance().show(ToastSeverity::INFO,
                                      lv_tr("Set up your label printer in Settings"), 3000);
        return;
    }

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

// ============================================================================
// Save Orchestration
// ============================================================================

void AmsEditOverlay::apply_material_defaults(const std::string& material) {
    // No product: temperatures follow the material's database range.
    working_info_.nozzle_temp_recommended = 0;
    working_info_.catalog_id.clear();
    working_info_.product_name.clear();
    if (auto mat = filament::find_material(material)) {
        working_info_.nozzle_temp_min = mat->nozzle_min;
        working_info_.nozzle_temp_max = mat->nozzle_max;
        working_info_.bed_temp = mat->bed_temp;
    }
}

void AmsEditOverlay::stage_selection_into_working() {
    if (!selection_moved_ || !catalog_) {
        return;
    }
    const std::string type = selected_type(sel_, *catalog_);
    working_info_.material = type;
    working_info_.brand = helix::text_io::equals_ignore_case(sel_.brand, "Generic")
                              ? std::string("Generic")
                              : sel_.brand;
    if (!sel_.product_id.empty()) {
        if (const auto* ef = catalog_->resolve_id(sel_.product_id)) {
            working_info_.nozzle_temp_min = ef->nozzle_min;
            working_info_.nozzle_temp_max = ef->nozzle_max;
            working_info_.nozzle_temp_recommended = ef->nozzle_recommended;
            working_info_.bed_temp = ef->bed_temp;
            working_info_.catalog_id = ef->id;
            working_info_.product_name = ef->name;
            // A Spoolman name describes the spool the old identity named.
            helix::ams::drop_stale_spool_name(original_info_, working_info_);
            return;
        }
    }
    apply_material_defaults(type);
    helix::ams::drop_stale_spool_name(original_info_, working_info_);
}

void AmsEditOverlay::fire_completion(bool saved) {
    if (completion_fired_) {
        return;
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
    // Guarded like fire_completion: a second close (prompt double-tap, close
    // callback racing the Save path) must not pop whatever screen opened
    // after us.
    if (completion_fired_) {
        return;
    }
    fire_completion(saved);
    helix::nav::go_back();
}

// ============================================================================
// Event Handlers
// ============================================================================

void AmsEditOverlay::handle_clear_spool() {
    if (save_in_flight_) {
        return;
    }
    // The lane menus' Clear, so a slot clears the same way from either place. It
    // needs no Spoolman: the slot's record is ours, and the Spoolman spool itself
    // is left alone. The clear is the commit, so the caller is told nothing saved.
    spdlog::info("[AmsEditOverlay] Clear spool for slot {}", slot_index_);
    ams_dispatch_backend_action(AmsContextMenu::MenuAction::CLEAR_SPOOL, slot_index_, nullptr);
    close_editor(false);
}

void AmsEditOverlay::handle_reapply_linked_spool() {
    if (save_in_flight_ || !lane_contradicts_linked_spool()) {
        return;
    }
    remember_form_for_note_action();
    const SpoolInfo& spool = detail_original_;
    spdlog::info("[AmsEditOverlay] Re-applying spool {} to slot {}", spool.id, slot_index_);
    // The lane's catalog product and its temperatures survive only a fix that keeps
    // both the material and the vendor, such as a colour-only one.
    const bool material_differs =
        !spool.material.empty() &&
        !helix::text_io::equals_ignore_case(spool.material, working_info_.material);
    const bool vendor_differs = !spool.vendor.empty() && !helix::text_io::equals_ignore_case(
                                                             spool.vendor, working_info_.brand);
    if (material_differs) {
        working_info_.material = spool.material;
    }
    if (!spool.vendor.empty()) {
        working_info_.brand = spool.vendor;
    }
    if (material_differs || vendor_differs) {
        apply_material_defaults(working_info_.material);
    }
    working_info_.spool_name = spool.filament_name;
    uint32_t rgb = 0;
    if (!spool.color_hex.empty() && helix::parse_hex_color(spool.color_hex.c_str(), rgb)) {
        working_info_.color_rgb = rgb;
        working_info_.color_name.clear();
        working_info_.multi_color_hexes.clear();
    }
    sel_ = selection_from_slot(working_info_);
    selection_moved_ = false;
    details_color_ = working_info_.color_rgb;
    details_color_set_ = false;
    handle_save();
}

void AmsEditOverlay::handle_unlink_disagreeing_spool() {
    if (save_in_flight_ || !lane_contradicts_linked_spool()) {
        return;
    }
    spdlog::info("[AmsEditOverlay] Unlinking disagreeing spool {} from slot {}",
                 working_info_.spoolman_id, slot_index_);
    remember_form_for_note_action();
    stage_selection_into_working();
    stage_pending_color_into_working();
    working_info_.unlink_as_different_filament(original_info_);
    handle_save();
}

void AmsEditOverlay::handle_back() {
    int view = lv_subject_get_int(&view_mode_subject_);
    if (save_in_flight_) {
        return; // the save's completion closes the editor
    }
    switch (view) {
    case kViewColor:
        return_to_editor();
        break;
    case kViewEditor:
    default:
        if (back_to_edit_spool()) {
            return;
        }
        if (is_dirty()) {
            prompt_unsaved_changes();
            return;
        }
        spdlog::debug("[AmsEditOverlay] Back on editor - cancelling");
        working_info_ = original_info_;
        close_editor(false);
        break;
    }
}

void AmsEditOverlay::prompt_unsaved_changes() {
    // Back with unsaved edits: Save / Discard. A dismissal (backdrop tap,
    // ESC) keeps the editor open — the user has not answered.
    ConfirmOptions opts;
    opts.cancel_text = lv_tr("Discard");
    opts.on_cancel = [] {
        auto& editor = get_ams_edit_overlay();
        editor.working_info_ = editor.original_info_;
        editor.close_editor(false);
    };
    opts.on_dismiss = [] { /* stay open */ };
    opts.owner_token = lifetime_.token();
    modal_confirm(
        lv_tr("Unsaved changes"), lv_tr("Save the changes to this spool?"), ModalSeverity::Info,
        lv_tr("Save"), [] { get_ams_edit_overlay().handle_save(); }, opts);
}

bool AmsEditOverlay::should_create_new_spool(const SlotInfo& working_info, bool add_requested) {
    return add_requested && working_info.spoolman_id == 0 &&
           helix::SpoolmanSlotSaver::is_filament_complete(working_info);
}

AmsEditOverlay::SpoolContradiction
AmsEditOverlay::contradiction_with_spool(const SlotInfo& lane, const SpoolInfo& spool) {
    SpoolContradiction result;
    result.material = !lane.material.empty() && !spool.material.empty() &&
                      !helix::FilamentMapper::materials_match(lane.material, spool.material);
    uint32_t spool_rgb = 0;
    if (helix::ams::is_declarable_color(lane.color_rgb) && !spool.color_hex.empty() &&
        helix::parse_hex_color(spool.color_hex.c_str(), spool_rgb)) {
        result.color = !helix::FilamentMapper::colors_match(lane.color_rgb, spool_rgb);
    }
    return result;
}

bool AmsEditOverlay::lane_contradicts_linked_spool() const {
    if (working_info_.spoolman_id <= 0 || working_info_.spoolman_id != original_info_.spoolman_id ||
        details_spool_id_ != working_info_.spoolman_id) {
        return false;
    }
    const AmsBackend* backend = AmsState::instance().get_backend();
    return backend && backend->firmware_owns_lane_identity() &&
           contradiction_with_spool(original_info_, detail_original_).any();
}

AmsEditOverlay::SpoolmanView AmsEditOverlay::spoolman_view() const {
    SpoolmanView view{original_info_, working_info_};
    if (!lane_contradicts_linked_spool()) {
        return view;
    }
    const SpoolInfo& spool = detail_original_;
    if (!spool.material.empty()) {
        view.base.material = spool.material;
    }
    if (!spool.vendor.empty()) {
        view.base.brand = spool.vendor;
    }
    uint32_t rgb = 0;
    if (!spool.color_hex.empty() && helix::parse_hex_color(spool.color_hex.c_str(), rgb)) {
        view.base.color_rgb = rgb;
    }
    if (working_info_.material == original_info_.material) {
        view.target.material = view.base.material;
    }
    if (working_info_.brand == original_info_.brand) {
        view.target.brand = view.base.brand;
    }
    if (working_info_.color_rgb == original_info_.color_rgb) {
        view.target.color_rgb = view.base.color_rgb;
    }
    view.base.catalog_id = view.target.catalog_id;
    view.base.product_name = view.target.product_name;
    return view;
}

void AmsEditOverlay::remember_form_for_note_action() {
    note_action_form_ =
        FormSnapshot{working_info_, sel_, selection_moved_, details_color_, details_color_set_};
}

void AmsEditOverlay::restore_form_after_note_action() {
    if (!note_action_form_) {
        return;
    }
    working_info_ = note_action_form_->working;
    sel_ = note_action_form_->selection;
    selection_moved_ = note_action_form_->selection_moved;
    details_color_ = note_action_form_->details_color;
    details_color_set_ = note_action_form_->details_color_set;
    note_action_form_.reset();
    if (editor_entered_) {
        rebuild_option_lists();
        update_summary();
        update_temp_display();
    }
}

void AmsEditOverlay::update_spool_disagreement() {
    if (!subjects_initialized_) {
        return;
    }
    const bool disagrees = lane_contradicts_linked_spool();
    if (disagrees) {
        const SpoolContradiction differs =
            contradiction_with_spool(original_info_, detail_original_);
        std::string stated;
        if (differs.material) {
            stated = detail_original_.material;
        }
        uint32_t rgb = 0;
        if (differs.color && helix::parse_hex_color(detail_original_.color_hex.c_str(), rgb)) {
            const std::string color = helix::get_color_name_from_hex(rgb);
            stated += stated.empty() ? color : ", " + color;
        }
        const std::string note =
            fmt::format(fmt::runtime(lv_tr("The linked spool says {}.")), stated);
        snprintf(spool_disagreement_buf_, sizeof(spool_disagreement_buf_), "%s", note.c_str());
        lv_subject_set_string(&spool_disagreement_text_subject_, spool_disagreement_buf_);
    }
    lv_subject_set_int(&spool_disagrees_subject_, disagrees ? 1 : 0);
}

bool AmsEditOverlay::needs_identity_confirmation(const SlotInfo& original, const SlotInfo& edited) {
    if (edited.spoolman_id <= 0) {
        return false; // nothing linked to clobber
    }
    if (edited.spoolman_id != original.spoolman_id) {
        return false; // a relink is not an edit of the linked spool
    }
    auto changes = helix::SpoolmanSlotSaver::detect_changes(original, edited);
    if (!changes.any()) {
        return false;
    }
    return is_material_identity_change(original, edited);
}

void AmsEditOverlay::stage_pending_color_into_working() {
    if (!details_color_set_)
        return;
    working_info_.color_rgb = details_color_;
    // The hex is the colour. A stored name would describe the previous one,
    // and every label that wants words computes them from the hex.
    working_info_.color_name.clear();
    working_info_.multi_color_hexes.clear();
}

void AmsEditOverlay::handle_add_to_spoolman() {
    // Add to Spoolman & Assign: create a Spoolman spool from what the form
    // describes and link the lane to it. Stages the selection first so the
    // created spool carries the dropdowns' identity, not the pre-edit one.
    if (save_in_flight_) {
        return;
    }
    stage_assign_form();
    const helix::MissingFilamentFields missing =
        helix::SpoolmanSlotSaver::missing_filament_fields(working_info_);
    if (missing.any()) {
        report_save_problem(true, missing_filament_message(missing));
        return;
    }
    if (!commit_lane_settings_for_assign()) {
        return;
    }
    do_spoolman_save(helix::SpoolmanSlotSaver::LinkIntent::CreateAndRebind);
}

void AmsEditOverlay::handle_save() {
    // The editor stays open while an async Spoolman save runs; a second Save
    // from the prompt or the header would start a second find-or-create.
    if (save_in_flight_) {
        return;
    }
    if (lv_subject_get_int(&view_mode_subject_) == kViewColor) {
        apply_color(custom_color_); // Select on the colour view stages + returns
        return;
    }

    spdlog::info("[AmsEditOverlay] Saving edits for slot {}", slot_index_);

    set_save_state(kSaveIdle);
    read_detail_fields();
    if (detail_working_.price < 0) {
        report_save_problem(true, lv_tr("Values must not be negative"));
        return;
    }

    // Assign Spool's Save (Back's unsaved-changes prompt) commits what its
    // form says, the spool size included, as its own actions do.
    if (lv_subject_get_int(&mode_subject_) == kModeAssign) {
        stage_assign_form();
    } else {
        stage_selection_into_working();
        stage_pending_color_into_working();
    }

    // The More section's Spoolman-owned fields (price/location/lot/notes):
    // PATCH the spool record's diff before committing, the way the identity
    // outcomes write theirs. When the identity prompt owns this save, the
    // PATCH instead rides the Update arm inside do_spoolman_save - so a
    // material change and a Notes edit in one Save lose nothing - while
    // Cancel stays a true abort and the Unlink arm drops logistics by design
    // (the spool leaves our record). Fields are written only once the record
    // of the spool the slot now names has landed: before that they hold no
    // server values to diff against.
    const bool details_loaded =
        working_info_.spoolman_id > 0 && details_spool_id_ == working_info_.spoolman_id;
    const bool has_logistics_diff = [this]() {
        nlohmann::json a;
        nlohmann::json b;
        SpoolmanSlotSaver::build_spool_patches(detail_original_, detail_working_, a, b);
        return !a.empty() || !b.empty();
    }();
    const bool spoolman_reachable = api_ && get_printer_state().is_spoolman_available();
    if (working_info_.spoolman_id > 0 && !details_loaded && has_logistics_diff &&
        spoolman_reachable) {
        // The record those fields edit has not arrived, so there is nothing to
        // diff them against. Keep them, ask for the record again, and say so
        // rather than closing on a Save that wrote them nowhere.
        report_save_problem(
            true, lv_tr("This spool's details haven't loaded from Spoolman yet. Try Save again."));
        if (!linked_fetch_pending_) {
            fetch_linked_spool();
        }
        return;
    }

    // The lane's settings go to the backend, not to Spoolman, so they land
    // whatever the spool's own save does next.
    const bool lane_changed = lane_settings_dirty();
    if (!commit_lane_settings()) {
        // The refusal has toasted. The editor stays open with that field
        // still staged, and the slot's own edits wait for the same Save.
        stay_open_after_failed_save();
        return;
    }
    if (lane_changed && !is_dirty()) {
        // Only the lane's settings changed: there is no slot edit to commit.
        close_editor(false);
        return;
    }

    if (working_info_.spoolman_id > 0 && !details_loaded && has_logistics_diff) {
        // Spoolman is unreachable, so the record never will arrive. The
        // slot's own edits commit now; the More edits stay in the editor.
        SaveCapture cap = capture_save();
        cap.stay_open = true;
        commit_capture(cap);
        return;
    }
    const SpoolmanView spoolman = spoolman_view();
    const bool spoolman_writable = may_write_spoolman_now(spoolman.base, spoolman.target);
    logistics_pending_ = has_logistics_diff && details_loaded && !spoolman_writable;
    if (details_loaded && api_ && !logistics_pending_ && spoolman_writable) {
        nlohmann::json spool_patch;
        nlohmann::json filament_patch;
        SpoolmanSlotSaver::build_spool_patches(detail_original_, detail_working_, spool_patch,
                                               filament_patch);
        // The More fields are all spool-level (build_spool_patches files only
        // colour under the filament, and these fields never carry one), so the
        // shared filament record is never written from here.
        if (!spool_patch.empty()) {
            const int spool_id = detail_working_.id;
            save_in_flight_ = true;
            update_sync_button_state();
            // Owed to Spoolman and the slot, not to the screen: the rest of the
            // save runs from what this Save captured, editor open or not.
            auto token = object_lifetime_.token();
            const SaveCapture cap = capture_save();
            begin_slot_save(cap.slot_index);
            auto on_saved = [this, token, cap]() {
                token.defer("AmsEditOverlay::logistics_saved", [this, cap]() {
                    if (cap.session == session_) {
                        save_in_flight_ = false;
                        detail_original_ = detail_working_;
                        update_sync_button_state();
                    }
                    SaveCapture next = cap;
                    next.more_saved = true;
                    commit_capture(next);
                    end_slot_save(cap.slot_index);
                });
            };
            auto on_error = [this, token, cap, spool_id](const MoonrakerError& err) {
                spdlog::error("[AmsEditOverlay] Logistics PATCH for spool {} failed: {}", spool_id,
                              err.message);
                token.defer("AmsEditOverlay::logistics_error", [this, cap]() {
                    if (cap.session == session_) {
                        save_in_flight_ = false;
                    }
                    report_save_problem(save_is_live(cap), lv_tr("Failed to save spool"));
                    if (cap.session == session_) {
                        update_sync_button_state();
                    }
                    end_slot_save(cap.slot_index);
                });
            };
            api_->spoolman().update_spoolman_spool(spool_id, spool_patch, on_saved, on_error);
            return; // commit continues from the callback
        }
    }

    commit_and_close();
}

bool AmsEditOverlay::save_pending_for(int slot_index) const {
    const auto it = pending_saves_.find(slot_index);
    return it != pending_saves_.end() && it->second > 0;
}

void AmsEditOverlay::begin_slot_save(int slot_index) {
    ++pending_saves_[slot_index];
}

void AmsEditOverlay::end_slot_save(int slot_index) {
    auto it = pending_saves_.find(slot_index);
    if (it != pending_saves_.end() && --it->second <= 0) {
        pending_saves_.erase(it);
    }
}

AmsEditOverlay::SaveCapture AmsEditOverlay::capture_save() const {
    SaveCapture cap;
    cap.session = session_;
    cap.slot_index = slot_index_;
    const SpoolmanView spoolman = spoolman_view();
    cap.original = spoolman.base;
    cap.spoolman_target = spoolman.target;
    cap.saving = working_info_;
    cap.owed = completion_callback_;
    cap.chained_spool_id = detail_working_.id;
    // The More fields ride the Update arm when the identity prompt owns this
    // save, so a combined material+Notes Save loses nothing. They are all
    // spool-level; the filament half of the diff is always empty here.
    if (logistics_pending_) {
        nlohmann::json filament_patch;
        SpoolmanSlotSaver::build_spool_patches(detail_original_, detail_working_,
                                               cap.chained_spool_patch, filament_patch);
    }
    return cap;
}

bool AmsEditOverlay::save_is_live(const SaveCapture& cap) const {
    return cap.session == session_ && !completion_fired_;
}

void AmsEditOverlay::deliver_commit(const SaveCapture& cap, const SlotInfo& committed) {
    if (cap.stay_open && save_is_live(cap)) {
        // Commit the slot and keep the editor, holding the edits that could
        // not be saved.
        if (cap.owed) {
            EditResult result;
            result.saved = true;
            result.slot_index = cap.slot_index;
            result.slot_info = committed;
            cap.owed(result);
        }
        original_info_ = committed;
        working_info_ = committed;
        sel_ = selection_from_slot(working_info_);
        selection_moved_ = false;
        details_color_ = working_info_.color_rgb;
        details_color_set_ = false;
        update_summary();
        report_save_problem(true, lv_tr("Saved. Spoolman is offline, so price, location, lot "
                                        "and notes stay here unsaved."));
        return;
    }
    if (save_is_live(cap)) {
        working_info_ = committed;
        commit_then_await(cap, committed);
        return;
    }
    if (cap.owed) {
        spdlog::info("[AmsEditOverlay] Save for slot {} finished after the editor closed; "
                     "committing it",
                     cap.slot_index);
        EditResult late;
        late.saved = true;
        late.slot_index = cap.slot_index;
        late.slot_info = committed;
        cap.owed(late);
    }
}

std::string AmsEditOverlay::not_saved_message(helix::ams::PendingOutcome outcome,
                                              const std::string& reason) {
    std::string why = reason;
    if (why.empty()) {
        why = outcome == helix::ams::PendingOutcome::TimedOut
                  ? lv_tr("the printer did not confirm it")
                  : lv_tr("the printer refused it");
    }
    return fmt::format(fmt::runtime(lv_tr("Not saved: {}. Try again.")), why);
}

void AmsEditOverlay::set_save_state(int state, const std::string& error) {
    if (!subjects_initialized_) {
        return;
    }
    snprintf(save_error_buf_, sizeof(save_error_buf_), "%s", error.c_str());
    lv_subject_set_string(&save_error_subject_, save_error_buf_);
    if (state == kSaveWaiting) {
        update_save_wait_text();
        if (!save_wait_action_observer_) {
            save_wait_action_observer_ = helix::ui::observe<int>(
                AmsState::instance().get_ams_action_subject(), this,
                [](AmsEditOverlay* self, int) { self->update_save_wait_text(); },
                AmsState::instance().get_subjects_lifetime());
        }
    } else {
        save_wait_action_observer_.reset();
    }
    lv_subject_set_int(&save_state_subject_, state);
}

std::string AmsEditOverlay::save_wait_reason(const AmsBackend& backend, const AmsSystemInfo& info) {
    if (!ams_action_is_busy(info.action)) {
        return {};
    }
    const int slot =
        info.operation_working_slot >= 0 ? info.operation_working_slot : info.path_active_slot();
    // Each lane of a parallel system is its own head, and the head is what a
    // load or unload works on; elsewhere the lane names the operation.
    std::string where;
    if (slot >= 0) {
        where = backend.get_topology() == PathTopology::PARALLEL
                    ? helix::ui::active_tool_lane_label(backend.tool_noun(), slot)
                    : helix::ui::backend_lane_label(backend, slot);
    }
    const bool loading = info.action == AmsAction::LOADING;
    const bool unloading = info.action == AmsAction::UNLOADING;
    if (where.empty()) {
        return loading     ? lv_tr("Saves when the load finishes.")
               : unloading ? lv_tr("Saves when the unload finishes.")
                           : lv_tr("Saves when the filament operation finishes.");
    }
    const char* format = loading     ? lv_tr("Saves when the load on {} finishes.")
                         : unloading ? lv_tr("Saves when the unload on {} finishes.")
                                     : lv_tr("Saves when the filament operation on {} finishes.");
    return fmt::format(fmt::runtime(format), where);
}

void AmsEditOverlay::update_save_wait_text() {
    if (!subjects_initialized_) {
        return;
    }
    std::string text;
    if (const AmsBackend* backend = AmsState::instance().get_backend()) {
        text = save_wait_reason(*backend, backend->get_system_info());
    }
    if (text.empty()) {
        text = lv_tr("Saving...");
    }
    if (text == save_wait_text_buf_) {
        return;
    }
    snprintf(save_wait_text_buf_, sizeof(save_wait_text_buf_), "%s", text.c_str());
    lv_subject_set_string(&save_wait_text_subject_, save_wait_text_buf_);
}

void AmsEditOverlay::commit_then_await(const SaveCapture& cap, const SlotInfo& committed) {
    // The caller's completion commits the edit; the editor stays until the lane
    // holds it, so a write that does not stick leaves the user's values here.
    AmsBackend* backend = AmsState::instance().get_backend();
    const helix::ams::LaneId lane =
        backend ? backend->lane_id(cap.slot_index) : helix::ams::kInvalidLaneId;
    const std::uint64_t filed_before = helix::ams::pending_filings(lane);
    // Only a result the completion leaves is this save's: one an earlier
    // caller left on the slot and never took says nothing about it.
    (void)AmsState::instance().take_slot_edit_result(cap.slot_index);
    if (cap.owed) {
        EditResult result;
        result.saved = true;
        result.slot_index = cap.slot_index;
        result.slot_info = committed;
        cap.owed(result);
    }
    if (const std::optional<AmsError> err =
            AmsState::instance().take_slot_edit_result(cap.slot_index);
        err && !err->success() && !err->partially_applied) {
        finish_awaited_save(cap, helix::ams::PendingOutcome::Failed, err->user_msg);
        return;
    }
    save_in_flight_ = true;
    set_save_state(kSaveWaiting);
    update_sync_button_state();
    begin_slot_save(cap.slot_index);
    auto token = object_lifetime_.token();
    // A write that fails at once (a refusal, a reply already in) has ended
    // before this waits; filed_before lets the store report it.
    helix::ams::await_pending(
        lane, filed_before,
        [this, token, cap](helix::ams::PendingOutcome outcome, const std::string& reason) {
            token.defer("AmsEditOverlay::save_settled", [this, cap, outcome, reason]() {
                end_slot_save(cap.slot_index);
                finish_awaited_save(cap, outcome, reason);
            });
        });
}

void AmsEditOverlay::finish_awaited_save(const SaveCapture& cap, helix::ams::PendingOutcome outcome,
                                         const std::string& reason) {
    if (cap.session != session_ || completion_fired_) {
        return;
    }
    save_in_flight_ = false;
    if (outcome == helix::ams::PendingOutcome::Echoed) {
        set_save_state(kSaveIdle);
        // The callback already has the edit: close without firing it again.
        completion_fired_ = true;
        helix::nav::go_back();
        // Every saved assignment joins the printer's lane history, the list
        // Assign Spool offers without Spoolman.
        if (api_) {
            lane_spool_history::record(*api_, working_info_);
        }
        // What the lane settled on, which a backend may have normalized from
        // the form: Undo checks the lane against this.
        SlotInfo settled = working_info_;
        if (cap.slot_index == -2) {
            settled = AmsState::instance().get_external_spool_info().value_or(working_info_);
        } else if (AmsBackend* backend = AmsState::instance().get_backend()) {
            settled = backend->get_slot_info(cap.slot_index);
        }
        // Undo reverts the lane: a save that changed nothing on it (notes,
        // price, location, lot) or rewrote a Spoolman record has nothing it
        // can put back.
        const bool undoable =
            !edited_spoolman_record_ && spool_assign::save_changed_lane(original_info_, settled);
        show_assigned_toast(original_info_, settled, created_spool_id_ > 0, undoable);
        return;
    }
    spdlog::warn("[AmsEditOverlay] Save for slot {} did not stick: {}", cap.slot_index,
                 reason.empty() ? "no echo" : reason);
    // The lane shows what Moonraker holds again; the form keeps the user's
    // values, so Save is live for a retry and Back asks before discarding.
    if (AmsBackend* backend = AmsState::instance().get_backend()) {
        original_info_ = backend->get_slot_info(cap.slot_index);
    }
    restore_form_after_note_action();
    set_save_state(kSaveFailed, not_saved_message(outcome, reason));
    update_sync_button_state();
}

void AmsEditOverlay::commit_and_close() {
    commit_capture(capture_save());
}

void AmsEditOverlay::commit_capture(const SaveCapture& cap) {
    const bool live = save_is_live(cap);
    const SlotInfo& original = cap.original;
    const SlotInfo& saving = cap.saving;
    const SlotInfo& spoolman_target = cap.spoolman_target;

    // Spoolman can go away between staging a brand or material and saving it.
    // The change has nowhere to go, and committing it locally would leave the
    // slot claiming an identity the server never heard.
    if (identity_locked(original, saving, get_printer_state().is_spoolman_available()) &&
        (spoolman_target.brand != original.brand ||
         spoolman_target.material != original.material)) {
        report_save_problem(live,
                            lv_tr("Spoolman is offline, so brand and material can't be saved."));
        return;
    }

    // Switching the linked spool is a pure RELINK, not an edit of the linked
    // spool's record.
    if (saving.spoolman_id > 0 && saving.spoolman_id != original.spoolman_id) {
        spdlog::info("[AmsEditOverlay] Relink slot {} to spool {} (was {})", cap.slot_index,
                     saving.spoolman_id, original.spoolman_id);
        deliver_commit(cap, saving);
        return;
    }

    if (api_ && get_printer_state().is_spoolman_available()) {
        if (needs_identity_confirmation(original, spoolman_target)) {
            // Only an open editor can ask. A detached save never gets here: the
            // logistics PATCH that runs ahead of this needs no prompt.
            if (live) {
                prompt_identity_change_then_save();
            }
            return;
        }
        if (saving.spoolman_id > 0 &&
            helix::SpoolmanSlotSaver::detect_changes(original, spoolman_target).any()) {
            run_spoolman_save(cap, helix::SpoolmanSlotSaver::LinkIntent::UpdateLinked);
            return; // the save's completion commits
        }
    }

    // No Spoolman changes (or no Spoolman): commit locally now.
    deliver_commit(cap, saving);
}

void AmsEditOverlay::stay_open_after_failed_save() {
    restore_form_after_note_action();
    update_sync_button_state();
}

const char* AmsEditOverlay::missing_filament_message(const helix::MissingFilamentFields& missing) {
    if (missing.brand) {
        return lv_tr("Spoolman needs a brand for this filament.");
    }
    if (missing.material) {
        return lv_tr("Spoolman needs a material for this filament.");
    }
    if (missing.color) {
        return lv_tr("Spoolman needs a color for this filament.");
    }
    return nullptr;
}

void AmsEditOverlay::report_save_problem(bool live, const char* message) {
    if (message == nullptr) {
        return;
    }
    if (live) {
        restore_form_after_note_action();
        set_save_state(kSaveFailed, message);
        update_sync_button_state();
        return;
    }
    ToastManager::instance().show(ToastSeverity::WARNING, message, 4000);
}

void AmsEditOverlay::do_spoolman_save(helix::SpoolmanSlotSaver::LinkIntent intent) {
    run_spoolman_save(capture_save(), intent);
}

void AmsEditOverlay::run_spoolman_save(const SaveCapture& cap,
                                       helix::SpoolmanSlotSaver::LinkIntent intent) {
    // Owed to Spoolman and the slot, not to the screen: the commit comes from
    // the capture, so it lands even when the editor closes (a navbar tap, a
    // new session) before Spoolman answers.
    auto token = object_lifetime_.token();
    auto saver = std::make_shared<helix::SpoolmanSlotSaver>(
        api_, get_printer_state().get_discovery().filament_diameter_mm());
    // The offer arrives on the Spoolman reply's thread; the dialog is LVGL.
    saver->set_similar_filament_chooser(
        [token, slot_index = cap.slot_index](
            const FilamentInfo& similar,
            std::function<void(helix::SpoolmanSlotSaver::SimilarChoice)> answer) {
            token.defer("AmsEditOverlay::offer_similar_filament", [similar, slot_index,
                                                                   answer = std::move(answer)]() {
                const auto rgb = helix::parse_hex_color(
                    helix::spoolman::normalize_color_hex(similar.color_hex).substr(0, 6));
                const std::string name = helix::spoolman::filament_display_name(
                    similar, rgb ? helix::get_color_name_from_hex(*rgb) : "");
                get_ams_edit_overlay().offer_similar_filament(name, slot_index, answer);
            });
        });
    // One save at a time: the editor stays open while this runs.
    if (cap.session == session_) {
        save_in_flight_ = true;
        update_sync_button_state();
    }
    begin_slot_save(cap.slot_index);
    saver->save(
        cap.original, cap.spoolman_target, intent,
        [this, token, cap, saver, intent](const helix::SaveResult& result) {
            // Spoolman callback arrives on a background thread — defer
            // to the UI thread before touching LVGL.
            token.defer([this, cap, result, intent]() {
                const bool live = save_is_live(cap);
                if (cap.session == session_) {
                    save_in_flight_ = false;
                }
                if (result.missing.any()) {
                    spdlog::info("[AmsEditOverlay] Spoolman save stopped: the filament is "
                                 "missing a field it is identified by");
                    report_save_problem(live, missing_filament_message(result.missing));
                    end_slot_save(cap.slot_index);
                    return;
                }
                if (result.cancelled) {
                    // The user declined at the similar-filament offer: the
                    // identity save wrote nothing. A More PATCH that ran ahead
                    // of it did land, and an editor that has closed keeps no
                    // edits, so both say so.
                    if (cap.more_saved) {
                        report_save_problem(
                            live, lv_tr("Spoolman saved the price, location, lot and notes, but "
                                        "not the rest. Nothing else changed on this printer."));
                    } else if (!live) {
                        report_save_problem(
                            live,
                            lv_tr("Couldn't save to Spoolman. Nothing changed on this printer."));
                    } else {
                        stay_open_after_failed_save();
                    }
                    end_slot_save(cap.slot_index);
                    return;
                }
                if (!result.success) {
                    spdlog::error("[AmsEditOverlay] Spoolman save failed; nothing written");
                    // A More PATCH that ran ahead of this save did land.
                    report_save_problem(
                        live, cap.more_saved
                                  ? lv_tr("Spoolman saved the price, location, lot and notes, "
                                          "but not the rest. Nothing else changed on this "
                                          "printer.")
                                  : lv_tr("Couldn't save to Spoolman. Nothing changed on this "
                                          "printer."));
                    end_slot_save(cap.slot_index);
                    return;
                }
                SlotInfo committed = cap.saving;
                helix::SpoolmanSlotSaver::apply_save_result_identity(committed, result);
                SlotInfo spool_now = cap.spoolman_target;
                helix::SpoolmanSlotSaver::apply_save_result_identity(spool_now, result);
                // What Spoolman now holds never rewrites a lane it disagrees with.
                if (cap.spoolman_target.material != cap.saving.material ||
                    cap.spoolman_target.brand != cap.saving.brand ||
                    cap.spoolman_target.color_rgb != cap.saving.color_rgb) {
                    committed.material = cap.saving.material;
                    committed.brand = cap.saving.brand;
                    committed.color_rgb = cap.saving.color_rgb;
                }
                if (result.repointed_filament) {
                    // The spool now carries the identity this save
                    // wrote. File it as the lane's Spoolman record
                    // BEFORE committing: the consumer commit keeps a
                    // linked spool's own identity, and the record it
                    // would otherwise read is the pre-repoint one the
                    // next fetch has not replaced yet.
                    SpoolInfo repointed;
                    repointed.id = committed.spoolman_id;
                    repointed.filament_id = result.new_filament_id;
                    repointed.vendor_id = result.new_vendor_id;
                    repointed.vendor = spool_now.brand;
                    repointed.material = spool_now.material;
                    repointed.filament_name = spool_now.spool_name;
                    repointed.color_hex = helix::color_to_hex_string(spool_now.color_rgb);
                    if (auto* owner = AmsState::instance().get_backend()) {
#if !defined(HELIX_PLATFORM_ESP32)
                        // spoolman_manager.cpp is scoped out of the ESP32
                        // image; the repointed record there re-reads on the
                        // next poll instead.
                        SpoolmanManager::apply_fetched_spool(*owner, 0, cap.slot_index, repointed);
#endif
                    }
                }
                if (live) {
                    working_info_ = committed;
                    // The closing toast names the spool this save created.
                    if (result.created_new_spool) {
                        created_spool_id_ = committed.spoolman_id;
                    }
                    if (intent == helix::SpoolmanSlotSaver::LinkIntent::UpdateLinked) {
                        edited_spoolman_record_ = true;
                    }
#if HELIX_HAS_LABEL_PRINTER
                    // A spool new to Spoolman gets its label printed at once.
                    if (result.created_new_spool &&
                        helix::LabelPrinterSettingsManager::instance().is_configured()) {
                        handle_print_label();
                    }
#endif
                } else if (result.created_new_spool) {
                    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Added to Spoolman"),
                                                  2500);
                }

                // Close the editor that is still showing this save, or hand the
                // commit straight to the callback the save began with.
                const int saved_spool_id = committed.spoolman_id;
                auto deliver = [this, cap, committed, saved_spool_id]() {
                    deliver_commit(cap, committed);
                    end_slot_save(cap.slot_index);
                    SpoolmanManager::refresh_spool(saved_spool_id);
                };

                const bool chain = !cap.chained_spool_patch.empty() &&
                                   cap.chained_spool_id == saved_spool_id && api_;
                if (!chain) {
                    deliver();
                    return;
                }
                // Spoolman callbacks arrive on a background thread; the commit
                // itself goes through the queue (L01). The chained PATCH is
                // best-effort: the identity save already succeeded, so a
                // failure says what did not save and commits anyway.
                auto token2 = object_lifetime_.token();
                auto finish = [token2, deliver](bool more_saved) {
                    token2.defer("AmsEditOverlay::chained_logistics_done", [deliver, more_saved]() {
                        if (!more_saved) {
                            ToastManager::instance().show(
                                ToastSeverity::WARNING,
                                lv_tr("Saved to Spoolman, but the spool's price, location, lot "
                                      "and notes didn't save."),
                                4000);
                        }
                        deliver();
                    });
                };
                api_->spoolman().update_spoolman_spool(
                    saved_spool_id, cap.chained_spool_patch, [finish]() { finish(true); },
                    [finish](const MoonrakerError& err) {
                        spdlog::error("[AmsEditOverlay] chained logistics PATCH failed: {}",
                                      err.message);
                        finish(false);
                    });
            });
        });
}

bool AmsEditOverlay::may_write_spoolman_now(const SlotInfo& original, const SlotInfo& edited) {
    return !needs_identity_confirmation(original, edited);
}

bool AmsEditOverlay::is_material_identity_change(const SlotInfo& original, const SlotInfo& edited) {
    if (!helix::FilamentMapper::materials_match(original.material, edited.material)) {
        return true;
    }
    // A brand-only move changes which filament the spool should point at,
    // exactly like a material move; Spoolman's detect_changes includes it.

    if (!helix::text_io::equals_ignore_case(original.brand, edited.brand)) {
        return true;
    }
    return !helix::FilamentMapper::colors_match(original.color_rgb, edited.color_rgb);
}

void AmsEditOverlay::prompt_identity_change_then_save() {
    // A Spoolman-linked slot whose material/brand/colour changed. The two real
    // outcomes: move the spool to the matching filament (creating one if
    // needed — the saver repoints, it never edits the shared filament record),
    // or unlink and keep the slot local. Cancel/dismiss aborts the save
    // entirely: nothing written, edits stay staged.
    auto true_abort = [] { get_ams_edit_overlay().stay_open_after_failed_save(); };

    ConfirmOptions opts;
    opts.cancel_text = lv_tr("Unlink, keep on this slot");
    opts.on_cancel = [] {
        // Keep the slot's edited values lane-local; nothing reaches Spoolman.
        auto& editor = get_ams_edit_overlay();
        editor.working_info_.unlink_as_different_filament(editor.original_info_);
        editor.close_editor(true);
    };
    opts.on_dismiss = true_abort;
    opts.owner_token = lifetime_.token();
    lv_obj_t* dlg = modal_confirm(
        lv_tr("Different filament?"),
        lv_tr("This doesn't match the linked Spoolman spool. Update it in Spoolman, or unlink and "
              "keep it on this slot?"),
        ModalSeverity::Warning, lv_tr("Update in Spoolman"),
        [] {
            get_ams_edit_overlay().do_spoolman_save(
                helix::SpoolmanSlotSaver::LinkIntent::UpdateLinked);
        },
        opts);
    if (!dlg) {
        spdlog::warn("[AmsEditOverlay] identity confirmation failed to show; aborting save");
        close_editor(false);
    }
}

void AmsEditOverlay::offer_similar_filament(
    const std::string& filament_name, int slot_index,
    std::function<void(helix::SpoolmanSlotSaver::SimilarChoice)> answer) {
    using Choice = helix::SpoolmanSlotSaver::SimilarChoice;
    // One reply per offer, whichever of the three paths reaches it.
    auto reply = std::make_shared<std::function<void(Choice)>>(std::move(answer));
    auto send = [reply](Choice choice) {
        if (*reply) {
            auto once = std::move(*reply);
            *reply = nullptr;
            once(choice);
        }
    };

    ConfirmOptions opts;
    opts.cancel_text = lv_tr("Add new");
    opts.on_cancel = [send] { send(Choice::CreateNew); };
    opts.on_dismiss = [send] { send(Choice::Cancel); };
    // The save outlives the editor, so the answer must too: the editor closing
    // under the dialog would otherwise strand the slot's save for good.
    opts.owner_token = object_lifetime_.token();
    // Named by lane: the editor may have closed, and the dialog then stands
    // over whatever screen is up.
    const std::string message = fmt::format(
        fmt::runtime(lv_tr("Spoolman already has \"{}\" from this brand in a close color. Use "
                           "it for lane {}, or add a new filament?")),
        filament_name, helix::ui::lane_number(slot_index));
    lv_obj_t* dlg = modal_confirm(
        lv_tr("Use existing filament?"), message.c_str(), ModalSeverity::Info,
        lv_tr("Use existing"), [send] { send(Choice::UseExisting); }, opts);
    if (!dlg) {
        spdlog::warn("[AmsEditOverlay] similar-filament offer failed to show; adding a new one");
        send(Choice::CreateNew);
    }
}

// ============================================================================
// Callback Registration
// ============================================================================

void AmsEditOverlay::register_callbacks() {
    register_xml_callbacks({
        {"ams_edit_back_cb", [](lv_event_t*) { get_ams_edit_overlay().handle_back(); }},
        {"ams_edit_different_spool_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_different_spool(); }},
        {"ams_edit_assign_local_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_assign_local(); }},
        {"ams_edit_remaining_minus_cb",
         [](lv_event_t*) {
             get_ams_edit_overlay().handle_remaining_step(-spool_assign::kRemainingStepGrams);
         }},
        {"ams_edit_remaining_plus_cb",
         [](lv_event_t*) {
             get_ams_edit_overlay().handle_remaining_step(spool_assign::kRemainingStepGrams);
         }},
        {"ams_edit_size_chip_cb",
         [](lv_event_t* e) {
             // size_chip_<n>
             const char* name = lv_obj_get_name(lv_event_get_current_target_obj(e));
             const char* digit = name ? std::strrchr(name, '_') : nullptr;
             if (digit) {
                 get_ams_edit_overlay().handle_size_chip(std::atoi(digit + 1));
             }
         }},
        {"ams_edit_common_spool_cb",
         [](lv_event_t* e) {
             // common_spool_<n>
             const char* name = lv_obj_get_name(lv_event_get_current_target_obj(e));
             const char* digit = name ? std::strrchr(name, '_') : nullptr;
             if (digit) {
                 get_ams_edit_overlay().handle_common_spool(
                     static_cast<size_t>(std::atoi(digit + 1)));
             }
         }},
        {"ams_edit_row_use_cb",
         [](lv_event_t* e) {
             // The Use button sits in its row; the row carries its position.
             lv_obj_t* row = lv_obj_get_parent(lv_event_get_current_target_obj(e));
             const auto index =
                 static_cast<size_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
             get_ams_edit_overlay().handle_row_use(index);
         }},
        {"ams_edit_quick_swatch_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_quick_swatch(lv_event_get_target_obj(e));
         }},
        {"ams_edit_custom_color_cb", [](lv_event_t*) { get_ams_edit_overlay().open_color_view(); }},
        {"ams_edit_reset_tag_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_reset_to_tag(); }},
        {"ams_edit_clear_spool_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_clear_spool(); }},
        {"ams_edit_reapply_spool_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_reapply_linked_spool(); }},
        {"ams_edit_unlink_spool_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_unlink_disagreeing_spool(); }},
        {"ams_edit_color_swatch_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_color_swatch(lv_event_get_target_obj(e));
         }},
        {"ams_edit_color_apply_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_color_apply(); }},
        {"ams_edit_color_show_picker_cb",
         [](lv_event_t*) { get_ams_edit_overlay().show_color_pane(1); }},
        {"ams_edit_color_show_presets_cb",
         [](lv_event_t*) { get_ams_edit_overlay().show_color_pane(0); }},
        {"ams_edit_color_hex_changed_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_color_hex_changed(); }},
        {"ams_edit_detail_field_changed_cb",
         [](lv_event_t*) {
             auto& editor = get_ams_edit_overlay();
             editor.read_detail_fields();
             editor.update_sync_button_state();
         }},
        {"ams_edit_save_cb", [](lv_event_t*) { get_ams_edit_overlay().handle_save(); }},
        {"ams_edit_more_toggle_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_more_toggle(); }},
        {"ams_edit_lane_toggle_cb",
         [](lv_event_t*) {
             auto& editor = get_ams_edit_overlay();
             lv_subject_set_int(&editor.lane_open_subject_,
                                lv_subject_get_int(&editor.lane_open_subject_) ? 0 : 1);
         }},
        {"ams_edit_weight_cb", [](lv_event_t*) { get_ams_edit_overlay().handle_weight_tap(); }},
        {"ams_edit_add_to_spoolman_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_add_to_spoolman(); }},
        {"ams_edit_material_dd_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_option_selected(0, helix::ui::event_selected(e));
         }},
        {"ams_edit_variant_dd_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_option_selected(1, helix::ui::event_selected(e));
         }},
        {"ams_edit_brand_dd_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_option_selected(2, helix::ui::event_selected(e));
         }},
        {"ams_edit_product_dd_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_option_selected(3, helix::ui::event_selected(e));
         }},
        {"ams_edit_tool_dd_cb",
         [](lv_event_t* e) { get_ams_edit_overlay().handle_tool_selected(event_selected(e)); }},
        {"ams_edit_backup_dd_cb",
         [](lv_event_t* e) { get_ams_edit_overlay().handle_backup_selected(event_selected(e)); }},
        {"ams_edit_print_label_cb",
         [](lv_event_t*) {
#if HELIX_HAS_LABEL_PRINTER
             get_ams_edit_overlay().handle_print_label();
#endif
         }},
        {"ams_edit_scan_qr_cb", [](lv_event_t*) { get_ams_edit_overlay().handle_scan_qr(); }},
        {"ams_edit_picker_search_cb",
         [](lv_event_t* e) {
             const char* text = lv_textarea_get_text(lv_event_get_target_obj(e));
             get_ams_edit_overlay().picker_search_debounce_.schedule(text ? text : "");
         }},
        {"ams_edit_picker_retry_cb",
         [](lv_event_t*) {
             spdlog::info("[AmsEditOverlay] Picker retry requested by user");
             get_ams_edit_overlay().populate_picker();
         }},
    });
}

} // namespace helix::ui
