// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_slot_saver.h"

#include "color_utils.h"
#include "filament_database.h"
#include "filament_variants.h"
#include "i_moonraker_api.h"
#include "lane_translation.h"
#include "spoolman_catalog_search.h"
#include "spoolman_manager.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "hv/json.hpp"

namespace helix {

SpoolmanSlotSaver::SpoolmanSlotSaver(IMoonrakerAPI* api, float filament_diameter_mm)
    : api_(api), filament_diameter_mm_(filament_diameter_mm) {}

MissingFilamentFields SpoolmanSlotSaver::missing_filament_fields(const SlotInfo& slot) {
    MissingFilamentFields missing;
    missing.brand = slot.brand.empty();
    missing.material = slot.material.empty();
    // The default grey is a slot with no colour, not a grey filament.
    missing.color = !helix::ams::is_declarable_color(slot.color_rgb);
    return missing;
}

bool SpoolmanSlotSaver::is_filament_complete(const SlotInfo& slot) {
    return !missing_filament_fields(slot).any();
}

namespace {

/// The error a declined similar-filament offer travels as.
constexpr int kCancelledByUserCode = -32800;

} // namespace

bool SpoolmanSlotSaver::is_cancellation(const MoonrakerError& err) {
    return err.type == MoonrakerErrorType::VALIDATION_ERROR && err.code == kCancelledByUserCode;
}

NewFilament SpoolmanSlotSaver::new_filament_for(const SlotInfo& original, const SlotInfo& slot) {
    NewFilament f;
    f.precise_type = std::string(text_io::trim(slot.material));
    f.material = filament::slicer_material(f.precise_type);
    f.color_hex = color_to_hex(slot.color_rgb);

    // A spool name that is only the material names nothing Spoolman's own
    // material field does not, and one from before a brand, material or
    // colour change names the old identity.
    const bool identity_kept = slot.brand == original.brand && slot.material == original.material &&
                               slot.color_rgb == original.color_rgb;
    const std::string spool_name(text_io::trim(slot.spool_name));
    if (identity_kept && !spool_name.empty() &&
        !text_io::equals_ignore_case(spool_name, f.precise_type) &&
        !text_io::equals_ignore_case(spool_name, f.material)) {
        f.name = spool_name;
    } else {
        const std::string product =
            text_io::trim(slot.product_name).empty() ? f.precise_type : slot.product_name;
        const std::string color = text_io::trim(slot.color_name).empty()
                                      ? describe_color(slot.color_rgb & 0xFFFFFF)
                                      : slot.color_name;
        f.name = spoolman::new_filament_name(product, color);
    }
    if (f.name.empty()) {
        f.name = f.material;
    }

    f.extruder_temp = spoolman::temperature_for_range(slot.nozzle_temp_min, slot.nozzle_temp_max,
                                                      slot.nozzle_temp_recommended);
    f.bed_temp = std::max(slot.bed_temp, 0);
    return f;
}

ChangeSet SpoolmanSlotSaver::detect_changes(const SlotInfo& original, const SlotInfo& edited) {
    ChangeSet changes;

    // Filament-level: brand, material, color_rgb, catalog product identity.
    //
    // The product fields matter on their own: a variant swap within one vendor
    // and material (SUNLU "PLA Marble" -> "PLA+ 2.0") leaves brand, material and
    // color identical, so without them detect_changes() reports no change at all
    // and commit_and_close() treats the edit as a no-op.
    //
    // This does NOT make such a swap prompt "Different filament?" —
    // needs_identity_confirmation() additionally requires
    // is_material_identity_change(), which compares materials and colors only.
    // save_impl() writes no filament for it either: a catalog pick with
    // brand, material and colour unmoved leaves the spool on its record.
    if (original.brand != edited.brand || original.material != edited.material ||
        original.color_rgb != edited.color_rgb) {
        changes.filament_identity = true;
    }
    if (changes.filament_identity || original.catalog_id != edited.catalog_id ||
        original.product_name != edited.product_name) {
        changes.filament_level = true;
    }

    // Spool-level: remaining_weight_g / total_weight_g (float comparison with
    // threshold) or spoolman_id. total_weight_g maps to Spoolman's initial_weight
    // and was previously only ever sent at spool-creation time, so an edit to it on
    // a linked spool lit up Save and persisted locally while Spoolman kept the old
    // value indefinitely.
    if (std::abs(original.remaining_weight_g - edited.remaining_weight_g) > kWeightThreshold ||
        std::abs(original.total_weight_g - edited.total_weight_g) > kWeightThreshold ||
        original.spoolman_id != edited.spoolman_id) {
        changes.spool_level = true;
    }

    return changes;
}

void SpoolmanSlotSaver::build_spool_patches(const SpoolInfo& original, const SpoolInfo& edited,
                                            nlohmann::json& spool_patch,
                                            nlohmann::json& filament_patch) {
    // Spool-level fields (per-spool in Spoolman API)
    if (std::abs(edited.remaining_weight_g - original.remaining_weight_g) > 0.1) {
        spool_patch["remaining_weight"] = edited.remaining_weight_g;
    }
    if (std::abs(edited.price - original.price) > 0.001) {
        spool_patch["price"] = edited.price;
    }
    if (edited.lot_nr != original.lot_nr) {
        spool_patch["lot_nr"] = edited.lot_nr;
    }
    if (edited.comment != original.comment) {
        spool_patch["comment"] = edited.comment;
    }
    if (edited.location != original.location) {
        spool_patch["location"] = edited.location;
    }
    // The spool's own tare weight, which Spoolman copies from the filament
    // when the spool is created and reads from the spool from then on.
    if (std::abs(edited.spool_weight_g - original.spool_weight_g) > 0.1) {
        spool_patch["spool_weight"] = edited.spool_weight_g;
    }

    // Filament-level fields (affect all spools of this filament definition)
    if (edited.color_hex != original.color_hex) {
        filament_patch["color_hex"] = edited.color_hex;
    }
}

std::string SpoolmanSlotSaver::color_to_hex(uint32_t rgb) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06X", rgb & 0xFFFFFF);
    return std::string(buf);
}

