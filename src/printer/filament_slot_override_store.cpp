// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_slot_override_store.h"

#include "ams_types.h"
#include "filament_catalog.h"
#include "filament_database.h"
#include "filament_slot_override.h"
#include "filament_variants.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_error.h"
#include "spoolman_types.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace helix::ams {

namespace {

/// helix_filament_id_source's value on a record whose `filament_id` is the
/// linked spool's own Orca id rather than the catalog pick's.
constexpr const char* kFilamentIdSourceSpoolman = "spoolman";

// Outer Moonraker DB key for a slot. NOTE the two styles have DIFFERENT bases:
//   Lane -> "lane<i+1>"  (1-based, AFC/Happy Hare convention)
//   Tool -> "T<i>"       (0-based, Orca/Mainsail tool convention)
// Both carry the SAME 0-based inner "lane" field (to_lane_data_record).
std::string format_lane_key(int slot_index, LaneKeyStyle style) {
    return style == LaneKeyStyle::Tool ? "T" + std::to_string(slot_index)
                                       : "lane" + std::to_string(slot_index + 1);
}

std::string format_iso8601(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    const size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf, n);
}

// Read `primary`, falling back to `alias` when primary is absent, JSON null or
// not a string. safe_string, because the shared namespace legitimately carries
// present-but-null values and .value() throws on them.
std::string string_with_alias(const nlohmann::json& j, const char* primary, const char* alias) {
    if (j.contains(primary) && j[primary].is_string())
        return helix::json_util::safe_string(j, primary);
    return helix::json_util::safe_string(j, alias);
}

int int_with_alias(const nlohmann::json& j, const char* primary, const char* alias) {
    const int value = helix::json_util::safe_int(j, primary, 0);
    return value != 0 ? value : helix::json_util::safe_int(j, alias, 0);
}

/// @p value as JSON, or null when it is the "unknown" value.
nlohmann::json text_or_null(const std::string& value) {
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

nlohmann::json positive_or_null(int value) {
    return value > 0 ? nlohmann::json(value) : nlohmann::json(nullptr);
}

nlohmann::json weight_or_null(float grams) {
    return grams >= 0.0f ? nlohmann::json(grams) : nlohmann::json(nullptr);
}

} // namespace

nlohmann::json to_lane_data_record(int slot_index, const FilamentSlotOverride& o,
                                   LaneKeyStyle style) {
    nlohmann::json j;
    j["lss_version"] = kLaneDataLssVersion;
    j["writer"] = kLaneDataWriter;
    // A string: OrcaSlicer drops a numeric lane. 0-based whatever the key style.
    j["lane"] = std::to_string(slot_index);
    j["lane_name"] =
        o.external_mirror ? std::string(kBypassLaneName) : format_lane_key(slot_index, style);
    j["loaded"] = o.loaded.has_value() ? nlohmann::json(*o.loaded) : nlohmann::json(nullptr);
    // Pure black is a colour; color_set, not the value, says whether there is one.
    if (o.color_set) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "#%06X", o.color_rgb & 0x00FFFFFFu);
        j["color"] = buf;
    } else {
        j["color"] = nullptr;
    }
    // Two strings. OrcaSlicer matches `material` by exact equality against its
    // library and falls back to a PLA preset on a miss, so `material` carries
    // a matchable string and `filament_type` the precise one ("ASA-GF").
    const std::string match =
        o.material.empty() ? std::string{} : filament::orca_match_type(o.material);
    j["material"] = text_or_null(match);
    j["filament_type"] = text_or_null(o.material);
    j["vendor_name"] = text_or_null(o.brand);
    j["name"] = text_or_null(o.spool_name);
    j["spool_id"] = positive_or_null(o.spoolman_id);
    j["spoolman_filament_id"] = positive_or_null(o.spoolman_filament_id);
    // The linked spool's own Orca id first, because the tracker wins on
    // identity (RFC 0001 §6); otherwise only an id the catalog records for this
    // one product, never a guess.
    j["filament_id"] = nullptr;
    if (o.spoolman_id > 0 && !o.spoolman_orca_filament_id.empty()) {
        j["filament_id"] = o.spoolman_orca_filament_id;
        j["helix_filament_id_source"] = kFilamentIdSourceSpoolman;
    } else if (!o.catalog_id.empty()) {
        if (std::string orca = helix::printer::FilamentCatalog::orca_id_for(o.catalog_id);
            !orca.empty())
            j["filament_id"] = std::move(orca);
    }
    // Resolved at emit time, so a material change picks up fresh defaults.
    const ResolvedTemps temps = resolved_temps(o);
    j["bed_temp"] = positive_or_null(temps.bed_temp);
    j["nozzle_temp"] = positive_or_null(temps.nozzle_temp);
    j["remaining_weight_g"] = weight_or_null(o.remaining_weight_g);
    j["total_weight_g"] = weight_or_null(o.total_weight_g);
    j["scan_time"] = nullptr;
    // HelixScreen's own keys. Omitted when empty, so a lane without them
    // squats no names in the shared namespace.
    if (!o.color_name.empty())
        j["helix_color_name"] = o.color_name;
    if (!o.catalog_id.empty())
        j["helix_catalog_id"] = o.catalog_id;
    if (!o.product_name.empty())
        j["helix_product_name"] = o.product_name;
    if (o.spoolman_vendor_id > 0)
        j["helix_spoolman_vendor_id"] = o.spoolman_vendor_id;
    if (!o.fingerprint.empty())
        j["helix_fingerprint"] = o.fingerprint;
    return j;
}

