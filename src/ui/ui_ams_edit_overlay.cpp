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
#include "ui_swatch.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "ams_tag_reading.h"
#include "app_globals.h"
#include "color_utils.h"
#include "display_numbering.h"
#include "filament_database.h"
#include "filament_display_name.h"
#include "filament_variants.h"
#include "format_utils.h"
#include "label_printer_settings.h"
#include "lane_translation.h"
#include "spoolman_manager.h"
#include "static_panel_registry.h"
#include "text_io.h"
#include "ui/ui_widget_helpers.h"
#if HELIX_HAS_LABEL_PRINTER
#include "ipp_print_modal.h"
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
    // Picker-entry mode: a selection commits + closes. Cleared when the picker
    // is later re-entered via the header Spoolman button.
    opened_on_picker_ = open_on_picker;
    api_ = api;
    completion_callback_ = std::move(on_complete);
    completion_fired_ = false;
    save_in_flight_ = false;
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
    picker_search_debounce_.cancel();

    // Always prefer the active screen so the overlay renders above everything
    lv_obj_t* screen = lv_screen_active();
    if (!show(screen ? screen : parent)) {
        spdlog::error("[AmsEditOverlay] Failed to push overlay for slot {}", slot_index);
        return false;
    }
    lv_subject_set_int(&more_open_subject_, 0);
    // After show(): the first one initialises the subjects this writes.
    load_lane_settings();

    // Reset per-session view state HERE (covered-safe — on_deactivate must not
    // touch it, since it also fires when the QR scanner merely covers us).
    if (open_on_picker) {
        set_view(kViewSpoolPicker);
        populate_picker();
    } else {
        enter_editor();
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
    }
}

void AmsEditOverlay::on_deactivating(DeactivateReason) {
    // Fires when POPPED **and** when COVERED (e.g. QR scanner pushed on top).
    // Must NOT fire completion or reset the view subject — session state
    // resets happen in show_for_slot(). A pending debounced search belongs to
    // the session being left.
    picker_search_debounce_.cancel();
    // lifetime_ is invalidated right after this hook: its fetches die here.
    linked_fetch_pending_ = false;
    recents_pending_ = false;
    spdlog::debug("[AmsEditOverlay] on_deactivating()");
}

// ============================================================================
// Subject Management
// ============================================================================

void AmsEditOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        lv_subject_init_string(&title_subject_, title_buf_, nullptr, sizeof(title_buf_), "--");
        subjects_.register_subject(&title_subject_);

        // View state (kViewEditor..kViewColor) - registered globally
        UI_MANAGED_SUBJECT_INT(view_mode_subject_, 0, "ams_edit_view", subjects_);

        // Picker state (0=loading, 1=empty, 2=content) - registered globally
        UI_MANAGED_SUBJECT_INT(picker_state_subject_, 0, "edit_picker_state", subjects_);

        // Header Save button dirty gate (1=disabled). Starts disabled — nothing
        // is dirty when the editor opens.
        UI_MANAGED_SUBJECT_INT(save_disabled_subject_, 1, "ams_edit_save_disabled", subjects_);

        // Header Save button visibility gate (1=hidden). Save applies to the
        // editor view only; picker and color views hide it. Written
        // exclusively from set_view().
        UI_MANAGED_SUBJECT_INT(save_hidden_subject_, 0, "ams_edit_save_hidden", subjects_);

        // Managed-vs-untracked signal: drives the header Spoolman badge, the
        // More section's Spoolman-owned fields and the Add-link's absence.
        UI_MANAGED_SUBJECT_INT(is_managed_subject_, 0, "ams_edit_is_managed", subjects_);

        // Scan QR reachability: Spoolman availability AND a camera on this
        // build. The header button binds its hidden flag to this.
        UI_MANAGED_SUBJECT_INT(scan_enabled_subject_, 0, "ams_edit_scan_enabled", subjects_);

        // Row visibility for the Generic-gated product dropdown.
        UI_MANAGED_SUBJECT_INT(product_hidden_subject_, 1, "ams_edit_product_hidden", subjects_);
        UI_MANAGED_SUBJECT_INT(color_selected_subject_, -1, "ams_edit_color_selected", subjects_);
        UI_MANAGED_SUBJECT_INT(color_col_count_subject_, 8, "ams_edit_color_col_count", subjects_);
        UI_MANAGED_SUBJECT_INT(color_row_count_subject_, 29, "ams_edit_color_row_count", subjects_);
        // Colour view: both panes at once where they fit (update_color_fit),
        // else one at a time, switched by the custom swatch and Presets.
        UI_MANAGED_SUBJECT_INT(color_fits_subject_, 1, "ams_edit_color_fits", subjects_);
        UI_MANAGED_SUBJECT_INT(color_pane_subject_, 0, "ams_edit_color_pane", subjects_);

        // Summary line, temperatures, weight pill, badges, disclosure.
        UI_MANAGED_SUBJECT_STRING(summary_text_subject_, summary_buf_, "", "ams_edit_summary_text",
                                  subjects_);
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
        UI_MANAGED_SUBJECT_INT(add_spoolman_visible_subject_, 0, "ams_edit_add_spoolman_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(clear_visible_subject_, 0, "ams_edit_clear_visible", subjects_);
        UI_MANAGED_SUBJECT_INT(more_open_subject_, 0, "ams_edit_more_open", subjects_);

        // Spoolman spool number for the header badge ("#19").
        UI_MANAGED_SUBJECT_STRING(spoolman_id_subject_, spoolman_id_buf_, "",
                                  "ams_edit_spoolman_id", subjects_);

        // The lane's Assigned Tool and Endless Spool rows (load_lane_settings).
        UI_MANAGED_SUBJECT_INT(lane_visible_subject_, 0, "ams_edit_lane_visible", subjects_);
        UI_MANAGED_SUBJECT_INT(tool_row_visible_subject_, 0, "ams_edit_tool_row_visible",
                               subjects_);
        UI_MANAGED_SUBJECT_INT(backup_row_visible_subject_, 0, "ams_edit_backup_row_visible",
                               subjects_);
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
    deinit_subjects_base(subjects_);
}