void SpoolmanSlotSaver::save(const SlotInfo& original, const SlotInfo& edited,
                             CompletionCallback on_complete) {
    // Back-compat: reproduce the old inference exactly.
    save(original, edited,
         edited.spoolman_id ? LinkIntent::UpdateLinked : LinkIntent::CreateAndRebind,
         std::move(on_complete));
}

void SpoolmanSlotSaver::save(const SlotInfo& original, const SlotInfo& edited, LinkIntent intent,
                             CompletionCallback on_complete) {
    // A user edit is the one thing that makes an otherwise-immutable Spoolman
    // identity stale, so drop the cached record for every id this save could
    // touch: the spool that was linked, the spool that is linked now, and any
    // spool the save created. Invalidating on *completion* rather than on entry
    // matters — a weight poll landing mid-save would otherwise re-cache the old
    // server-side values and, since identity is extracted once per id, keep them
    // forever. Doing it on failure too is harmless: the next poll refills it.
    //
    // This runs on whichever thread the API completes on. invalidate_identity()
    // is mutex-guarded, shutdown-guarded and touches no LVGL, so no marshalling.
    const int original_id = original.spoolman_id;
    const int edited_id = edited.spoolman_id;
    adopted_.reset();
    CompletionCallback invalidating = [this, original_id, edited_id,
                                       on_complete =
                                           std::move(on_complete)](const SaveResult& result) {
        SpoolmanManager::invalidate_identity(original_id);
        SpoolmanManager::invalidate_identity(edited_id);
        SpoolmanManager::invalidate_identity(result.new_spool_id);
        SaveResult reported = result;
        if (reported.success && adopted_ &&
            (reported.created_new_spool || reported.repointed_filament)) {
            reported.adopted = adopted_;
        }
        if (on_complete) {
            on_complete(reported);
        }
    };
    on_complete = std::move(invalidating);

    // UnlinkLocalOnly never touches Spoolman: the lane stops being tracked and
    // its values stay local. Nothing to write, so report success immediately.
    if (intent == LinkIntent::UnlinkLocalOnly) {
        if (on_complete)
            on_complete(SaveResult{.success = true});
        return;
    }

    // CreateAndRebind means "this is a different physical spool". Force the
    // create path even when the slot still carries a link, and leave the
    // previously linked spool completely alone.
    if (intent == LinkIntent::CreateAndRebind) {
        SlotInfo fresh = edited;
        fresh.spoolman_id = 0;
        fresh.spoolman_filament_id = 0;
        fresh.spoolman_orca_filament_id.clear();
        save_impl(original, fresh, std::move(on_complete));
        return;
    }

    save_impl(original, edited, std::move(on_complete));
}