std::optional<std::pair<int, FilamentSlotOverride>> from_lane_data_record(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("lane"))
        return std::nullopt;
    int slot_index = 0;
    if (j["lane"].is_string()) {
        const auto parsed = helix::text_io::parse_leading<int>(j["lane"].get<std::string>());
        if (!parsed) {
            return std::nullopt;
        }
        slot_index = *parsed;
    } else if (j["lane"].is_number_integer()) {
        slot_index = j["lane"].get<int>();
    } else {
        return std::nullopt;
    }
    // Matches OrcaSlicer: a negative lane is never a slot.
    if (slot_index < 0)
        return std::nullopt;

    FilamentSlotOverride o;
    if (j.contains("color") && j["color"].is_string()) {
        const ColorReading color = read_lane_color(j["color"].get<std::string>());
        if (color.kind == ColorReadingKind::Observed) {
            o.color_rgb = color.rgb;
            o.color_set = true;
        }
    }
    // The precise type first, then the legacy spelling, then the matchable one
    // another writer may be the only one to carry.
    o.material = string_with_alias(j, "filament_type", "helix_material");
    if (o.material.empty())
        o.material = helix::json_util::safe_string(j, "material");
    o.brand = string_with_alias(j, "vendor_name", "vendor");
    o.spool_name = string_with_alias(j, "name", "spool_name");
    o.spoolman_id = helix::json_util::safe_int(j, "spool_id", 0);
    o.bed_temp = helix::json_util::safe_int(j, "bed_temp", 0);
    o.nozzle_temp = helix::json_util::safe_int(j, "nozzle_temp", 0);
    o.spoolman_vendor_id = int_with_alias(j, "helix_spoolman_vendor_id", "spoolman_vendor_id");
    // A numeric filament_id is Spoolman's filament id (Happy Hare's usage, RFC
    // 0001 §7); a string one is Orca's.
    o.spoolman_filament_id =
        int_with_alias(j, "spoolman_filament_id", "helix_spoolman_filament_id");
    if (o.spoolman_filament_id <= 0 && j.contains("filament_id") &&
        j["filament_id"].is_number_integer())
        o.spoolman_filament_id = j["filament_id"].get<int>();
    // Only an id our own record says came from Spoolman: a catalog-derived one
    // is recomputed on emit, and another tool's `filament_id` is not ours.
    if (helix::json_util::safe_string(j, "helix_filament_id_source") == kFilamentIdSourceSpoolman) {
        std::string orca = helix::json_util::safe_string(j, "filament_id");
        if (helix::is_usable_orca_filament_id(orca))
            o.spoolman_orca_filament_id = std::move(orca);
    }
    o.remaining_weight_g = helix::json_util::safe_float(j, "remaining_weight_g", -1.0f);
    o.total_weight_g = helix::json_util::safe_float(j, "total_weight_g", -1.0f);
    o.color_name = string_with_alias(j, "helix_color_name", "color_name");
    o.catalog_id = helix::json_util::safe_string(j, "helix_catalog_id");
    o.product_name = helix::json_util::safe_string(j, "helix_product_name");
    o.fingerprint = helix::json_util::safe_string(j, "helix_fingerprint");
    // helix_external marks the bypass records 1.1 betas wrote.
    o.external_mirror = helix::json_util::safe_string(j, "lane_name") == kBypassLaneName ||
                        helix::json_util::safe_bool(j, "helix_external", false);
    if (j.contains("loaded") && j["loaded"].is_boolean())
        o.loaded = j["loaded"].get<bool>();
    return std::make_pair(slot_index, o);
}

FilamentSlotOverride extras_only(const FilamentSlotOverride& o, ExtrasFields fields) {
    // No firmware holds the bypass spool, so its record keeps everything.
    if (o.external_mirror)
        return o;
    FilamentSlotOverride kept;
    if (fields.remaining_weight) {
        kept.remaining_weight_g = o.remaining_weight_g;
    }
    if (fields.brand) {
        kept.brand = o.brand;
    }
    if (fields.spool_name) {
        kept.spool_name = o.spool_name;
    }
    kept.color_name = o.color_name;
    kept.catalog_id = o.catalog_id;
    kept.product_name = o.product_name;
    kept.total_weight_g = o.total_weight_g;
    kept.fingerprint = o.fingerprint;
    return kept;
}