// ============================================================================
// Identity dropdowns
// ============================================================================

void AmsEditOverlay::fill_dropdown(const char* stacked_name, const char* split_name,
                                   const std::vector<Option>& options_list,
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
    for (const char* name : {stacked_name, split_name}) {
        if (lv_obj_t* dd = name && name[0] ? find_widget(name) : nullptr) {
            if (options_list.empty()) {
                // No lines for this brand: an empty list, and no phantom
                // first-item selection.
                lv_dropdown_set_options(dd, "");
                continue;
            }
            lv_dropdown_set_options(dd, options.c_str());
            lv_dropdown_set_selected(dd, selected < 0 ? 0 : selected);
        }
    }
}

void AmsEditOverlay::refresh_dropdowns() {
    fill_dropdown("dropdown_material", "dropdown_material_split", material_options_, sel_.family);
    fill_dropdown("dropdown_variant", "dropdown_variant_split", variant_options_, sel_.variant);
    fill_dropdown("dropdown_brand", "dropdown_brand_split", brand_options_, sel_.brand);
    fill_dropdown("dropdown_product", "dropdown_product_split", product_options_, sel_.product_id);
}

void AmsEditOverlay::rebuild_option_lists() {
    if (!catalog_) {
        return;
    }
    // MATERIAL: recents hoisted over all families, then "Browse catalog...".
    std::vector<Option> material;
    std::vector<std::string> families = hoist_recents(catalog_->all_families(), recent_families_);
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
            lv_screen_active(), sel_.family, [](const helix::printer::EffectiveFilament& ef) {
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
    // Leaving the picker with a search still settling: the debounced rebuild
    // would re-render the now-hidden spool list mid-transition.
    if (lv_subject_get_int(&view_mode_subject_) == kViewSpoolPicker && view != kViewSpoolPicker) {
        picker_search_debounce_.cancel();
    }
    lv_subject_set_int(&view_mode_subject_, view);
    // Header Save is visible on the editor view only; the picker and the color
    // view hide it.
    lv_subject_set_int(&save_hidden_subject_, view == kViewEditor ? 0 : 1);
    update_sync_button_state();
}

void AmsEditOverlay::switch_to_picker() {
    if (!subjects_initialized_) {
        spdlog::warn("[AmsEditOverlay] switch_to_picker() aborted: subjects not initialized");
        return;
    }
    // Reaching the picker via the header button is the two-step flow — a
    // selection returns to the editor for review.
    opened_on_picker_ = false;
    set_view(kViewSpoolPicker);
    populate_picker();
}

void AmsEditOverlay::populate_picker() {
    if (!overlay_root_ || !api_) {
        spdlog::warn("[AmsEditOverlay] populate_picker() aborted: overlay_root_={}, api_={}",
                     static_cast<void*>(overlay_root_), static_cast<void*>(api_));
        lv_subject_set_int(&picker_state_subject_, 1);
        return;
    }

    picker_search_debounce_.cancel();
    cached_spools_.clear();
    cached_searchables_.clear();
    lv_subject_set_int(&picker_state_subject_, 0);

    lv_obj_t* search = helix::ui::find_required(overlay_root_, "picker_search", get_name());
    if (search) {
        lv_textarea_set_text(search, "");
    }

    auto token = lifetime_.token();
    api_->spoolman().get_spoolman_spools(
        [this, token](const std::vector<SpoolInfo>& spools) {
            spdlog::debug("[AmsEditOverlay] Spoolman returned {} spools", spools.size());
            token.defer([this, spools]() {
                if (!overlay_root_ || !subjects_initialized_) {
                    return;
                }
                if (spools.empty()) {
                    lv_subject_set_int(&picker_state_subject_, 1);
                    return;
                }
                // Spools arrive ordered by most recent activity first
                // (sort_spools_by_recency() in the API layer).
                cached_spools_ = spools;
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
            token.defer([this, msg = err.message]() {
                if (!overlay_root_ || !subjects_initialized_) {
                    return;
                }
                spdlog::warn("[AmsEditOverlay] Failed to fetch spools: {}", msg);
                lv_subject_set_int(&picker_state_subject_, 1);
            });
        });
}

void AmsEditOverlay::render_spool_list(const std::string& filter) {
    lv_obj_t* spool_list = helix::ui::find_required(overlay_root_, "picker_spool_list", get_name());
    if (!spool_list) {
        return;
    }

    // Invoked from a token.defer() callback (UpdateQueue batch). Sync
    // lv_obj_clean in that context corrupts LVGL's event linked list (#776).
    helix::ui::safe_clean_children(spool_list);

    auto filtered = filter_spools(cached_spools_, filter, cached_searchables_);
    // A lane's spool sits in ToolState under the tool the lane feeds, whose
    // number need not match the lane's. The external spool feeds through no
    // lane, so every lane's spool is taken.
    int own_tool = -1;
    if (AmsBackend* backend = slot_index_ >= 0 ? AmsState::instance().get_backend() : nullptr) {
        own_tool = backend->get_slot_info(slot_index_).mapped_tool;
    }
    auto in_use = ToolState::instance().assigned_spool_ids(own_tool);

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    UiBreakpoint bp = bp_subj ? as_breakpoint(lv_subject_get_int(bp_subj)) : UiBreakpoint::Medium;
    const bool is_compact = bp <= UiBreakpoint::Medium;
    const char* flow = is_compact ? "row" : "column";
    const char* name_grow = is_compact ? "1" : "0";
    const char* attrs_plain[] = {"detail_flow", flow, "name_grow", name_grow, nullptr, nullptr};
    const char* attrs_current[] = {"detail_flow",      flow,    "name_grow", name_grow,
                                   "hide_edit_pencil", "false", nullptr,     nullptr};

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

        lv_obj_t* name_label = helix::ui::find_required(item, "spool_name", get_name());
        if (name_label) {
            const std::string name = "#" + std::to_string(spool.id) + " " + spool.display_name();
            lv_label_set_text(name_label, name.c_str());
        }

        lv_obj_t* weight_label = helix::ui::find_required(item, "spool_weight", get_name());
        if (weight_label && spool.remaining_weight_g > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0fg", spool.remaining_weight_g);
            lv_label_set_text(weight_label, buf);
        }

        lv_obj_t* swatch = helix::ui::find_required(item, "spool_swatch", get_name());
        if (swatch) {
            uint32_t rgb = helix::parse_hex_color(spool.color_hex).value_or(0x808080);
            helix::ui::apply_swatch_color(swatch, rgb, spool.multi_color_hexes,
                                          helix::ui::swatch_finish(spool.finish()));
        }

        const bool disabled = in_use.count(spool.id) > 0;
        if (disabled) {
            lv_obj_add_state(item, LV_STATE_DISABLED);
            lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE);
        }

        bool is_selected = false;
        if (working_info_.spoolman_id > 0) {
            is_selected = (spool.id == working_info_.spoolman_id);
        } else if (!have_preselection && !disabled) {
            is_selected = true;
        }
        if (is_selected) {
            have_preselection = true;
            lv_obj_set_state(item, LV_STATE_CHECKED, true);
            lv_obj_t* check_icon = helix::ui::find_required(item, "selected_icon", get_name());
            if (check_icon) {
                lv_obj_remove_flag(check_icon, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_set_state(item, LV_STATE_CHECKED, false);
        }
    }

    lv_subject_set_int(&picker_state_subject_, filtered.empty() ? 3 : 2);
}

void AmsEditOverlay::handle_spool_selected(int spool_id) {
    spdlog::info("[AmsEditOverlay] Spool {} selected for slot {}", spool_id, slot_index_);

    for (const auto& spool : cached_spools_) {
        if (spool.id == spool_id) {
            apply_spool_to_slot(working_info_, spool);
            break;
        }
    }

    if (opened_on_picker_) {
        spdlog::debug("[AmsEditOverlay] Picker-entry selection - committing and closing");
        commit_and_close();
        return;
    }

    // Two-step flow: return to the editor with the picked spool loaded.
    load_swapped_spool();
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
    // The split layout shows at the short landscape class, the stacked one
    // everywhere else (the XML's bind_flag conditions).
    lv_subject_t* bp = theme_manager_get_breakpoint_subject();
    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    const bool split =
        bp && lv_subject_get_int(bp) == 0 && !(portrait && lv_subject_get_int(portrait) == 1);
    return lv_subject_get_int(split ? &color_col_count_subject_ : &color_row_count_subject_);
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
    // The colour areas are subject-counted (a literal <repeat> inside <if> is
    // refused by the engine), sized here from the measured grid width: whole
    // rows only, two in the split column and one in the stacked grid, which
    // leaves the lane's settings in view. The rainbow tile opens the rest.
    if (!overlay_root_) {
        return;
    }
    lv_obj_update_layout(overlay_root_);
    const int palette = static_cast<int>(swatch_palette(ColorPicker::Palette::General).size());
    const struct {
        const char* grid;
        const char* custom;
        int rows;
        lv_subject_t* count;
    } grids[] = {
        {"color_grid_split", "btn_custom_color_split", 2, &color_col_count_subject_},
        {"color_grid", "btn_custom_color", 1, &color_row_count_subject_},
    };
    for (const auto& g : grids) {
        lv_obj_t* grid = find_widget(g.grid);
        lv_obj_t* custom = find_widget(g.custom);
        if (!grid || !custom) {
            continue;
        }
        const int count =
            grid_preset_count(lv_obj_get_content_width(grid), lv_obj_get_width(custom),
                              lv_obj_get_style_pad_column(grid, LV_PART_MAIN), g.rows, palette);
        // A hidden layout measures 0 wide; it keeps the count it has.
        if (lv_obj_get_content_width(grid) > 0 && lv_subject_get_int(g.count) != count) {
            lv_subject_set_int(g.count, count);
        }
    }
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
    case kViewSpoolPicker:
        populate_picker();
        break;
    case kViewColor:
        populate_color_view();
        update_color_fit();
        break;
    case kViewEditor:
    default:
        populate_editor_view();
        refresh_tag_state();
        break;
    }
    spdlog::debug("[AmsEditOverlay] Repopulated view {}", view);
}

// ============================================================================
// Summary / temps / weight
// ============================================================================

void AmsEditOverlay::update_summary() {
    // Identity: "eSUN Silk PETG, Blue" — brand + the selected type + colour
    // name. The type comes from the SELECTION (live per tap), not the slot.
    const std::string type = catalog_ ? selected_type(sel_, *catalog_) : working_info_.material;
    const std::string color = details_color_set_
                                  ? helix::get_color_name_from_hex(details_color_)
                                  : helix::get_color_name_from_hex(working_info_.color_rgb);
    std::string label =
        helix::compose_filament_label(sel_.brand, working_info_.spool_name, type, " ");
    if (!color.empty()) {
        label += ", " + color;
    }
    snprintf(summary_buf_, sizeof(summary_buf_), "%s", label.c_str());
    lv_subject_copy_string(&summary_text_subject_, summary_buf_);

    // Colour dot: multi-colour spools paint the split; a picked colour paints
    // plain. Multi-colour shows read-only in the grid (the ring has no
    // multi-cell); picking replaces.
    lv_obj_t* dot = find_widget("summary_color_swatch");
    if (dot) {
        apply_swatch_color(dot, details_color_set_ ? details_color_ : working_info_.color_rgb,
                           details_color_set_ ? std::string() : working_info_.multi_color_hexes);
    }

    // Ring the visible preset the pending colour matches. A real colour no
    // visible preset holds rings the custom swatch (index -2); no colour, or
    // a multi-colour split, rings nothing.
    const uint32_t pending = details_color_set_ ? details_color_ : working_info_.color_rgb;
    const bool colour_set = details_color_set_ || (working_info_.multi_color_hexes.empty() &&
                                                   slot_has_colour(working_info_));
    lv_subject_set_int(&color_selected_subject_,
                       color_ring_index(pending, colour_set, visible_preset_count(),
                                        swatch_palette(ColorPicker::Palette::General)));

    // Weight pill: "750 g" + "of 1000 g".
    if (working_info_.remaining_weight_g >= 0) {
        snprintf(weight_buf_, sizeof(weight_buf_), "%.0f g", working_info_.remaining_weight_g);
    } else {
        snprintf(weight_buf_, sizeof(weight_buf_), "—");
    }
    lv_subject_copy_string(&weight_text_subject_, weight_buf_);
    if (working_info_.total_weight_g > 0) {
        snprintf(weight_hint_buf_, sizeof(weight_hint_buf_), "%s %.0f g", lv_tr("of"),
                 working_info_.total_weight_g);
    } else {
        weight_hint_buf_[0] = '\0';
    }
    lv_subject_copy_string(&weight_hint_subject_, weight_hint_buf_);
    update_tag_badges();
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
    if (!from_catalog && !type.empty() &&
        (selection_moved_ || (nozzle_min == 0 && nozzle_max == 0 && bed_temp == 0))) {
        if (auto mat = filament::find_material(type)) {
            nozzle_min = mat->nozzle_min;
            nozzle_max = mat->nozzle_max;
            bed_temp = mat->bed_temp;
        }
    }

    // One read-only line under the summary: "Nozzle 230–250 °C · Bed 80 °C".
    if (nozzle_min > 0 || nozzle_max > 0 || bed_temp > 0) {
        snprintf(temp_buf_, sizeof(temp_buf_),
                 "%s %d\xE2\x80\x93%d\xC2\xB0"
                 "C \xC2\xB7 %s %d\xC2\xB0"
                 "C",
                 lv_tr("Nozzle"), nozzle_min, nozzle_max, lv_tr("Bed"), bed_temp);
    } else {
        temp_buf_[0] = '\0';
    }
    lv_subject_copy_string(&temp_text_subject_, temp_buf_);
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

    // The quiet Add link: untracked slots reach Spoolman only through it.
    lv_subject_set_int(&add_spoolman_visible_subject_,
                       (has_spoolman && working_info_.spoolman_id == 0) ? 1 : 0);

    // Clear acts on what the slot holds now, not on the edits staged over it.
    lv_subject_set_int(&clear_visible_subject_,
                       AmsContextMenu::should_show_clear_spool(original_info_) ? 1 : 0);

    // Title: the slot's own name in the backend's lane noun ("Gate 3").
    if (slot_index_ == -2) {
        snprintf(title_buf_, sizeof(title_buf_), "%s", lv_tr("External Filament"));
    } else {
        snprintf(title_buf_, sizeof(title_buf_), "%s",
                 helix::ui::lane_label(helix::ui::active_lane_noun(), slot_index_).c_str());
    }
    lv_subject_copy_string(&title_subject_, title_buf_);

    // Spool number beside the tracked mark.
    if (managed) {
        snprintf(spoolman_id_buf_, sizeof(spoolman_id_buf_), "#%d", working_info_.spoolman_id);
    } else {
        spoolman_id_buf_[0] = '\0';
    }
    lv_subject_copy_string(&spoolman_id_subject_, spoolman_id_buf_);

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
    if (backend) {
        const AmsSystemInfo info = backend->get_system_info();
        lane_tool_count_ = info.total_slots;
        backup_candidates_ = lane_settings::backup_candidates(info.present_slots(), slot_index_);
        tool_row = lane_settings::show_tool_row(backend);
        if (tool_row) {
            lane_original_.tool = lane_settings::assigned_tool(*backend, slot_index_);
        }
        const auto caps = backend->get_endless_spool_capabilities();
        backup_row =
            lane_settings::show_backup_row(caps, backend->get_endless_spool_config(), slot_index_);
        if (backup_row) {
            lane_original_.backup = lane_settings::backup_of(*backend, slot_index_);
            backup_editable = caps.editable();
            lv_subject_copy_string(&backup_label_subject_,
                                   backup_lane_text(backend->lane_noun()).c_str());
        }
    }
    lane_working_ = lane_original_;
    lv_subject_set_int(&lane_visible_subject_, backend ? 1 : 0);
    lv_subject_set_int(&tool_row_visible_subject_, tool_row ? 1 : 0);
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
    ui_keypad_config_t config = {
        .initial_value =
            working_info_.remaining_weight_g > 0 ? working_info_.remaining_weight_g : 0,
        .min_value = 0,
        .max_value = 100000,
        .title_label = lv_tr("Remaining weight"),
        .unit_label = "g",
        .allow_decimal = false,
        .allow_negative = false,
        .callback =
            [](float value, void* user_data) {
                static_cast<AmsEditOverlay*>(user_data)->apply_weight(value);
            },
        .user_data = this};
    ui_keypad_show(&config);
}

void AmsEditOverlay::apply_weight(float grams) {
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
            SlotInfo scanned;
            apply_spool_to_slot(scanned, spool);
            helix::ui::queue_update(
                "AmsEditOverlay::handle_scan_qr", [scanned = std::move(scanned)]() {
                    auto& editor = get_ams_edit_overlay();
                    if (!editor.get_root()) {
                        return;
                    }
                    int keep_slot = editor.working_info_.slot_index;
                    int keep_global = editor.working_info_.global_index;
                    editor.working_info_ = scanned;
                    editor.working_info_.slot_index = keep_slot;
                    editor.working_info_.global_index = keep_global;
                    editor.load_swapped_spool();
                    NOTIFY_INFO("{} {} scanned — review and Save", scanned.brand, scanned.material);
                });
        },
        []() { spdlog::debug("[AmsEditOverlay] QR scan cancelled - editor resumes"); });
#endif
}