void SpoolmanSlotSaver::save_impl(const SlotInfo& original, const SlotInfo& edited,
                                  CompletionCallback on_complete) {
    // New-spool-on-save path — user entered manual filament info on an unlinked slot.
    if (!edited.spoolman_id) {
        const MissingFilamentFields missing = missing_filament_fields(edited);
        if (missing.any()) {
            // A new spool is a new filament, and Spoolman identifies one by
            // vendor, material and colour. Nothing is sent, and the caller is
            // told which field it wanted rather than that the save succeeded.
            spdlog::info("[SpoolmanSlotSaver] A new spool needs a brand, a material and a "
                         "colour (brand missing: {}, material missing: {}, colour missing: {})",
                         missing.brand, missing.material, missing.color);
            if (on_complete)
                on_complete(SaveResult{.success = false, .missing = missing});
            return;
        }

        const float weight = edited.remaining_weight_g;
        // Spoolman rejects a spool POST (400) when it can't determine the spool's
        // initial weight: no initial_weight in the request AND filament.weight is null
        // on the newly-created filament. Prefer the slot's total_weight_g if the user
        // specified one; otherwise fall back to the standard 1 kg consumer spool.
        const double initial_weight =
            edited.total_weight_g > 0 ? static_cast<double>(edited.total_weight_g) : 1000.0;

        const NewFilament filament = new_filament_for(original, edited);
        find_or_create_vendor(
            edited.brand,
            [this, filament, weight, initial_weight, on_complete](int vendor_id) {
                find_or_create_filament(
                    vendor_id, filament,
                    [this, vendor_id, weight, initial_weight, on_complete](int filament_id) {
                        nlohmann::json payload;
                        payload["filament_id"] = filament_id;
                        payload["initial_weight"] = initial_weight;
                        if (weight > 0.0f) {
                            payload["remaining_weight"] = static_cast<double>(weight);
                        }
                        spdlog::info("[SpoolmanSlotSaver] Creating new spool "
                                     "(filament_id={}, initial={:.1f}g, remaining={:.1f}g)",
                                     filament_id, initial_weight, weight);
                        api_->spoolman().create_spoolman_spool(
                            payload,
                            [vendor_id, filament_id, on_complete](const SpoolInfo& info) {
                                SaveResult out;
                                out.success = true;
                                out.created_new_spool = true;
                                out.new_spool_id = info.id;
                                out.new_filament_id = filament_id;
                                out.new_vendor_id = vendor_id;
                                if (on_complete)
                                    on_complete(out);
                            },
                            [on_complete](const MoonrakerError& err) {
                                spdlog::error(
                                    "[SpoolmanSlotSaver] create_spoolman_spool failed: {}",
                                    err.message);
                                if (on_complete)
                                    on_complete(SaveResult{.success = false});
                            });
                    },
                    [on_complete](const MoonrakerError& err) {
                        const bool cancelled = is_cancellation(err);
                        if (!cancelled) {
                            spdlog::error("[SpoolmanSlotSaver] create filament failed: {}",
                                          err.message);
                        }
                        if (on_complete)
                            on_complete(SaveResult{.success = false, .cancelled = cancelled});
                    });
            },
            [on_complete](const MoonrakerError& err) {
                spdlog::error("[SpoolmanSlotSaver] create vendor failed: {}", err.message);
                if (on_complete)
                    on_complete(SaveResult{.success = false});
            });
        return;
    }

    auto changes = detect_changes(original, edited);

    // No changes detected
    if (!changes.any()) {
        spdlog::debug("[SpoolmanSlotSaver] No changes detected for spool {}", edited.spoolman_id);
        if (on_complete)
            on_complete(SaveResult{.success = true});
        return;
    }

    const int spool_id = edited.spoolman_id;

    // Only spool-level (weight) change
    if (!changes.filament_level && changes.spool_level) {
        spdlog::info("[SpoolmanSlotSaver] Updating weight for spool {} to {:.1f}g", spool_id,
                     edited.remaining_weight_g);
        update_weight(spool_id, edited.remaining_weight_g, on_complete);
        return;
    }

    // Filament-level change (possibly also weight)
    if (changes.filament_level) {
        spdlog::info("[SpoolmanSlotSaver] Filament-level change for spool {} "
                     "(brand={}, material={}, color={:#08x})",
                     spool_id, edited.brand, edited.material, edited.color_rgb);

        const MissingFilamentFields missing = missing_filament_fields(edited);
        if (missing.any() || !changes.filament_identity) {
            if (changes.filament_identity) {
                // The user moved one of the three fields a filament is
                // identified by, and the slot cannot name all three. One Save
                // is one outcome, so the weight waits with the rest rather
                // than landing beside a failure.
                spdlog::info("[SpoolmanSlotSaver] Spool {} needs a brand, a material and a "
                             "colour (brand missing: {}, material missing: {}, colour "
                             "missing: {})",
                             spool_id, missing.brand, missing.material, missing.color);
                if (on_complete)
                    on_complete(SaveResult{.success = false, .missing = missing});
                return;
            }
            // A catalog pick is HelixScreen's own: with brand, material and
            // colour where they were, Spoolman has nothing to file, and the
            // rest of the save stands.
            spdlog::info("[SpoolmanSlotSaver] Catalog pick alone for spool {}; no filament write",
                         spool_id);
            if (changes.spool_level) {
                update_weight(spool_id, edited.remaining_weight_g, on_complete);
                return;
            }
            if (on_complete)
                on_complete(SaveResult{.success = true});
            return;
        }

        NewFilament filament = new_filament_for(original, edited);
        filament.linked_filament_id = original.spoolman_filament_id;
        const float weight = edited.remaining_weight_g;
        const int original_filament_id = original.spoolman_filament_id;
        const bool weight_changed = changes.spool_level;

        find_or_create_vendor(
            edited.brand,
            [this, filament, spool_id, weight, original_filament_id, weight_changed,
             on_complete](int vendor_id) {
                find_or_create_filament(
                    vendor_id, filament,
                    [this, spool_id, weight, vendor_id, original_filament_id, weight_changed,
                     on_complete](int filament_id) {
                        // If we resolved to the SAME filament, skip repoint
                        // (but still do weight if that also changed).
                        if (filament_id == original_filament_id) {
                            spdlog::debug("[SpoolmanSlotSaver] Resolved to same filament_id={}, "
                                          "skipping repoint",
                                          filament_id);
                            if (weight_changed) {
                                update_weight(
                                    spool_id, weight,
                                    [vendor_id, filament_id, on_complete](const SaveResult& r) {
                                        SaveResult out = r;
                                        out.new_vendor_id = vendor_id;
                                        out.new_filament_id = filament_id;
                                        if (on_complete)
                                            on_complete(out);
                                    });
                                return;
                            }
                            SaveResult out;
                            out.success = true;
                            out.new_vendor_id = vendor_id;
                            out.new_filament_id = filament_id;
                            if (on_complete)
                                on_complete(out);
                            return;
                        }

                        // Different filament — repoint the spool at it.
                        repoint_spool(
                            spool_id, filament_id,
                            [this, spool_id, weight, vendor_id, filament_id, weight_changed,
                             on_complete]() {
                                if (weight_changed) {
                                    update_weight(
                                        spool_id, weight,
                                        [vendor_id, filament_id, on_complete](const SaveResult& r) {
                                            SaveResult out = r;
                                            out.repointed_filament = true;
                                            out.new_vendor_id = vendor_id;
                                            out.new_filament_id = filament_id;
                                            if (on_complete)
                                                on_complete(out);
                                        });
                                    return;
                                }
                                SaveResult out;
                                out.success = true;
                                out.repointed_filament = true;
                                out.new_vendor_id = vendor_id;
                                out.new_filament_id = filament_id;
                                if (on_complete)
                                    on_complete(out);
                            },
                            [on_complete](const MoonrakerError& err) {
                                spdlog::error("[SpoolmanSlotSaver] repoint_spool failed: {}",
                                              err.message);
                                if (on_complete)
                                    on_complete(SaveResult{.success = false});
                            });
                    },
                    [on_complete](const MoonrakerError& err) {
                        const bool cancelled = is_cancellation(err);
                        if (!cancelled) {
                            spdlog::error("[SpoolmanSlotSaver] find_or_create_filament failed: {}",
                                          err.message);
                        }
                        if (on_complete)
                            on_complete(SaveResult{.success = false, .cancelled = cancelled});
                    });
            },
            [on_complete](const MoonrakerError& err) {
                spdlog::error("[SpoolmanSlotSaver] find_or_create_vendor failed: {}", err.message);
                if (on_complete)
                    on_complete(SaveResult{.success = false});
            });
        return;
    }
}