bool written_by_another_producer(const nlohmann::json& wire) {
    const std::string writer = helix::json_util::safe_string(wire, "writer");
    return !writer.empty() && writer != kLaneDataWriter;
}

std::unordered_map<int, LaneDataRecord>
parse_namespace_document(const nlohmann::json& namespace_doc, LaneKeyStyle key_style,
                         const std::string& log_tag) {
    std::unordered_map<int, LaneDataRecord> records;
    if (!namespace_doc.is_object())
        return records;

    // The outer key is advisory; the inner "lane" field is the only gate. Two
    // records naming one slot: a record under our canonical key beats one that
    // is not, otherwise the incumbent stays, so the outcome does not depend on
    // iteration order.
    std::unordered_map<int, bool> canonical_seen;
    for (auto it = namespace_doc.begin(); it != namespace_doc.end(); ++it) {
        const std::string& key = it.key();
        if (!it.value().is_object())
            continue;
        const auto parsed = from_lane_data_record(it.value());
        if (!parsed) {
            spdlog::debug("[FilamentSlotOverrideStore:{}] unreadable record under {}", log_tag,
                          key);
            continue;
        }
        const int idx = parsed->first;
        const bool canonical = (key == format_lane_key(idx, key_style));
        auto seen = canonical_seen.find(idx);
        if (seen == canonical_seen.end() || (canonical && !seen->second)) {
            if (seen != canonical_seen.end()) {
                spdlog::warn("[FilamentSlotOverrideStore:{}] two records name slot {}; taking "
                             "the one under {}",
                             log_tag, idx, key);
            }
            records[idx] = LaneDataRecord{parsed->second, it.value()};
            canonical_seen[idx] = canonical;
        }
    }
    return records;
}

ResolvedTemps resolved_temps(const FilamentSlotOverride& o) {
    ResolvedTemps r{o.bed_temp, o.nozzle_temp};
    if ((r.bed_temp == 0 || r.nozzle_temp == 0) && !o.material.empty()) {
        if (auto mat = filament::find_material(o.material)) {
            if (r.bed_temp == 0)
                r.bed_temp = mat->bed_temp;
            if (r.nozzle_temp == 0)
                r.nozzle_temp = mat->nozzle_recommended();
        }
    }
    return r;
}

void populate_temps_from_slot_info(FilamentSlotOverride& ovr, const SlotInfo& info) {
    ovr.bed_temp = info.bed_temp;
    if (info.nozzle_temp_min > 0 && info.nozzle_temp_max > info.nozzle_temp_min) {
        ovr.nozzle_temp = (info.nozzle_temp_min + info.nozzle_temp_max) / 2;
    } else if (info.nozzle_temp_min > 0) {
        ovr.nozzle_temp = info.nozzle_temp_min;
    } else {
        ovr.nozzle_temp = 0;
    }
}

FilamentSlotOverride user_override_from_slot_info(const SlotInfo& edited,
                                                  const std::string& material,
                                                  const FilamentSlotOverride* prior) {
    FilamentSlotOverride ovr;
    ovr.brand = edited.brand;
    ovr.spool_name = edited.spool_name;
    ovr.spoolman_id = edited.spoolman_id;
    ovr.spoolman_filament_id = edited.spoolman_filament_id;
    ovr.spoolman_vendor_id = edited.spoolman_vendor_id;
    ovr.spoolman_orca_filament_id = edited.spoolman_orca_filament_id;
    ovr.remaining_weight_g = edited.remaining_weight_g;
    ovr.total_weight_g = edited.total_weight_g;
    ovr.color_name = edited.color_name;
    // A linked spool owns its material, so a linked record keeps the spool's
    // spelling; a backend's normalized spelling is for firmware.
    ovr.material = edited.spoolman_id > 0 ? edited.material : material;
    ovr.catalog_id = edited.catalog_id;
    ovr.product_name = edited.product_name;
    if (is_declarable_color(edited.color_rgb)) {
        ovr.color_rgb = edited.color_rgb;
        ovr.color_set = true;
    }
    ovr.loaded = slot_status_reports_filament(edited.status);
    populate_temps_from_slot_info(ovr, edited);
    // Bookkeeping, not an editable value, so a rebuilt record inherits it.
    ovr.fingerprint = prior != nullptr ? prior->fingerprint : std::string{};
    return ovr;
}

FilamentSlotOverride user_override_from_slot_info(const SlotInfo& edited,
                                                  const FilamentSlotOverride* prior) {
    return user_override_from_slot_info(edited, edited.material, prior);
}

FilamentSlotOverride& stage_user_override(std::unordered_map<int, FilamentSlotOverride>& overrides,
                                          int slot_index, const SlotInfo& edited,
                                          const std::string& material) {
    const auto existing = overrides.find(slot_index);
    FilamentSlotOverride rebuilt = user_override_from_slot_info(
        edited, material, existing == overrides.end() ? nullptr : &existing->second);
    FilamentSlotOverride& staged = overrides[slot_index];
    staged = std::move(rebuilt);
    return staged;
}