#if HELIX_HAS_LABEL_PRINTER
void AmsEditOverlay::handle_print_label() {
    auto& settings = helix::LabelPrinterSettingsManager::instance();

    if (!settings.is_configured()) {
        ToastManager::instance().show(ToastSeverity::INFO,
                                      lv_tr("Set up your label printer in Settings"), 3000);
        return;
    }

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
            working_info_.bed_temp = ef->bed_temp;
            working_info_.catalog_id = ef->id;
            working_info_.product_name = ef->name;
            // A Spoolman name describes the spool the old identity named.
            helix::ams::drop_stale_spool_name(original_info_, working_info_);
            return;
        }
    }
    // No product: temperatures follow the material's database range.
    working_info_.catalog_id.clear();
    working_info_.product_name.clear();
    if (auto mat = filament::find_material(type)) {
        working_info_.nozzle_temp_min = mat->nozzle_min;
        working_info_.nozzle_temp_max = mat->nozzle_max;
        working_info_.bed_temp = mat->bed_temp;
    }
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

void AmsEditOverlay::handle_back() {
    int view = lv_subject_get_int(&view_mode_subject_);
    if (save_in_flight_) {
        return; // the save's completion closes the editor
    }
    switch (view) {
    case kViewSpoolPicker:
    case kViewColor:
        return_to_editor();
        break;
    case kViewEditor:
    default:
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
    // The quiet link: create a Spoolman spool from what the slot holds (then
    // print its label). Stages the selection first so the created spool
    // carries the dropdowns' identity, not the pre-edit one.
    stage_selection_into_working();
    stage_pending_color_into_working();
    const helix::MissingFilamentFields missing =
        helix::SpoolmanSlotSaver::missing_filament_fields(working_info_);
    if (missing.any()) {
        show_missing_filament_toast(missing);
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

    read_detail_fields();
    if (detail_working_.price < 0) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Values must not be negative"),
                                      3000);
        return;
    }

    stage_selection_into_working();
    stage_pending_color_into_working();

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
        ToastManager::instance().show(
            ToastSeverity::WARNING,
            lv_tr("This spool's details haven't loaded from Spoolman yet. Try Save again."), 4000);
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
    logistics_pending_ = has_logistics_diff && details_loaded &&
                         !may_write_spoolman_now(original_info_, working_info_);
    if (details_loaded && api_ && !logistics_pending_ &&
        may_write_spoolman_now(original_info_, working_info_)) {
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
                    ToastManager::instance().show(ToastSeverity::ERROR,
                                                  lv_tr("Failed to save spool"), 3000);
                    if (cap.session == session_) {
                        save_in_flight_ = false;
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
    cap.original = original_info_;
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
        update_sync_button_state();
        ToastManager::instance().show(
            ToastSeverity::WARNING,
            lv_tr("Saved. Spoolman is offline, so price, location, lot and notes stay here "
                  "unsaved."),
            4000);
        return;
    }
    if (save_is_live(cap)) {
        working_info_ = committed;
        close_editor(true);
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

void AmsEditOverlay::commit_and_close() {
    commit_capture(capture_save());
}

void AmsEditOverlay::commit_capture(const SaveCapture& cap) {
    const bool live = save_is_live(cap);
    const SlotInfo& original = cap.original;
    const SlotInfo& saving = cap.saving;

    // Spoolman can go away between staging a brand or material and saving it.
    // The change has nowhere to go, and committing it locally would leave the
    // slot claiming an identity the server never heard.
    if (identity_locked(original, saving, get_printer_state().is_spoolman_available()) &&
        (saving.brand != original.brand || saving.material != original.material)) {
        ToastManager::instance().show(
            ToastSeverity::ERROR,
            lv_tr("Spoolman is offline, so brand and material can't be saved."), 3000);
        if (live) {
            stay_open_after_failed_save();
        }
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
        if (needs_identity_confirmation(original, saving)) {
            // Only an open editor can ask. A detached save never gets here: the
            // logistics PATCH that runs ahead of this needs no prompt.
            if (live) {
                prompt_identity_change_then_save();
            }
            return;
        }
        if (saving.spoolman_id > 0 &&
            helix::SpoolmanSlotSaver::detect_changes(original, saving).any()) {
            run_spoolman_save(cap, helix::SpoolmanSlotSaver::LinkIntent::UpdateLinked);
            return; // the save's completion commits
        }
    }

    // No Spoolman changes (or no Spoolman): commit locally now.
    deliver_commit(cap, saving);
}

void AmsEditOverlay::stay_open_after_failed_save() {
    update_sync_button_state();
}

void AmsEditOverlay::show_missing_filament_toast(const helix::MissingFilamentFields& missing) {
    const char* message = nullptr;
    if (missing.brand) {
        message = lv_tr("Spoolman needs a brand for this filament.");
    } else if (missing.material) {
        message = lv_tr("Spoolman needs a material for this filament.");
    } else if (missing.color) {
        message = lv_tr("Spoolman needs a color for this filament.");
    }
    if (message) {
        ToastManager::instance().show(ToastSeverity::ERROR, message, 3000);
    }
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
    // One save at a time: the editor stays open while this runs.
    if (cap.session == session_) {
        save_in_flight_ = true;
        update_sync_button_state();
    }
    begin_slot_save(cap.slot_index);
    saver->save(
        cap.original, cap.saving, intent,
        [this, token, cap, saver](const helix::SaveResult& result) {
            // Spoolman callback arrives on a background thread — defer
            // to the UI thread before touching LVGL.
            token.defer([this, cap, result]() {
                const bool live = save_is_live(cap);
                if (cap.session == session_) {
                    save_in_flight_ = false;
                }
                if (result.missing.any()) {
                    spdlog::info("[AmsEditOverlay] Spoolman save stopped: the filament is "
                                 "missing a field it is identified by");
                    show_missing_filament_toast(result.missing);
                    if (live) {
                        stay_open_after_failed_save();
                    }
                    end_slot_save(cap.slot_index);
                    return;
                }
                if (!result.success) {
                    spdlog::error("[AmsEditOverlay] Spoolman save failed; nothing written");
                    // A More PATCH that ran ahead of this save did land.
                    ToastManager::instance().show(
                        ToastSeverity::ERROR,
                        cap.more_saved
                            ? lv_tr("Spoolman saved the price, location, lot and notes, but "
                                    "not the rest. Nothing else changed on this printer.")
                            : lv_tr("Couldn't save to Spoolman. Nothing changed on this "
                                    "printer."),
                        4000);
                    if (live) {
                        stay_open_after_failed_save();
                    }
                    end_slot_save(cap.slot_index);
                    return;
                }
                SlotInfo committed = cap.saving;
                if (result.created_new_spool || result.repointed_filament) {
                    if (result.new_spool_id != 0) {
                        committed.spoolman_id = result.new_spool_id;
                    }
                    if (result.new_filament_id != 0) {
                        committed.spoolman_filament_id = result.new_filament_id;
                    }
                    if (result.new_vendor_id != 0) {
                        committed.spoolman_vendor_id = result.new_vendor_id;
                    }
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
                    repointed.vendor = committed.brand;
                    repointed.material = committed.material;
                    repointed.filament_name = committed.spool_name;
                    repointed.color_hex = helix::color_to_hex_string(committed.color_rgb);
                    if (auto* owner = AmsState::instance().get_backend()) {
#if !defined(HELIX_PLATFORM_ESP32)
                        // spoolman_manager.cpp is scoped out of the ESP32
                        // image; the repointed record there re-reads on the
                        // next poll instead.
                        SpoolmanManager::apply_fetched_spool(*owner, 0, cap.slot_index, repointed);
#endif
                    }
                }
                if (result.created_new_spool) {
                    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Added to Spoolman"),
                                                  2500);
                }
                if (live) {
                    working_info_ = committed;
#if HELIX_HAS_LABEL_PRINTER
                    // "Add to Spoolman + label": the new spool's label.
                    if (result.created_new_spool &&
                        helix::LabelPrinterSettingsManager::instance().is_configured()) {
                        handle_print_label();
                    }
#endif
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

// ============================================================================
// Callback Registration
// ============================================================================

void AmsEditOverlay::register_callbacks() {
    register_xml_callbacks({
        {"ams_edit_back_cb", [](lv_event_t*) { get_ams_edit_overlay().handle_back(); }},
        {"ams_edit_choose_spoolman_cb",
         [](lv_event_t*) { get_ams_edit_overlay().switch_to_picker(); }},
        {"ams_edit_setup_entry_cb", [](lv_event_t*) { get_ams_edit_overlay().return_to_editor(); }},
        {"ams_edit_quick_swatch_cb",
         [](lv_event_t* e) {
             get_ams_edit_overlay().handle_quick_swatch(lv_event_get_target_obj(e));
         }},
        {"ams_edit_custom_color_cb", [](lv_event_t*) { get_ams_edit_overlay().open_color_view(); }},
        {"ams_edit_reset_tag_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_reset_to_tag(); }},
        {"ams_edit_clear_spool_cb",
         [](lv_event_t*) { get_ams_edit_overlay().handle_clear_spool(); }},
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
        // Shared spool_item component callbacks.
        {"spoolman_spool_item_clicked_cb",
         [](lv_event_t* e) {
             lv_obj_t* item = lv_event_get_current_target_obj(e);
             auto spool_id =
                 static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(item)));
             if (spool_id <= 0) {
                 spdlog::warn("[AmsEditOverlay] Spool item clicked with invalid spool_id={}",
                              spool_id);
                 return;
             }
             get_ams_edit_overlay().handle_spool_selected(spool_id);
         }},
        {"spoolman_spool_item_edit_cb",
         [](lv_event_t*) { get_ams_edit_overlay().return_to_editor(); }},
    });
}

} // namespace helix::ui