void SpoolmanSlotSaver::update_weight(int spool_id, float weight_g,
                                      CompletionCallback on_complete) {
    api_->spoolman().update_spoolman_spool_weight(
        spool_id, static_cast<double>(weight_g),
        [on_complete]() {
            spdlog::debug("[SpoolmanSlotSaver] Weight update succeeded");
            if (on_complete)
                on_complete(SaveResult{.success = true});
        },
        [on_complete](const MoonrakerError& err) {
            spdlog::error("[SpoolmanSlotSaver] Weight update failed: {}", err.message);
            if (on_complete)
                on_complete(SaveResult{.success = false});
        });
}

void SpoolmanSlotSaver::find_or_create_vendor(const std::string& vendor_name,
                                              VendorCallback on_found, ErrorCallback on_error) {
    const std::string needle = helix::text_io::to_lower(vendor_name);
    api_->spoolman().get_spoolman_vendors(
        [this, vendor_name, needle, on_found, on_error](const std::vector<VendorInfo>& vendors) {
            for (const auto& v : vendors) {
                if (helix::text_io::to_lower(v.name) == needle) {
                    spdlog::debug("[SpoolmanSlotSaver] Reusing vendor '{}' -> id={}", v.name, v.id);
                    if (on_found)
                        on_found(v.id);
                    return;
                }
            }
            nlohmann::json payload;
            payload["name"] = vendor_name;
            spdlog::info("[SpoolmanSlotSaver] Creating vendor '{}'", vendor_name);
            api_->spoolman().create_spoolman_vendor(
                payload,
                [on_found](const VendorInfo& info) {
                    if (on_found)
                        on_found(info.id);
                },
                on_error);
        },
        on_error);
}