FilamentSlotOverride& stage_user_override(std::unordered_map<int, FilamentSlotOverride>& overrides,
                                          int slot_index, const SlotInfo& edited) {
    return stage_user_override(overrides, slot_index, edited, edited.material);
}

FilamentSlotOverride&
stage_weight_override(std::unordered_map<int, FilamentSlotOverride>& overrides, int slot_index,
                      float remaining_weight_g, float total_weight_g) {
    FilamentSlotOverride& staged = overrides[slot_index];
    staged.remaining_weight_g = remaining_weight_g;
    if (total_weight_g >= 0.0f) {
        staged.total_weight_g = total_weight_g;
    }
    return staged;
}

FilamentSlotOverrideStore::FilamentSlotOverrideStore(IMoonrakerAPI* api, std::string backend_id,
                                                     LaneKeyStyle key_style, std::string ns)
    : api_(api), backend_id_(std::move(backend_id)), key_style_(key_style),
      namespace_(std::move(ns)) {}

void FilamentSlotOverrideStore::reload_async(ReloadCallback cb, std::function<void()> on_failure) {
    if (!cb) {
        return;
    }
    if (!api_) {
        if (on_failure) {
            on_failure();
        }
        return;
    }
    // Everything the callbacks read is carried by value: the store can be gone
    // before Moonraker's request tracker fires its error timeout.
    const LaneKeyStyle style = key_style_;
    const std::string id = backend_id_;
    const std::string ns = namespace_;
    const bool extras = holds_extras_only();
    ExtrasFields fields;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fields = extras_fields_;
    }
    const auto narrowed = [extras, fields](std::unordered_map<int, LaneDataRecord> records) {
        if (extras) {
            for (auto& [slot, entry] : records) {
                entry.record = extras_only(entry.record, fields);
            }
        }
        return records;
    };
    api_->database_get_namespace(
        namespace_,
        [style, id, cb, on_failure, narrowed](const nlohmann::json& value) {
            if (!value.is_object()) {
                if (on_failure) {
                    on_failure();
                }
                return;
            }
            nlohmann::json keys = nlohmann::json::object();
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!it.value().is_object()) {
                    keys[it.key()] = it.value();
                }
            }
            cb(narrowed(parse_namespace_document(value, style, id)), std::move(keys));
        },
        [id, ns, cb, on_failure](const MoonrakerError& err) {
            // A namespace nothing has written yet is empty, not unreachable.
            if (err.is_not_found()) {
                cb({}, nlohmann::json::object());
                return;
            }
            spdlog::debug("[FilamentSlotOverrideStore:{}] read of {} failed: {}", id, ns,
                          err.message);
            if (on_failure) {
                on_failure();
            }
        });
}

void FilamentSlotOverrideStore::adopt_read(const std::unordered_map<int, LaneDataRecord>& records) {
    std::lock_guard<std::mutex> lock(mutex_);
    read_adopted_ = true;
    writers_.clear();
    for (const auto& [slot, entry] : records) {
        writers_[slot] = helix::json_util::safe_string(entry.wire, "writer");
    }
}

FilamentSlotOverrideStore::ReadTicket FilamentSlotOverrideStore::read_ticket() const {
    return ReadTicket{clock_->landed.load(), clock_->in_flight.load() == 0};
}

bool FilamentSlotOverrideStore::read_still_current(const ReadTicket& ticket) const {
    return ticket.no_write_in_flight && clock_->in_flight.load() == 0 &&
           clock_->landed.load() == ticket.writes_landed;
}

bool FilamentSlotOverrideStore::write_in_flight() const {
    return clock_->in_flight.load() != 0;
}

std::string FilamentSlotOverrideStore::write_gate(int slot_index) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (read_required_ && !read_adopted_) {
            return "the lane records have not been read yet";
        }
    }
    return writer_refusal(slot_index);
}

std::string FilamentSlotOverrideStore::writer_refusal(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = writers_.find(slot_index);
    if (it == writers_.end() || it->second.empty() || it->second == kLaneDataWriter) {
        return {};
    }
    return fmt::format("lane {} in {} is written by {}", slot_index, namespace_, it->second);
}

void FilamentSlotOverrideStore::save_async(int slot_index, const FilamentSlotOverride& ovr,
                                           SaveCallback cb) {
    if (!api_) {
        if (cb)
            cb(false, "no API");
        return;
    }
    if (slot_index < 0) {
        if (cb)
            cb(false, "invalid slot_index");
        return;
    }
    if (std::string refusal = write_gate(slot_index); !refusal.empty()) {
        spdlog::warn("[FilamentSlotOverrideStore:{}] save refused: {}", backend_id_, refusal);
        if (cb)
            cb(false, refusal);
        if (on_write_failed_)
            on_write_failed_(slot_index);
        return;
    }
    ExtrasFields fields;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fields = extras_fields_;
    }
    nlohmann::json record = to_lane_data_record(
        slot_index, holds_extras_only() ? extras_only(ovr, fields) : ovr, key_style_);
    record["scan_time"] = format_iso8601(std::chrono::system_clock::now());
    // One key per slot: no read-modify-write, so edits on different slots
    // cannot race. Callbacks carry copies only; the store may be gone first.
    const std::string key = format_lane_key(slot_index, key_style_);
    const std::string backend_id_copy = backend_id_;
    ++clock_->in_flight;
    api_->database_post_item(
        namespace_, key, record,
        [cb, on_written = on_written_, clock = clock_]() {
            --clock->in_flight;
            ++clock->landed;
            if (on_written)
                on_written();
            if (cb)
                cb(true, "");
        },
        [cb, backend_id_copy, key, slot_index, on_failed = on_write_failed_,
         clock = clock_](const MoonrakerError& err) {
            --clock->in_flight;
            ++clock->landed;
            spdlog::warn("[FilamentSlotOverrideStore:{}] save failed for key {}: {}",
                         backend_id_copy, key, err.message);
            if (cb)
                cb(false, err.message);
            if (on_failed)
                on_failed(slot_index);
        });
}

void FilamentSlotOverrideStore::clear_async(int slot_index, SaveCallback cb) {
    if (!api_) {
        if (cb)
            cb(false, "no API");
        return;
    }
    if (slot_index < 0) {
        if (cb)
            cb(false, "invalid slot_index");
        return;
    }
    if (std::string refusal = write_gate(slot_index); !refusal.empty()) {
        spdlog::warn("[FilamentSlotOverrideStore:{}] clear refused: {}", backend_id_, refusal);
        if (cb)
            cb(false, refusal);
        if (on_write_failed_)
            on_write_failed_(slot_index);
        return;
    }
    const std::string key = format_lane_key(slot_index, key_style_);
    const std::string backend_id_copy = backend_id_;
    ++clock_->in_flight;
    api_->database_delete_item(
        namespace_, key,
        [cb, on_written = on_written_, clock = clock_]() {
            --clock->in_flight;
            ++clock->landed;
            if (on_written)
                on_written();
            if (cb)
                cb(true, "");
        },
        [cb, backend_id_copy, key, slot_index, on_failed = on_write_failed_,
         clock = clock_](const MoonrakerError& err) {
            --clock->in_flight;
            ++clock->landed;
            spdlog::warn("[FilamentSlotOverrideStore:{}] clear failed for key {}: {}",
                         backend_id_copy, key, err.message);
            if (cb)
                cb(false, err.message);
            if (on_failed)
                on_failed(slot_index);
        });
}

// =============================================================================
// Seated-lane persistence
// =============================================================================

void FilamentSlotOverrideStore::save_key_async(const std::string& key, nlohmann::json value,
                                               SaveCallback cb) {
    if (!api_) {
        if (cb)
            cb(false, "no API");
        return;
    }
    // Lifetime safety mirrors save_async: Moonraker's request tracker can fire
    // the error callback ~60s after this returns, well after the store may have
    // been destroyed. Value-capture only; no `this`.
    const std::string backend_id_copy = backend_id_;
    api_->database_post_item(
        namespace_, key, std::move(value),
        [cb, on_written = on_written_]() {
            if (on_written)
                on_written();
            if (cb)
                cb(true, "");
        },
        [cb, backend_id_copy, key](const MoonrakerError& err) {
            spdlog::warn("[FilamentSlotOverrideStore:{}] save {} failed: {}", backend_id_copy, key,
                         err.message);
            if (cb)
                cb(false, err.message);
        });
}

void FilamentSlotOverrideStore::clear_key_async(const std::string& key, SaveCallback cb) {
    if (!api_) {
        if (cb)
            cb(false, "no API");
        return;
    }
    // Value-capture only; no `this` (see save_key_async).
    const std::string backend_id_copy = backend_id_;
    api_->database_delete_item(
        namespace_, key,
        [cb, on_written = on_written_]() {
            if (on_written)
                on_written();
            if (cb)
                cb(true, "");
        },
        [cb, backend_id_copy, key](const MoonrakerError& err) {
            // A key already gone is the clear's goal.
            if (err.is_not_found()) {
                if (cb)
                    cb(true, "");
                return;
            }
            spdlog::warn("[FilamentSlotOverrideStore:{}] clear {} failed: {}", backend_id_copy, key,
                         err.message);
            if (cb)
                cb(false, err.message);
        });
}

void FilamentSlotOverrideStore::save_seated_slot_async(int slot_index, SaveCallback cb) {
    // Reject negative indices symmetrically with save_async — a negative seated
    // index is never a valid lane. "Nothing seated" goes through
    // clear_seated_slot_async, not a sentinel index.
    if (api_ && slot_index < 0) {
        if (cb)
            cb(false, "invalid slot_index");
        return;
    }
    save_key_async("seated", nlohmann::json(slot_index), std::move(cb));
}