void SpoolmanSlotSaver::find_or_create_filament(int vendor_id, const NewFilament& filament,
                                                FilamentCallback on_found, ErrorCallback on_error) {
    const std::string needle_color = spoolman::normalize_color_hex(filament.color_hex);
    if (needle_color.empty()) {
        spdlog::warn("[SpoolmanSlotSaver] Invalid color hex '{}', aborting find_or_create_filament",
                     filament.color_hex);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid color hex: " + filament.color_hex;
            on_error(err);
        }
        return;
    }
    NewFilament wanted = filament;
    wanted.color_hex = needle_color;
    api_->spoolman().get_spoolman_filaments(
        vendor_id,
        [this, vendor_id, wanted, on_found, on_error](const std::vector<FilamentInfo>& filaments) {
            // Only the exact record this save would file is reused without
            // asking; anything near it goes to the chooser below.
            if (const FilamentInfo* f = spoolman::find_matching_filament(
                    filaments, wanted.material, wanted.name, wanted.color_hex)) {
                spdlog::debug("[SpoolmanSlotSaver] Reusing filament id={} "
                              "(vendor={}, material={}, color={})",
                              f->id, vendor_id, wanted.material, wanted.color_hex);
                if (on_found)
                    on_found(f->id);
                return;
            }
            const FilamentInfo* similar = spoolman::find_similar_filament(
                filaments, wanted.material, wanted.color_hex, wanted.linked_filament_id);
            if (!similar || !similar_chooser_) {
                create_filament(vendor_id, wanted, on_found, on_error);
                return;
            }
            const FilamentInfo offered = *similar;
            const int similar_id = similar->id;
            spdlog::info("[SpoolmanSlotSaver] Offering filament id={} ('{}') before creating '{}'",
                         similar_id, similar->filament_name, wanted.name);
            similar_chooser_(*similar, [this, vendor_id, wanted, offered, similar_id, on_found,
                                        on_error](SimilarChoice choice) {
                switch (choice) {
                case SimilarChoice::UseExisting:
                    adopted_ = offered;
                    if (on_found)
                        on_found(similar_id);
                    return;
                case SimilarChoice::CreateNew:
                    create_filament(vendor_id, wanted, on_found, on_error);
                    return;
                case SimilarChoice::Cancel:
                    break;
                }
                spdlog::info("[SpoolmanSlotSaver] Save declined at the similar-filament offer");
                if (on_error) {
                    MoonrakerError err;
                    err.type = MoonrakerErrorType::VALIDATION_ERROR;
                    err.code = kCancelledByUserCode;
                    err.message = "Cancelled";
                    on_error(err);
                }
            });
        },
        on_error);
}