void FilamentSlotOverrideStore::clear_seated_slot_async(SaveCallback cb) {
    clear_key_async("seated", std::move(cb));
}

std::optional<int> FilamentSlotOverrideStore::load_seated_slot_blocking() {
    if (!api_)
        return std::nullopt;

    // A callback firing after our local timeout must not touch a freed stack
    // frame, so the state is heap-owned and captured by value into the lambdas.
    struct SyncState {
        std::mutex m;
        std::condition_variable cv;
        bool done{false};
        bool got{false};
        nlohmann::json received;
    };
    auto state = std::make_shared<SyncState>();
    const std::string backend_id_copy = backend_id_;
    const std::string namespace_copy = namespace_;

    api_->database_get_item(
        namespace_, "seated",
        [state](const nlohmann::json& value) {
            std::lock_guard<std::mutex> lk(state->m);
            state->received = value;
            state->got = true;
            state->done = true;
            state->cv.notify_one();
        },
        [state, backend_id_copy, namespace_copy](const MoonrakerError& err) {
            // Missing key is the common case (nothing seated yet) — not an error
            // worth more than a debug line.
            spdlog::debug("[FilamentSlotOverrideStore:{}] database_get_item({}:seated) failed: {}",
                          backend_id_copy, namespace_copy, err.message);
            std::lock_guard<std::mutex> lk(state->m);
            state->done = true;
            state->cv.notify_one();
        });

    bool got_copy;
    nlohmann::json received_copy;
    {
        std::unique_lock<std::mutex> lk(state->m);
        state->cv.wait_for(lk, load_timeout_, [state] { return state->done; });
        got_copy = state->got;
        if (got_copy)
            received_copy = state->received;
    }

    if (!got_copy)
        return std::nullopt;
    // Must be a plain non-negative integer. The store doesn't know kNumPorts,
    // so the caller is responsible for the upper-bound range check.
    if (!received_copy.is_number_integer())
        return std::nullopt;
    int idx = received_copy.get<int>();
    if (idx < 0)
        return std::nullopt;
    return idx;
}

// =============================================================================
// Shared firmware -> lane_data mirror helper
// =============================================================================

bool mirror_firmware_to_lane_data(FilamentSlotOverrideStore* store,
                                  std::unordered_map<int, FilamentSlotOverride>& overrides,
                                  int slot_index, uint32_t firmware_color,
                                  const std::string& firmware_material, bool slot_has_filament,
                                  MirrorPolicy policy, const std::string& log_tag) {
    // No filament = no signal. Don't establish a phantom lane_data entry for
    // an empty slot; clear_override paths handle ejection.
    //
    // IMPORTANT: do NOT skip on firmware_color == 0 — pure black (0x000000) is
    // a legitimate filament color (the K2 reports loaded black PLA as RGB
    // "0000000"). Callers that need a "color not yet parsed" guard must apply
    // it BEFORE invoking this helper, e.g. AmsBackendAd5xIfs::
    // check_external_color_change short-circuits on observed_color == 0
    // because IFS's parse path may run with colors_[idx] still empty.
    if (!slot_has_filament)
        return false;

    auto& ovr = overrides[slot_index]; // creates default-constructed entry if absent
    bool changed = false;

    switch (policy) {
    case MirrorPolicy::OverwriteAlways: {
        // IFS / Snapmaker-extended: user edits CAN reach firmware (IFS:
        // direct Adventurer5M.json write; Snapmaker paxx12: POST endpoint).
        // In steady state firmware-truth and user-truth converge, so this
        // policy bootstraps an empty override AND catches genuine external
        // edits (Mainsail console, native LCD, etc.).

        if (!ovr.color_set || ovr.color_rgb != firmware_color) {
            ovr.color_rgb = firmware_color;
            ovr.color_set = true;
            changed = true;
        }
        if (ovr.material != firmware_material) {
            ovr.material = firmware_material;
            changed = true;
        }
        break;
    }
    case MirrorPolicy::FillUnsetOnly: {
        // CFS / Snapmaker: user edits do NOT propagate to firmware (RFID
        // is hardware-truth). Only fill fields the user hasn't explicitly
        // set, otherwise every status poll would erase the user's choice.
        // The escape hatch is clear_slot_override, which erases the entry
        // and lets auto-mirror take over again.

        if (!ovr.color_set) {
            ovr.color_rgb = firmware_color;
            ovr.color_set = true;
            changed = true;
        }
        if (ovr.material.empty() && !firmware_material.empty()) {
            ovr.material = firmware_material;
            changed = true;
        }
        break;
    }
    }

    if (!changed)
        return false;

    if (store) {
        FilamentSlotOverride snapshot = ovr;
        // Capture log_tag by value — the save callback can fire long after
        // this returns (Moonraker tracker ~60s timeout). Do NOT capture the
        // backend by reference: the backend may outlive its store, but the
        // store will outlive the scheduled save by design.
        const std::string tag_copy = log_tag;
        store->save_async(slot_index, snapshot,
                          [tag_copy, slot_index](bool success, const std::string& err) {
                              if (!success) {
                                  spdlog::warn("{} lane_data auto-mirror failed for slot {}: {}",
                                               tag_copy, slot_index, err);
                              }
                          });
    }
    return true;
}

// =============================================================================
// SlotFingerprintTracker
// =============================================================================

FingerprintEvent SlotFingerprintTracker::observe(int slot_index, const std::string& observed,
                                                 std::string* previous) {
    if (observed.empty())
        return FingerprintEvent::NoSignal;

    // Fires on Baseline/Unchanged only; see set_baseline_sink. Runs inline,
    // under the caller's lock.
    auto confirm_baseline = [this, slot_index, &observed]() {
        if (baseline_sink_)
            baseline_sink_(slot_index, observed);
    };

    auto it = baseline_.find(slot_index);
    if (it == baseline_.end()) {
        baseline_[slot_index] = observed;
        confirm_baseline();
        return FingerprintEvent::Baseline;
    }
    if (it->second == observed) {
        confirm_baseline();
        return FingerprintEvent::Unchanged;
    }

    if (previous)
        *previous = it->second;
    // Advance the baseline BEFORE returning, so a caller whose follow-up
    // action fails (a rejected clear_async, say) doesn't re-fire the same event
    // on every subsequent poll.
    it->second = observed;
    return FingerprintEvent::Changed;
}

std::optional<std::string> SlotFingerprintTracker::baseline(int slot_index) const {
    auto it = baseline_.find(slot_index);
    if (it == baseline_.end())
        return std::nullopt;
    return it->second;
}

void SlotFingerprintTracker::clear() {
    baseline_.clear();
}

void bind_fingerprint_persistence(SlotFingerprintTracker& tracker, FilamentSlotOverrideStore* store,
                                  std::unordered_map<int, FilamentSlotOverride>& overrides) {
    for (const auto& [slot, ovr] : overrides) {
        if (!ovr.fingerprint.empty())
            tracker.seed_baseline(slot, ovr.fingerprint);
    }
    if (!store)
        return;
    tracker.set_baseline_sink([&overrides, store](int slot, const std::string& fingerprint) {
        auto it = overrides.find(slot);
        // No record means nothing for a swap to clear, so a fingerprint under
        // it has no job; an equal one means the record is already current.
        if (it == overrides.end() || it->second.fingerprint == fingerprint)
            return;
        it->second.fingerprint = fingerprint;
        const FilamentSlotOverride snapshot = it->second;
        store->save_async(slot, snapshot, [slot](bool success, const std::string& err) {
            if (!success) {
                spdlog::warn("[FilamentSlotOverrideStore] fingerprint persist for slot {} "
                             "failed: {}",
                             slot, err);
            }
        });
    });
}

bool clear_persisted_override(FilamentSlotOverrideStore* store,
                              std::unordered_map<int, FilamentSlotOverride>& overrides,
                              int slot_index, const std::string& log_tag) {
    auto it = overrides.find(slot_index);
    if (it == overrides.end()) {
        return false;
    }
    overrides.erase(it);
    if (store) {
        store->clear_async(slot_index, [log_tag, slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("{} override clear persist failed for slot {}: {}", log_tag,
                             slot_index, err);
            }
        });
    }
    return true;
}

bool persist_override_external_identity(FilamentSlotOverrideStore* store,
                                        std::unordered_map<int, FilamentSlotOverride>& overrides,
                                        int slot_index, const Observation& spoolman,
                                        const std::string& log_tag) {
    auto it = overrides.find(slot_index);
    if (it == overrides.end()) {
        // A lane nobody edited has no record, and a filing is not an edit.
        return false;
    }
    FilamentSlotOverride& ovr = it->second;
    const int stated = spoolman.spoolman_id.value_or(0);
    if (stated <= 0 || ovr.spoolman_id != stated) {
        // A record naming another spool describes a different spool.
        return false;
    }

    bool changed = false;
    const auto take = [&changed](auto& field, const auto& state) {
        if (state.has_value() && field != *state) {
            field = *state;
            changed = true;
        }
    };
    take(ovr.brand, spoolman.brand);
    take(ovr.material, spoolman.material);
    take(ovr.spool_name, spoolman.spool_name);
    take(ovr.spoolman_vendor_id, spoolman.spoolman_vendor_id);
    take(ovr.spoolman_filament_id, spoolman.spoolman_filament_id);
    // A fetch states the filament whole, so a filament that no longer names an
    // Orca preset takes the record's id away with it.
    take(ovr.spoolman_orca_filament_id,
         std::optional<std::string>(spoolman.spoolman_orca_filament_id.value_or(std::string{})));
    // The colour is kept as the record states it: a lane's colour is what is
    // loaded, which the record or firmware says, not the spool's catalogue
    // colour.
    if (!ovr.color_set && spoolman.color_rgb.has_value()) {
        ovr.color_rgb = *spoolman.color_rgb;
        ovr.color_set = true;
        changed = true;
    }
    if (!changed) {
        return false;
    }
    if (store) {
        save_override_async(store, slot_index, ovr, log_tag, "Spoolman identity");
    }
    return true;
}