void SpoolmanSlotSaver::create_filament(int vendor_id, const NewFilament& filament,
                                        FilamentCallback on_found, ErrorCallback on_error) {
    nlohmann::json payload;
    payload["vendor_id"] = vendor_id;
    payload["material"] = filament.material;
    payload["color_hex"] = filament.color_hex;
    payload["name"] = filament.name;
    if (filament.extruder_temp > 0) {
        payload["settings_extruder_temp"] = filament.extruder_temp;
    }
    if (filament.bed_temp > 0) {
        payload["settings_bed_temp"] = filament.bed_temp;
    }
    // density and diameter are REQUIRED by Spoolman (no defaults in their API).
    // The precise type's density, else the recorded material's, else PLA's
    // 1.24 g/cm³. Diameter is the printer's, which the owner passes in.
    auto mat_info = filament::find_material(filament.precise_type);
    if (!mat_info || mat_info->density_g_cm3 <= 0.0f) {
        mat_info = filament::find_material(filament.material);
    }
    payload["density"] =
        (mat_info && mat_info->density_g_cm3 > 0.0f) ? mat_info->density_g_cm3 : 1.24;
    payload["diameter"] = filament_diameter_mm_;
    spdlog::info("[SpoolmanSlotSaver] Creating filament '{}' "
                 "(vendor={}, material={}, color={}, extruder={}, bed={})",
                 filament.name, vendor_id, filament.material, filament.color_hex,
                 filament.extruder_temp, filament.bed_temp);
    api_->spoolman().create_spoolman_filament(
        payload,
        [on_found](const FilamentInfo& info) {
            if (on_found)
                on_found(info.id);
        },
        on_error);
}

void SpoolmanSlotSaver::repoint_spool(int spool_id, int new_filament_id, VoidCallback on_success,
                                      ErrorCallback on_error) {
    nlohmann::json patch;
    patch["filament_id"] = new_filament_id;
    spdlog::info("[SpoolmanSlotSaver] Repointing spool {} -> filament {}", spool_id,
                 new_filament_id);
    api_->spoolman().update_spoolman_spool(spool_id, patch, on_success, on_error);
}

} // namespace helix