void persist_override_weight(FilamentSlotOverrideStore* store,
                             std::unordered_map<int, FilamentSlotOverride>& overrides,
                             int slot_index, float remaining_weight_g, float total_weight_g,
                             const std::string& log_tag) {
    const FilamentSlotOverride& staged =
        stage_weight_override(overrides, slot_index, remaining_weight_g, total_weight_g);
    if (store) {
        store->save_async(slot_index, staged, [log_tag, slot_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("{} weight persist failed for slot {}: {}", log_tag, slot_index, err);
            }
        });
    }
}

FilamentSlotOverride external_lane_record(const SlotInfo& spool) {
    FilamentSlotOverride ovr;
    ovr.external_mirror = true;
    ovr.material = spool.material;
    ovr.brand = spool.brand;
    ovr.spool_name = spool.spool_name;
    ovr.spoolman_id = spool.spoolman_id;
    ovr.spoolman_filament_id = spool.spoolman_filament_id;
    ovr.spoolman_vendor_id = spool.spoolman_vendor_id;
    ovr.spoolman_orca_filament_id = spool.spoolman_orca_filament_id;
    ovr.remaining_weight_g = spool.remaining_weight_g;
    ovr.total_weight_g = spool.total_weight_g;
    ovr.catalog_id = spool.catalog_id;
    ovr.product_name = spool.product_name;
    // User-set data (settings store / Spoolman): default gray = never picked,
    // black = picked.
    ovr.color_rgb = spool.color_rgb;
    ovr.color_set = is_declarable_color(spool.color_rgb);
    populate_temps_from_slot_info(ovr, spool);
    return ovr;
}

SlotInfo external_spool_from_record(const FilamentSlotOverride& record) {
    SlotInfo info;
    info.slot_index = kBypassSlotIndex;
    info.global_index = kBypassSlotIndex;
    info.status = SlotStatus::AVAILABLE;
    info.material = record.material;
    info.brand = record.brand;
    info.spool_name = record.spool_name;
    info.spoolman_id = record.spoolman_id;
    info.spoolman_filament_id = record.spoolman_filament_id;
    info.spoolman_vendor_id = record.spoolman_vendor_id;
    info.spoolman_orca_filament_id = record.spoolman_orca_filament_id;
    info.remaining_weight_g = record.remaining_weight_g;
    info.total_weight_g = record.total_weight_g;
    info.color_name = record.color_name;
    info.catalog_id = record.catalog_id;
    info.product_name = record.product_name;
    if (record.color_set)
        info.color_rgb = record.color_rgb;
    info.nozzle_temp_min = record.nozzle_temp;
    info.nozzle_temp_max = record.nozzle_temp;
    info.bed_temp = record.bed_temp;
    return info;
}

std::optional<std::pair<int, FilamentSlotOverride>>
find_bypass_record(const std::unordered_map<int, LaneDataRecord>& records) {
    for (const auto& [slot, entry] : records) {
        if (entry.record.external_mirror)
            return std::make_pair(slot, entry.record);
    }
    return std::nullopt;
}

void file_bypass_record(const FilamentSlotOverride* record) {
    if (record != nullptr) {
        ingest(kBypassLaneId, record_observation(*record));
    } else {
        drop_lane_source(kBypassLaneId, ObservationSource::Record);
    }
}

void save_override_async(FilamentSlotOverrideStore* store, int slot_index,
                         const FilamentSlotOverride& record, const std::string& tag,
                         const char* noun, LaneId lane) {
    store->save_async(
        slot_index, record, [tag, slot_index, noun, lane](bool success, const std::string& err) {
            if (success) {
                return;
            }
            // The lane falls back to what Moonraker holds; no toast, since the
            // editor is where a failed save belongs.
            spdlog::warn("{} {} write failed for slot {}: {}", tag, noun, slot_index, err);
            if (is_lane_id(lane)) {
                withdraw_pending(lane, err);
            }
        });
}

void persist_staged_override(FilamentSlotOverrideStore* store, std::mutex& mutex,
                             std::unordered_map<int, FilamentSlotOverride>& overrides,
                             int slot_index, const std::string& tag, const char* noun,
                             LaneId lane) {
    FilamentSlotOverride record;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = overrides.find(slot_index);
        if (it == overrides.end()) {
            return;
        }
        record = it->second;
    }
    if (store) {
        save_override_async(store, slot_index, record, tag, noun, lane);
    }
}

} // namespace helix::ams
