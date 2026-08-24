// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_slot_override_store.h"

#include "ams_types.h"
#include "data_root_resolver.h"
#include "filament_database.h"
#include "filament_slot_override.h"
#include "filament_variants.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "moonraker_error.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace helix::ams {

namespace {

// Outer Moonraker DB key for a slot. NOTE the two styles have DIFFERENT bases:
//   Lane -> "lane<i+1>"  (1-based, AFC/Happy Hare convention)
//   Tool -> "T<i>"       (0-based, Orca/Mainsail tool convention)
// Both carry the SAME 0-based inner "lane" field (to_lane_data_record above).
// IMPORTANT: changing the base of one style without the other silently breaks
// interop — the outer key and inner "lane" field are deliberately offset for
// Lane style (1-based key, 0-based field) and aligned for Tool style.
std::string format_lane_key(int slot_index, LaneKeyStyle style) {
    return style == LaneKeyStyle::Tool ? "T" + std::to_string(slot_index)
                                       : "lane" + std::to_string(slot_index + 1);
}

std::string format_iso8601(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

// Read `primary`, falling back to `alias` when primary is absent or JSON null.
//
// Both reads go through safe_string because nlohmann's .value("k", def) THROWS
// type_error.302 when the key is PRESENT with a null value — a shape AFC, Happy
// Hare and Mainsail all emit legitimately into this shared namespace. Nesting
// one .value() as another's default argument (the shape this replaces) is
// strictly worse: arguments are evaluated eagerly, so the inner call throws
// before the outer default can ever be used.
//
// A null primary falls through to the alias, which is what "no value here"
// should mean. A primary present as a non-string yields the alias too.
std::string string_with_alias(const nlohmann::json& j, const char* primary, const char* alias) {
    if (j.contains(primary) && !j[primary].is_null())
        return helix::json_util::safe_string(j, primary);
    return helix::json_util::safe_string(j, alias);
}

std::chrono::system_clock::time_point parse_iso8601(const std::string& s) {
    std::tm tm{};
    std::istringstream is(s);
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (is.fail())
        return {};
#if defined(HELIX_PLATFORM_ESP32)
    // newlib has no timegm(); compute days-since-epoch directly (UTC, no DST).
    const int y = tm.tm_year + 1900, mo = tm.tm_mon + 1;
    const int a = (14 - mo) / 12, yy = y + 4800 - a, mm = mo + 12 * a - 3;
    const long days = tm.tm_mday + (153 * mm + 2) / 5 + 365L * yy + yy / 4 - yy / 100 + yy / 400 -
                      32045 - 2440588;
    const time_t t = days * 86400L + tm.tm_hour * 3600L + tm.tm_min * 60L + tm.tm_sec;
    return std::chrono::system_clock::from_time_t(t);
#else
    return std::chrono::system_clock::from_time_t(timegm(&tm));
#endif
}

// Convert FilamentSlotOverride + slot_index to the AFC-shaped JSON Orca expects,
// plus our extension fields (prefixed comment fields are HelixScreen-only, silently
// ignored by Orca 2.3.2 which only reads the top 5 required fields).
//
// NOTE on indexing: the outer Moonraker DB key style varies (laneN 1-based for
// filament systems, T<n> 0-based for tool changers) but the "lane" field inside
// the record is ALWAYS 0-based (matches Orca's tool-index interpretation). The
// outer key is produced by format_lane_key() above; this function writes the
// 0-based inner field.
nlohmann::json to_lane_data_record(int slot_index, const FilamentSlotOverride& o) {
    nlohmann::json j;
    j["lane"] = std::to_string(slot_index); // REQUIRED by Orca (0-based)
    // Emit color only when the override actually has one set. Pure black
    // (#000000) is a legitimate user choice and a real firmware-detected
    // color (K2 reports loaded black PLA as RGB 0x000000), so the previous
    // `color_rgb != 0` check was wrong — it conflated "no color set" with
    // "color set to black". `color_set` is the explicit signal.
    if (o.color_set) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "#%06X", o.color_rgb & 0x00FFFFFFu);
        j["color"] = buf;
    }
    // Two strings, deliberately. OrcaSlicer reads `material` and matches it by
    // exact string equality against its filament library; a string it cannot
    // match does NOT degrade gracefully — it resolves to a PLA preset
    // (Preset.cpp:3300), which means PLA temps on whatever is really loaded. So
    // `material` carries a conservative, matchable string.
    //
    // `helix_material` carries our precise identity ("ASA-GF"), which Orca
    // ignores. Without it, load_blocking would read back the degraded string
    // and permanently forget what the user actually loaded.
    if (!o.material.empty()) {
        const std::string match = filament::orca_match_type(o.material);
        if (!match.empty())
            j["material"] = match;
        j["helix_material"] = o.material;
    }
    // Catalog product identity. HelixScreen-only (Orca and the AMS plugins have
    // no notion of a branded product id), hence the helix_ prefix in this shared
    // namespace. Emitted independently: a pick whose id later stops resolving
    // still has a name worth keeping, and a name with no id is a legitimate
    // half-record rather than a reason to drop both.
    if (!o.catalog_id.empty())
        j["helix_catalog_id"] = o.catalog_id;
    if (!o.product_name.empty())
        j["helix_product_name"] = o.product_name;
    // helix_locked_* are HelixScreen-internal markers. Always emit both (even
    // when false) so a future re-load can distinguish "explicit auto-mirror,
    // safe to track" from "missing key, fall back to pessimistic default."
    // OrcaSlicer's MoonrakerPrinterAgent ignores unknown fields, so the two
    // extra booleans per slot cost nothing on its side.
    j["helix_locked_color"] = o.user_locked_color;
    j["helix_locked_material"] = o.user_locked_material;
    if (!o.brand.empty()) {
        j["vendor"] = o.brand;      // legacy key, ours
        j["vendor_name"] = o.brand; // the shared lane_data spelling, established by
                                    // Happy Hare and adopted by AFC in #833, so a
                                    // reader of this namespace finds our overrides
                                    // under the one key it already looks for.
                                    // Zero-cost alias (consumers ignore unknown keys).
    }
    if (o.spoolman_id > 0)
        j["spool_id"] = o.spoolman_id;
    if (o.updated_at.time_since_epoch().count() > 0) {
        j["scan_time"] = format_iso8601(o.updated_at);
    }
    // Resolve at emit time — see resolved_temps() for the rule. The local
    // cache (to_json) goes through the same resolver so the two stores never
    // disagree on what an override means.
    auto temps = resolved_temps(o);
    if (temps.bed_temp > 0)
        j["bed_temp"] = temps.bed_temp;
    if (temps.nozzle_temp > 0)
        j["nozzle_temp"] = temps.nozzle_temp;
    if (!o.spool_name.empty()) {
        j["spool_name"] = o.spool_name; // legacy key, ours
        j["name"] = o.spool_name;       // the shared lane_data spelling — same rationale
                                        // as `vendor_name` above
    }
    if (o.spoolman_vendor_id > 0)
        j["spoolman_vendor_id"] = o.spoolman_vendor_id;
    if (o.remaining_weight_g >= 0)
        j["remaining_weight_g"] = o.remaining_weight_g;
    if (o.total_weight_g >= 0)
        j["total_weight_g"] = o.total_weight_g;
    if (!o.color_name.empty())
        j["color_name"] = o.color_name;
    return j;
}

// Update the on-disk cache file with the current state of one slot.
//
// Intentionally a free function, NOT a member — it's called from the success
// lambdas of save_async / clear_async, which must NOT capture `this`. The
// store may be destroyed before Moonraker's callback fires (the whole point
// of the lifetime discipline elsewhere in this file), so the cache write
// must work from value-captured locals only.
//
// Behavior:
// - Reads the existing cache file (if present). Parse failures are logged at
//   warn and treated as "start fresh" — a corrupt cache MUST NOT fail the
//   save call, because the MR DB write already succeeded (source of truth).
// - Ensures doc["version"] == 1 and doc[backend_id]["slots"] is an object.
// - If `ovr` is non-null: writes doc[backend_id]["slots"][slot_index] = to_json(*ovr).
// - If `ovr` is null: erases doc[backend_id]["slots"][slot_index] (clear path).
// - Writes atomically via tmp file + rename. Any IO failure is logged at warn
//   but does NOT propagate to the caller.
//
// Thread-model assumption: Moonraker's libhv EventLoop serializes all callback
// dispatch for a given connection on a single thread, so concurrent R-M-W of
// this cache file across two backends (e.g. IFS + ACE) can't interleave today.
// If that threading model ever changes (per-request dispatch, multi-connection
// fan-out), this read-modify-write becomes racy and needs a file lock.
void write_cache_slot(const std::filesystem::path& cache_path, const std::string& backend_id,
                      int slot_index, const FilamentSlotOverride* ovr) {
    nlohmann::json doc = nlohmann::json::object();
    std::error_code ec;
    if (std::filesystem::exists(cache_path, ec)) {
        std::ifstream in(cache_path);
        if (in) {
            try {
                doc = nlohmann::json::parse(in);
                if (!doc.is_object())
                    doc = nlohmann::json::object();
            } catch (const std::exception& e) {
                spdlog::warn("[FilamentSlotOverrideStore] cache parse failed "
                             "({}), starting fresh: {}",
                             cache_path.string(), e.what());
                doc = nlohmann::json::object();
            }
        }
    }

    doc["version"] = 1;
    if (!doc.contains(backend_id) || !doc[backend_id].is_object()) {
        doc[backend_id] = nlohmann::json::object();
    }
    if (!doc[backend_id].contains("slots") || !doc[backend_id]["slots"].is_object()) {
        doc[backend_id]["slots"] = nlohmann::json::object();
    }

    // The local cache is keyed by the slot_index string ("0", "1", ...),
    // independent of the DB key style (laneN vs T<n>). Key style is invisible
    // here — there is nothing to migrate in the cache when a backend switches
    // styles; only the Moonraker DB carries the outer key convention.
    const std::string key = std::to_string(slot_index);
    if (ovr != nullptr) {
        doc[backend_id]["slots"][key] = to_json(*ovr);
    } else {
        doc[backend_id]["slots"].erase(key);
    }

    // Atomic write: tmp file + rename. POSIX rename is atomic within a fs.
    std::filesystem::path tmp = cache_path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            spdlog::warn("[FilamentSlotOverrideStore] cache write failed: "
                         "cannot open {} for writing",
                         tmp.string());
            return;
        }
        out << doc.dump(2);
        if (!out) {
            spdlog::warn("[FilamentSlotOverrideStore] cache write failed: "
                         "error writing to {}",
                         tmp.string());
            return;
        }
    } // ofstream closed here, buffers flushed, before rename

    std::filesystem::rename(tmp, cache_path, ec);
    if (ec) {
        spdlog::warn("[FilamentSlotOverrideStore] cache rename failed ({} -> {}): {}", tmp.string(),
                     cache_path.string(), ec.message());
        // Best-effort cleanup of the orphan tmp — ignore errors.
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
    }
}

// Read the on-disk cache file and return this backend's slot overrides.
//
// Intentionally a free function (symmetric with write_cache_slot): load_blocking
// calls it synchronously after its MR DB wait, so it takes the cache_path and
// backend_id as value-parameters — no `this` access, matches the write-side
// discipline for future refactors that might move either path off the main
// thread.
//
// This is the OFFLINE-FALLBACK read. load_blocking only calls it when the MR
// DB round-trip didn't yield a value — either because the error callback fired
// (connection/server failure) or the cv.wait_for timeout elapsed. A successful
// MR DB response with an empty namespace is NOT cache-fallback-eligible — that
// response is authoritative ("no overrides configured") and we must not leak
// stale cache data past it.
//
// Behavior:
// - File absent → empty map, no log (normal first-run).
// - File present but parse fails → warn, empty map. Do NOT delete the file —
//   the next successful save will overwrite it atomically via write_cache_slot,
//   and in the meantime keeping it around lets an ops user inspect corruption.
// - doc["version"] != 1 → warn, empty map (forward-compat: a future schema
//   bump should be ignored here so an old build doesn't truncate new data on
//   its first save).
// - doc[backend_id]["slots"] missing → empty map, no log (this backend has
//   never been saved, or another backend owns the file exclusively).
// - Otherwise iterate slots: parse each key as int, skip if non-int or
//   negative (symmetric with from_lane_data_record's rejection rule), call
//   from_json on the value, insert into the result map.
std::unordered_map<int, FilamentSlotOverride> read_cache(const std::filesystem::path& cache_path,
                                                         const std::string& backend_id) {
    std::unordered_map<int, FilamentSlotOverride> result;
    std::error_code ec;
    if (!std::filesystem::exists(cache_path, ec))
        return result;

    std::ifstream in(cache_path);
    if (!in)
        return result;

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(in);
    } catch (const std::exception& e) {
        spdlog::warn("[FilamentSlotOverrideStore] cache parse failed ({}): {}", cache_path.string(),
                     e.what());
        return result;
    }
    if (!doc.is_object()) {
        spdlog::warn("[FilamentSlotOverrideStore] cache top-level is not an object ({})",
                     cache_path.string());
        return result;
    }

    // Forward-compat: silently ignore schemas we don't understand. A newer
    // build may have bumped version; we must not mis-parse its data nor
    // truncate it on our own next save (write_cache_slot re-reads and merges,
    // but only entries keyed under backend_id[slots] — other top-level fields
    // are preserved).
    if (!doc.contains("version") || doc["version"] != 1) {
        spdlog::warn("[FilamentSlotOverrideStore] cache schema version mismatch ({}): {}",
                     cache_path.string(),
                     doc.contains("version") ? doc["version"].dump() : std::string("<missing>"));
        return result;
    }

    if (!doc.contains(backend_id) || !doc[backend_id].is_object())
        return result;
    const auto& backend_entry = doc[backend_id];
    if (!backend_entry.contains("slots") || !backend_entry["slots"].is_object())
        return result;

    const auto& slots = backend_entry["slots"];
    for (auto it = slots.begin(); it != slots.end(); ++it) {
        int slot_index = 0;
        try {
            slot_index = std::stoi(it.key());
        } catch (...) {
            continue;
        }
        // Symmetric with from_lane_data_record / save_async / clear_async:
        // negative slot indices are never valid and must be silently skipped.
        if (slot_index < 0)
            continue;
        if (!it.value().is_object())
            continue;
        // Per-record, not per-file. The try above covers only json::parse, so
        // without this a single unreadable slot would abort the whole cache read
        // and silently discard every other slot the user has configured. from_
        // json is null-tolerant on its own; this is the structural backstop that
        // keeps that property from being a precondition of the caller.
        try {
            result[slot_index] = from_json(it.value());
        } catch (const std::exception& e) {
            spdlog::warn("[FilamentSlotOverrideStore] skipping unreadable cache slot {}: {}",
                         it.key(), e.what());
        }
    }
    return result;
}

// Slot index implied by a HelixScreen-style outer key, or nullopt if the key is
// not one of our shapes. "laneN" is 1-based (slot N-1); "T<n>" is 0-based (slot
// n). Foreign keys (e.g. "gate_0", "metadata") return nullopt and are never
// treated as inconsistent — only a key that LOOKS like ours but disagrees with
// its inner "lane" field is a mismatch.
std::optional<int> implied_slot_from_key(const std::string& key) {
    auto all_digits = [](const std::string& s) {
        return !s.empty() &&
               std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
    };
    if (key.rfind("lane", 0) == 0) {
        const std::string num = key.substr(4);
        if (all_digits(num)) {
            try {
                const int n = std::stoi(num);
                if (n >= 1)
                    return n - 1; // laneN is 1-based; lane0 is not a shape we write
            } catch (...) {
            }
        }
    } else if (!key.empty() && key[0] == 'T') {
        const std::string num = key.substr(1);
        if (all_digits(num)) {
            try {
                return std::stoi(num);
            } catch (...) {
            }
        }
    }
    return std::nullopt;
}

} // namespace

// Parse AFC-shaped record (+ our extensions) back into FilamentSlotOverride.
// Returns (slot_index, override) where slot_index comes from the "lane" field
// (which Orca requires). nullopt if the record is malformed (non-object or
// missing/invalid "lane" field).
//
// Namespace-scope (not anonymous) rather than static: this is the wire-format
// parser and tests need to reach it directly to verify round-tripping without
// going through the full save_async/load_blocking async machinery. Production
// callers in this file keep calling it unqualified.
std::optional<std::pair<int, FilamentSlotOverride>> from_lane_data_record(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("lane"))
        return std::nullopt;
    int slot_index = 0;
    if (j["lane"].is_string()) {
        try {
            slot_index = std::stoi(j["lane"].get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    } else if (j["lane"].is_number_integer()) {
        slot_index = j["lane"].get<int>();
    } else {
        return std::nullopt;
    }
    // Matches OrcaSlicer's MoonrakerPrinterAgent.cpp:796 — negative lane
    // values are never valid slot indices.
    if (slot_index < 0)
        return std::nullopt;

    FilamentSlotOverride o;
    if (j.contains("color") && j["color"].is_string()) {
        std::string s = j["color"].get<std::string>();
        if (!s.empty() && s[0] == '#') {
            s = s.substr(1);
        } else if (s.size() >= 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
            s = s.substr(2);
        }
        try {
            o.color_rgb = static_cast<uint32_t>(std::stoul(s, nullptr, 16));
            o.color_set = true;
        } catch (...) {
            // Leave color_rgb at default + color_set=false on parse failure.
        }
    }
    // Prefer our precise identity; fall back to `material` for foreign records
    // (Mainsail, AFC, Happy Hare) and for records written before helix_material
    // existed.
    o.material = string_with_alias(j, "helix_material", "material");
    // Pessimistic legacy-default: pre-fix records have no helix_locked_* key.
    // Assume the field IS user-locked when it carries a value — protects
    // existing overrides from auto-mirror clobber after upgrade. New records
    // (post-fix) carry the explicit flag and round-trip exactly. See struct
    // doc + #965 for rationale. A null on the wire falls to the same
    // pessimistic default as a missing key, which is the safe direction.
    o.user_locked_color = helix::json_util::safe_bool(j, "helix_locked_color", o.color_set);
    o.user_locked_material =
        helix::json_util::safe_bool(j, "helix_locked_material", !o.material.empty());
    // Prefer our own `vendor` key; fall back to `vendor_name`, the shared
    // lane_data spelling that Happy Hare established (mmu_server
    // push_lane_data) and AFC adopted in AFCProject/AFC-Klipper-Add-On#833, so
    // alias-only foreign records read correctly. Round-trips of our own records
    // stay exact.
    o.brand = string_with_alias(j, "vendor", "vendor_name");
    o.spoolman_id = helix::json_util::safe_int(j, "spool_id", 0);
    o.bed_temp = helix::json_util::safe_int(j, "bed_temp", 0);
    o.nozzle_temp = helix::json_util::safe_int(j, "nozzle_temp", 0);
    if (j.contains("scan_time") && j["scan_time"].is_string()) {
        o.updated_at = parse_iso8601(j["scan_time"].get<std::string>());
    }
    // Same rule as `brand` above: prefer `spool_name`, fall back to the shared
    // `name` alias.
    o.spool_name = string_with_alias(j, "spool_name", "name");
    o.spoolman_vendor_id = helix::json_util::safe_int(j, "spoolman_vendor_id", 0);
    o.remaining_weight_g = helix::json_util::safe_float(j, "remaining_weight_g", -1.0f);
    o.total_weight_g = helix::json_util::safe_float(j, "total_weight_g", -1.0f);
    o.color_name = helix::json_util::safe_string(j, "color_name");
    // No alias fallback and no legacy default: these keys are ours alone, so a
    // record without them simply had no catalog pick. safe_string (not .value())
    // because a hand-edited or third-party record can carry them as JSON null,
    // which .value() would throw type_error.302 on.
    o.catalog_id = helix::json_util::safe_string(j, "helix_catalog_id");
    o.product_name = helix::json_util::safe_string(j, "helix_product_name");
    return std::make_pair(slot_index, o);
}

LaneDataAnomalies scan_lane_data_anomalies(const nlohmann::json& namespace_doc) {
    LaneDataAnomalies a;
    if (!namespace_doc.is_object())
        return a;
    std::unordered_map<int, int> slot_counts; // inner slot idx -> record count
    for (auto it = namespace_doc.begin(); it != namespace_doc.end(); ++it) {
        const std::string& key = it.key();
        if (key == "seated")
            continue; // sibling scalar, never a lane record
        if (!it.value().is_object())
            continue; // other scalar siblings are not our concern
        const auto& v = it.value();
        // A record this scan cannot read is exactly what "unparseable" means, so
        // a throw and a nullopt land in the same bucket. Scoping it per-record
        // keeps a purely diagnostic pass from ever costing the caller data.
        std::optional<std::pair<int, FilamentSlotOverride>> parsed;
        try {
            parsed = from_lane_data_record(v);
        } catch (const std::exception&) {
            ++a.unparseable;
            continue;
        }
        if (!parsed) {
            ++a.unparseable; // object with no valid "lane" field
            continue;
        }
        const int idx = parsed->first;
        ++slot_counts[idx];
        if (v.contains("lane") && v["lane"].is_number_integer())
            ++a.int_typed_lane; // OrcaSlicer's is_string() guard drops these
        const auto implied = implied_slot_from_key(key);
        if (implied && *implied != idx)
            ++a.key_inner_mismatch;
    }
    for (const auto& [idx, n] : slot_counts) {
        (void)idx;
        if (n > 1)
            ++a.duplicate_slot;
    }
    return a;
}

nlohmann::json to_json(const FilamentSlotOverride& o) {
    return {
        {"brand", o.brand},
        {"spool_name", o.spool_name},
        {"spoolman_id", o.spoolman_id},
        {"spoolman_vendor_id", o.spoolman_vendor_id},
        {"remaining_weight_g", o.remaining_weight_g},
        {"total_weight_g", o.total_weight_g},
        {"color_rgb", o.color_rgb},
        {"color_set", o.color_set},
        {"color_name", o.color_name},
        {"material", o.material},
        // Bare names: this file is HelixScreen-private, so there is nothing to
        // disambiguate against (same convention as user_locked_color below,
        // which is helix_locked_color on the shared wire).
        {"catalog_id", o.catalog_id},
        {"product_name", o.product_name},
        {"user_locked_color", o.user_locked_color},
        {"user_locked_material", o.user_locked_material},
        {"bed_temp", o.bed_temp},
        {"nozzle_temp", o.nozzle_temp},
        {"updated_at", format_iso8601(o.updated_at)},
    };
}

FilamentSlotOverride from_json(const nlohmann::json& j) {
    FilamentSlotOverride o;
    o.brand = helix::json_util::safe_string(j, "brand");
    o.spool_name = helix::json_util::safe_string(j, "spool_name");
    o.spoolman_id = helix::json_util::safe_int(j, "spoolman_id", 0);
    o.spoolman_vendor_id = helix::json_util::safe_int(j, "spoolman_vendor_id", 0);
    o.remaining_weight_g = helix::json_util::safe_float(j, "remaining_weight_g", -1.0f);
    o.total_weight_g = helix::json_util::safe_float(j, "total_weight_g", -1.0f);
    // Read color_rgb through the u64 helper rather than safe_int: the field is a
    // full 24-bit RGB value and a hand-edited or third-party record can carry the
    // high byte set, which a narrowing get<int>() would mangle. The helper also
    // accepts a signed integer, which matters because nlohmann only tags a value
    // unsigned when it was built from an unsigned C++ type — a record assembled
    // in memory from an int literal arrives as number_integer and an
    // is_number_unsigned() gate silently drops the color.
    const std::uint64_t color_raw = helix::json_util::safe_uint64(j, "color_rgb", o.color_rgb);
    if (color_raw <= 0xFFFFFFFFu) {
        o.color_rgb = static_cast<std::uint32_t>(color_raw);
    }
    // Pre-fix caches don't have color_set; reconstruct from the old "0 = unset"
    // convention so a returning user's pre-existing overrides keep their
    // intended meaning. New caches always emit the explicit boolean.
    o.color_set = helix::json_util::safe_bool(j, "color_set", o.color_rgb != 0u);
    o.color_name = helix::json_util::safe_string(j, "color_name");
    o.material = helix::json_util::safe_string(j, "material");
    // Absent in a pre-upgrade cache -> empty, which is exactly "no catalog
    // pick". No schema bump: these are purely additive optional fields and
    // read_cache's version gate would discard the whole file if we bumped it
    // (an older build would then truncate a newer cache on its next save).
    o.catalog_id = helix::json_util::safe_string(j, "catalog_id");
    o.product_name = helix::json_util::safe_string(j, "product_name");
    // Pessimistic legacy-default — see from_lane_data_record + #965.
    o.user_locked_color = helix::json_util::safe_bool(j, "user_locked_color", o.color_set);
    o.user_locked_material =
        helix::json_util::safe_bool(j, "user_locked_material", !o.material.empty());
    o.bed_temp = helix::json_util::safe_int(j, "bed_temp", 0);
    o.nozzle_temp = helix::json_util::safe_int(j, "nozzle_temp", 0);
    if (j.contains("updated_at") && j["updated_at"].is_string()) {
        o.updated_at = parse_iso8601(j["updated_at"].get<std::string>());
    }
    return o;
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

// ============================================================================
// FilamentSlotOverrideStore skeleton (Task 2). Real load/save wiring lands
// in Tasks 3-5; this skeleton exists so other components can depend on the
// class shape now.
// ============================================================================

FilamentSlotOverrideStore::FilamentSlotOverrideStore(IMoonrakerAPI* api, std::string backend_id,
                                                     LaneKeyStyle key_style, std::string ns)
    : api_(api), backend_id_(std::move(backend_id)), key_style_(key_style),
      namespace_(std::move(ns)) {}

std::filesystem::path FilamentSlotOverrideStore::cache_dir_effective() const {
    if (!cache_dir_.empty()) {
        return cache_dir_;
    }
    // Test-binary seam: the shared fixture points the process-wide fallback
    // at its sandbox before main() (helix_test_fixture.cpp). Empty in
    // production, where the user config dir below remains the answer.
    const std::filesystem::path& process_default = detail::slot_override_cache_dir_ref();
    if (!process_default.empty()) {
        return process_default;
    }
    return std::filesystem::path(helix::get_user_config_dir());
}

std::filesystem::path FilamentSlotOverrideStore::cache_path() const {
    return cache_dir_effective() / "filament_slot_overrides.json";
}

namespace {

// Synchronous legacy-migration helper.
//
// Before Task 8, ACE and CFS backends stored per-slot overrides at
// "helix-screen:{backend_id}_slot_overrides" as a single JSON object keyed by
// slot-index string ("0", "1", ...). We now use the AFC-shaped lane_data
// namespace (lane1, lane2, ...). On first startup after upgrade we translate
// the old data forward so users don't lose their overrides silently.
//
// Contract:
// - Returns the migrated slot map (empty if nothing to migrate or migration
//   failed).
// - Only runs for ACE/CFS — no other backend ever wrote the legacy namespace,
//   and IFS/Snapmaker must NOT touch a spurious helix-screen:ifs_slot_overrides
//   entry that somehow existed (e.g. hand-seeded during testing).
// - Idempotent: once lane_data has data, callers skip migration entirely
//   (the caller site guards on lane_data being empty before invoking us).
// - Migration requires MR DB to be reachable — we need to READ legacy,
//   WRITE lane_data, and DELETE legacy. The caller (load_blocking) only
//   reaches this path when the lane_data round-trip itself succeeded
//   (got_copy == true, received empty), which proves the printer is up.
// - Safe against caller destruction: this function runs synchronously on the
//   caller's thread via shared_ptr<SyncState> waits, so it completes before
//   load_blocking returns — no orphaned lambdas to worry about.
// - Write failures abort migration WITHOUT deleting the legacy data so the
//   next startup can retry. We DO NOT attempt partial migration (writing 2
//   of 4 slots, deleting legacy) because mixing lane_data entries with a
//   still-live legacy blob would make subsequent reconciliation ambiguous.
//
// NOT captured in this helper (deliberately):
// - Calling code's `this`. Only value types pass through lambdas.
// - Any retry / exponential backoff. A transient network blip returns {}, the
//   user sees no overrides until next app start, and the legacy data is still
//   there for the next attempt. That's correct conservative behavior.
std::unordered_map<int, FilamentSlotOverride>
try_migrate_legacy(IMoonrakerAPI* api, const std::string& backend_id,
                   std::chrono::milliseconds timeout, const std::filesystem::path& cache_dir,
                   LaneKeyStyle key_style) {
    std::unordered_map<int, FilamentSlotOverride> empty_result;
    if (!api)
        return empty_result;
    if (backend_id != "ace" && backend_id != "cfs")
        return empty_result;

    const std::string legacy_ns = "helix-screen";
    const std::string legacy_key = backend_id + "_slot_overrides";

    struct SyncState {
        std::mutex m;
        std::condition_variable cv;
        bool done{false};
        bool got{false};
        nlohmann::json received;
    };
    auto get_state = std::make_shared<SyncState>();

    api->database_get_item(
        legacy_ns, legacy_key,
        [get_state](const nlohmann::json& value) {
            std::lock_guard<std::mutex> lk(get_state->m);
            get_state->received = value;
            get_state->got = true;
            get_state->done = true;
            get_state->cv.notify_one();
        },
        [get_state](const MoonrakerError&) {
            // Legacy namespace absent is the common case — no legacy data to
            // migrate, but it's not an error. Silently proceed with empty.
            std::lock_guard<std::mutex> lk(get_state->m);
            get_state->done = true;
            get_state->cv.notify_one();
        });

    nlohmann::json legacy_doc;
    bool legacy_got = false;
    {
        std::unique_lock<std::mutex> lk(get_state->m);
        // Keep the wait bounded — migration runs only when we already proved
        // MR DB is reachable via the prior lane_data fetch, so the caller's
        // load_timeout_ is a generous upper bound for a single
        // database_get_item round-trip.
        get_state->cv.wait_for(lk, timeout, [get_state] { return get_state->done; });
        legacy_got = get_state->got;
        if (legacy_got)
            legacy_doc = get_state->received;
    }

    if (!legacy_got)
        return empty_result;
    if (!legacy_doc.is_object() || legacy_doc.empty())
        return empty_result;

    // Build the migrated slot map. Legacy field names happen to match our
    // FilamentSlotOverride members 1:1 — same as from_json, with the single
    // wrinkle that legacy had no updated_at stamp. Use now() so future
    // conflict-avoidance logic can distinguish "just migrated" from "ancient".
    //
    // Track legacy_entries_seen so we can distinguish "legacy blob was
    // truly empty" (nothing to do) from "legacy had entries but every one
    // was unsalvageable" (delete anyway so we don't re-scan every startup).
    std::unordered_map<int, FilamentSlotOverride> migrated;
    int legacy_entries_seen = 0;
    for (auto it = legacy_doc.begin(); it != legacy_doc.end(); ++it) {
        ++legacy_entries_seen;
        int slot_index = 0;
        try {
            slot_index = std::stoi(it.key());
        } catch (...) {
            continue; // non-int keys silently skipped (matches cache reader)
        }
        if (slot_index < 0)
            continue;
        if (!it.value().is_object())
            continue; // malformed entry → skip

        // Same per-record scoping as read_cache: an unreadable legacy entry is
        // one lost slot, never an aborted migration. Skipping here still counts
        // it in legacy_entries_seen, so an all-unreadable blob takes the
        // "dropped N malformed legacy entries" path below and gets cleaned up
        // rather than re-scanned on every subsequent startup.
        try {
            FilamentSlotOverride o = from_json(it.value());
            o.updated_at = std::chrono::system_clock::now();
            migrated[slot_index] = o;
        } catch (const std::exception& e) {
            spdlog::warn("[FilamentSlotOverrideStore:{}] skipping unreadable legacy entry {}: {}",
                         backend_id, it.key(), e.what());
        }
    }

    // All-malformed case: legacy had entries but none survived parsing. We
    // must still delete the legacy blob — otherwise every subsequent startup
    // re-fetches the same unsalvageable data, hits this code path, and bails.
    // The cleanup is the whole point of a one-shot migration. Log at info so
    // the drop is auditable, with the count for ops to correlate.
    if (migrated.empty()) {
        if (legacy_entries_seen > 0) {
            spdlog::info("[FilamentSlotOverrideStore:{}] dropped {} malformed legacy "
                         "entries from helix-screen:{}_slot_overrides",
                         backend_id, legacy_entries_seen, backend_id);
            api->database_delete_item(legacy_ns, legacy_key, nullptr,
                                      [backend_id, legacy_key](const MoonrakerError& err) {
                                          spdlog::warn(
                                              "[FilamentSlotOverrideStore:{}] failed to delete "
                                              "all-malformed legacy helix-screen:{}: {} "
                                              "(non-fatal, will retry on next startup)",
                                              backend_id, legacy_key, err.message);
                                      });
            if (!cache_dir.empty()) {
                std::error_code rm_ec;
                std::filesystem::remove(cache_dir / (backend_id + "_slot_overrides.json"), rm_ec);
            }
        }
        return empty_result;
    }

    // Post each migrated slot to lane_data synchronously. If ANY write fails,
    // abort WITHOUT deleting legacy — the next startup retries cleanly. We
    // don't try to roll back partial writes: a retry will overwrite them with
    // the same values (idempotent), and leaving the legacy blob ensures the
    // retry path has full data.
    for (const auto& [slot_index, override_val] : migrated) {
        auto post_state = std::make_shared<SyncState>();
        // ACE/CFS are Lane style, so this resolves to "laneN"; threading the
        // style through format_lane_key keeps the one key-formatting rule in one
        // place (and closes the trap of a future toolchanger reaching this path
        // with the wrong keys).
        const std::string lane_data_key = format_lane_key(slot_index, key_style);
        api->database_post_item(
            "lane_data", lane_data_key, to_lane_data_record(slot_index, override_val),
            [post_state]() {
                std::lock_guard<std::mutex> lk(post_state->m);
                post_state->got = true;
                post_state->done = true;
                post_state->cv.notify_one();
            },
            [post_state](const MoonrakerError&) {
                std::lock_guard<std::mutex> lk(post_state->m);
                post_state->done = true;
                post_state->cv.notify_one();
            });

        bool write_ok = false;
        {
            std::unique_lock<std::mutex> lk(post_state->m);
            post_state->cv.wait_for(lk, timeout, [post_state] { return post_state->done; });
            write_ok = post_state->got;
        }
        if (!write_ok) {
            spdlog::warn("[FilamentSlotOverrideStore:{}] legacy migration aborted: "
                         "failed to write {} to lane_data (legacy preserved for retry)",
                         backend_id, lane_data_key);
            return empty_result;
        }
    }

    // All writes succeeded. Fire-and-forget delete of the legacy namespace —
    // failure to delete is logged but does NOT break the migrated result.
    // The idempotence guard (lane_data non-empty on next startup) means a
    // lingering legacy blob simply sits there until the user clears it.
    // Capture legacy_key by value (the lambda outlives this stack frame in
    // case the error callback fires after the outer function returns).
    api->database_delete_item(
        legacy_ns, legacy_key, nullptr, [backend_id, legacy_key](const MoonrakerError& err) {
            spdlog::warn("[FilamentSlotOverrideStore:{}] failed to delete legacy "
                         "helix-screen:{}: {} (non-fatal, migration complete)",
                         backend_id, legacy_key, err.message);
        });

    // Clean up the pre-Task-6 per-backend JSON file if it's still around.
    // Task 6 unified all backends under filament_slot_overrides.json, but an
    // upgrading user may have a lingering ace_slot_overrides.json or
    // cfs_slot_overrides.json from an older build. Nothing reads it anymore,
    // but leaving it behind is confusing when users inspect their config dir.
    // Best-effort: swallow IO errors (not fatal to the migration result).
    if (!cache_dir.empty()) {
        std::error_code rm_ec;
        std::filesystem::remove(cache_dir / (backend_id + "_slot_overrides.json"), rm_ec);
    }

    spdlog::info("[FilamentSlotOverrideStore:{}] migrated {} slot(s) from "
                 "helix-screen:{}_slot_overrides to lane_data",
                 backend_id, migrated.size(), backend_id);

    return migrated;
}

// Synchronous key-migration helper: rewrite our own stale "laneN" records to
// the "T<n>" tool-key style. Called from load_blocking AFTER the parse loop,
// gated on key_style_ == Tool.
//
// Unlike try_migrate_legacy this runs on a NON-EMPTY lane_data — that is
// precisely when there is something to migrate. It reuses the already-fetched
// namespace_doc (no extra GET); load_blocking only reaches here with
// got_copy == true, so MR DB reachability is already proven, and the offline
// cache-fallback branch returns before this point so a network blip never
// attempts destructive key moves.
//
// Self-idempotent via the delete: once the stale laneN keys are gone, the next
// boot sees only T<n> and this no-ops at the classify step. Classification:
//   - key != format_lane_key(idx, Lane) -> not a key WE wrote, skip. Renaming a
//     third party's arbitrary key would vandalize a shared namespace.
//   - format_lane_key(idx, Tool) already present -> DROP the stale laneN only.
//     Never POST laneN's body over T<n>: T<n> may be Mainsail's newer record,
//     so overwriting it would regress data. The reader already picked T<n> as
//     canonical for this slot, so the drop makes the DB agree with what we
//     returned — migration is consistent with the reader by construction.
//   - else -> MOVE: write T<n>, then delete laneN.
//
// Writes go in ascending slot order (test determinism), all-or-nothing: on ANY
// write failure, warn and return WITHOUT deleting anything (including to_drop),
// so an aborted migration stays atomic and trivially reasoned about.
//
// Lifetime: runs synchronously via shared_ptr<SyncState> + cv.wait_for, so it
// completes before load_blocking returns. Delete callbacks capture backend_id
// and key BY VALUE, never `this` (matches try_migrate_legacy).
void try_migrate_lane_keys_to_tool_keys(IMoonrakerAPI* api, const std::string& backend_id,
                                        const nlohmann::json& namespace_doc,
                                        std::chrono::milliseconds timeout) {
    if (!api || !namespace_doc.is_object())
        return;

    // Step 1 — classify. to_move is ordered (std::map) so writes go in ascending
    // slot order; to_drop holds stale laneN keys whose canonical T<n> exists.
    std::map<int, std::string> to_move; // slot idx -> laneN key
    std::vector<std::string> to_drop;   // laneN keys to delete without rewriting
    for (auto it = namespace_doc.begin(); it != namespace_doc.end(); ++it) {
        const std::string& key = it.key();
        if (key == "seated")
            continue;
        if (!it.value().is_object())
            continue;
        // A record we cannot read is a record we cannot prove we authored, so
        // skipping it is the same conservative answer as the key check below.
        // Scoped per-record so an unreadable third-party entry can't abort the
        // migration — or, worse, unwind into load_blocking's boundary and throw
        // away the overrides that were already parsed successfully.
        std::optional<std::pair<int, FilamentSlotOverride>> parsed;
        try {
            parsed = from_lane_data_record(it.value());
        } catch (const std::exception& e) {
            spdlog::warn("[FilamentSlotOverrideStore:{}] key migration skipping unreadable "
                         "record {}: {}",
                         backend_id, key, e.what());
            continue;
        }
        if (!parsed)
            continue;
        const int idx = parsed->first;
        if (key != format_lane_key(idx, LaneKeyStyle::Lane))
            continue; // not a key we wrote — leave it alone
        if (namespace_doc.contains(format_lane_key(idx, LaneKeyStyle::Tool)))
            to_drop.push_back(key); // canonical already present → drop the stale laneN
        else
            to_move[idx] = key;
    }

    // Step 2 — idempotence guard: nothing of ours to migrate.
    if (to_move.empty() && to_drop.empty())
        return;

    // Step 3 — write T<n> for every move, ascending slot order, all-or-nothing.
    struct SyncState {
        std::mutex m;
        std::condition_variable cv;
        bool done{false};
        bool got{false};
    };
    for (const auto& [idx, lane_data_key] : to_move) {
        // Copy the RAW record — preserving unmodeled third-party fields (Happy
        // Hare's filament_id, AFC extras) that round-tripping through
        // FilamentSlotOverride would silently drop. Normalize only the inner
        // "lane" to a string so the written record satisfies Orca's is_string()
        // contract even if the source had an integer lane.
        nlohmann::json moved = namespace_doc[lane_data_key];
        moved["lane"] = std::to_string(idx);
        const std::string tool_key = format_lane_key(idx, LaneKeyStyle::Tool);

        auto post_state = std::make_shared<SyncState>();
        api->database_post_item(
            "lane_data", tool_key, moved,
            [post_state]() {
                std::lock_guard<std::mutex> lk(post_state->m);
                post_state->got = true;
                post_state->done = true;
                post_state->cv.notify_one();
            },
            [post_state](const MoonrakerError&) {
                std::lock_guard<std::mutex> lk(post_state->m);
                post_state->done = true;
                post_state->cv.notify_one();
            });

        bool write_ok = false;
        {
            std::unique_lock<std::mutex> lk(post_state->m);
            post_state->cv.wait_for(lk, timeout, [post_state] { return post_state->done; });
            write_ok = post_state->got;
        }
        if (!write_ok) {
            spdlog::warn("[FilamentSlotOverrideStore:{}] key migration aborted: failed to write "
                         "{} to lane_data (laneN preserved for retry)",
                         backend_id, tool_key);
            return; // abort WITHOUT deleting anything (incl. to_drop) — stays atomic
        }
    }

    // Step 4 — all writes succeeded; destination reachability is proven. Delete
    // every stale laneN (moved + dropped). Fire-and-forget: a silently-failed
    // delete just re-migrates next boot (self-idempotent). Capture backend_id
    // and the key BY VALUE, never `this`.
    for (const auto& [idx, lane_data_key] : to_move) {
        (void)idx;
        const std::string deleted_key = lane_data_key;
        api->database_delete_item(
            "lane_data", deleted_key, nullptr,
            [backend_id, deleted_key](const MoonrakerError& err) {
                spdlog::warn("[FilamentSlotOverrideStore:{}] failed to delete migrated "
                             "lane_data:{}: {} (non-fatal, re-migrates next boot)",
                             backend_id, deleted_key, err.message);
            });
    }
    for (const auto& deleted_key : to_drop) {
        api->database_delete_item("lane_data", deleted_key, nullptr,
                                  [backend_id, deleted_key](const MoonrakerError& err) {
                                      spdlog::warn(
                                          "[FilamentSlotOverrideStore:{}] failed to delete stale "
                                          "lane_data:{}: {} (non-fatal, re-migrates next boot)",
                                          backend_id, deleted_key, err.message);
                                  });
    }

    spdlog::info("[FilamentSlotOverrideStore:{}] migrated {} lane_data key(s) to T<n> style "
                 "({} moved, {} dropped)",
                 backend_id, to_move.size() + to_drop.size(), to_move.size(), to_drop.size());
}

} // namespace

// Exception boundary for AMS initialization.
//
// lane_data is a SHARED Moonraker namespace: AFC, Happy Hare, Mainsail and the
// user's own hand edits all write records into it, and every one of them can
// produce a field that is present-but-null. nlohmann throws type_error.302 on
// such a field (it does NOT throw for a missing key), and none of the four AMS
// backends that call this wraps the call — so before this boundary existed, one
// null "material" written by somebody else's plugin unwound the entire backend
// init instead of costing a single slot.
//
// The per-record handlers inside load_blocking_impl scope the common cases to
// one lost slot. This catch is the backstop for everything else: worst case the
// user sees no overrides, which is the fresh-install state and fully
// recoverable, rather than an AMS subsystem that failed to come up.
std::unordered_map<int, FilamentSlotOverride> FilamentSlotOverrideStore::load_blocking() {
    try {
        return load_blocking_impl();
    } catch (const std::exception& e) {
        spdlog::warn("[FilamentSlotOverrideStore:{}] load aborted ({}); continuing with no "
                     "overrides",
                     backend_id_, e.what());
        return {};
    }
}

std::unordered_map<int, FilamentSlotOverride> FilamentSlotOverrideStore::load_blocking_impl() {
    std::unordered_map<int, FilamentSlotOverride> result;
    if (!api_)
        return result;

    // Wrap sync state in shared_ptr so callbacks firing after our local
    // cv.wait_for timeout (load_timeout_, default 5s) don't touch a freed
    // stack frame. Moonraker's request tracker has its own ~60s boundary,
    // so an error callback can fire well after we've already returned.
    // Captured by value, the shared_ptr keeps the state alive for the
    // orphaned callback to flip done/got harmlessly.
    // (Same pattern as AmsBackendAce::poll_info in src/printer/ams_backend_ace.cpp)
    struct SyncState {
        std::mutex m;
        std::condition_variable cv;
        bool done{false};
        bool got{false};
        nlohmann::json received;
    };
    auto state = std::make_shared<SyncState>();
    // Copy strings into the error lambda: the store may be destroyed before
    // Moonraker's request tracker fires its ~60s error timeout, so the lambda
    // can't rely on backend_id_/namespace_ still being alive.
    const std::string backend_id_copy = backend_id_;
    const std::string namespace_copy = namespace_;

    api_->database_get_namespace(
        namespace_,
        [state](const nlohmann::json& value) {
            std::lock_guard<std::mutex> lk(state->m);
            state->received = value;
            state->got = true;
            state->done = true;
            state->cv.notify_one();
        },
        [state, backend_id_copy, namespace_copy](const MoonrakerError& err) {
            spdlog::debug("[FilamentSlotOverrideStore:{}] database_get_namespace({}) failed: {}",
                          backend_id_copy, namespace_copy, err.message);
            std::lock_guard<std::mutex> lk(state->m);
            state->done = true;
            state->cv.notify_one();
        });

    // Snapshot the shared state under lock: after wait_for returns, a callback
    // firing from a background thread could still write to state->got /
    // state->received concurrently with our reads. Take the lock, copy what we
    // need, release. The shared_ptr keeps the state alive for any late
    // callback — it just writes to the copy-source without racing us.
    bool got_copy;
    bool done_copy;
    nlohmann::json received_copy;
    {
        std::unique_lock<std::mutex> lk(state->m);
        state->cv.wait_for(lk, load_timeout_, [state] { return state->done; });
        got_copy = state->got;
        done_copy = state->done;
        if (got_copy)
            received_copy = state->received;
    }

    spdlog::debug("[FilamentSlotOverrideStore:{}] load_blocking: got={} done={} "
                  "received_type={} received_size={}",
                  backend_id_, got_copy, done_copy,
                  got_copy ? (received_copy.is_object()  ? "object"
                              : received_copy.is_null()  ? "null"
                              : received_copy.is_array() ? "array"
                                                         : "other")
                           : "n/a",
                  got_copy && received_copy.is_object() ? received_copy.size() : 0u);

    // Fall back to local cache ONLY when the MR DB round-trip didn't yield a
    // value — either because the error callback fired (got==false, done==true)
    // or the cv.wait_for timeout elapsed (got==false, done==false). A
    // successful MR DB response with an empty namespace is authoritative ("no
    // overrides configured") and must NOT be replaced by stale cache data.
    if (!got_copy) {
        auto cached = read_cache(cache_path(), backend_id_);
        spdlog::debug("[FilamentSlotOverrideStore:{}] load_blocking: cache fallback "
                      "returned {} entries",
                      backend_id_, cached.size());
        return cached;
    }
    if (!received_copy.is_object())
        return result;

    // One-shot diagnostic: surface lane_data records that are inconsistent or
    // invisible to other readers (OrcaSlicer drops int-typed lanes; key/inner
    // mismatches and residual duplicates confuse interop). Read-only — we do NOT
    // rewrite third-party/corrupt records here; the migration below only touches
    // keys we authored. This just turns silent field weirdness into one log line.
    if (const auto anomalies = scan_lane_data_anomalies(received_copy); anomalies.total() > 0) {
        spdlog::info("[FilamentSlotOverrideStore:{}] lane_data has {} anomalous record(s): "
                     "{} int-typed lane, {} key/inner mismatch, {} unparseable, {} duplicate slot",
                     backend_id_, anomalies.total(), anomalies.int_typed_lane,
                     anomalies.key_inner_mismatch, anomalies.unparseable, anomalies.duplicate_slot);
    }

    // Key-agnostic ingest with canonical-key duplicate reconciliation. The
    // outer key is NOT a filter anymore — Mainsail writes "T0", AFC "lane1",
    // Happy Hare its own — so we ingest any well-formed lane record and let
    // from_lane_data_record adjudicate. The inner "lane" field is now the only
    // gate.
    //
    // On duplicate slots (two writers under two keys) prefer the record whose
    // key is canonical for OUR key style: first canonical wins, a canonical
    // always beats a non-canonical, otherwise the incumbent stays. This is
    // order-independent (no reliance on nlohmann's byte-sorted iteration),
    // converges (load->save->load is a fixed point since save writes the
    // canonical key), and agrees with Orca's alphabetical first-wins in every
    // case that can occur. warn, not debug: two writers disagreeing about one
    // slot is an ops signal.
    std::unordered_map<int, bool> canonical_seen; // slot idx -> incumbent was canonical
    for (auto it = received_copy.begin(); it != received_copy.end(); ++it) {
        const std::string& key = it.key();
        // "seated" is a known sibling scalar (the 0-based seated-lane index),
        // never a lane record — named-skip so the intent survives a future
        // where it grows into an object.
        if (key == "seated")
            continue;
        // Non-objects are namespace siblings, not malformed records — a separate
        // debug line so genuine parse failures below stay distinguishable.
        if (!it.value().is_object()) {
            spdlog::debug("[FilamentSlotOverrideStore:{}] skipping non-object key: {}", backend_id_,
                          key);
            continue;
        }
        // Per-record, not per-namespace: a record we cannot read costs that one
        // slot. Anything wider would let a co-author's malformed entry erase
        // every override the user has on the printer. warn (not debug) because
        // unlike the nullopt case below this means the record threw, which is
        // worth surfacing even though we recover from it.
        std::optional<std::pair<int, FilamentSlotOverride>> parsed;
        try {
            parsed = from_lane_data_record(it.value());
        } catch (const std::exception& e) {
            spdlog::warn("[FilamentSlotOverrideStore:{}] skipping unreadable lane_data record "
                         "{}: {}",
                         backend_id_, key, e.what());
            continue;
        }
        if (!parsed) {
            spdlog::debug("[FilamentSlotOverrideStore:{}] from_lane_data_record failed for {}",
                          backend_id_, key);
            continue;
        }
        const int idx = parsed->first;
        const bool canonical = (key == format_lane_key(idx, key_style_));
        auto seen = canonical_seen.find(idx);
        if (seen == canonical_seen.end()) {
            result[idx] = std::move(parsed->second);
            canonical_seen[idx] = canonical;
        } else if (canonical && !seen->second) {
            spdlog::warn("[FilamentSlotOverrideStore:{}] duplicate lane_data records for slot {}; "
                         "preferring canonical key {}",
                         backend_id_, idx, key);
            result[idx] = std::move(parsed->second);
            seen->second = true;
        } else {
            spdlog::warn("[FilamentSlotOverrideStore:{}] duplicate lane_data record for slot {} "
                         "under key {}; keeping the incumbent",
                         backend_id_, idx, key);
        }
    }

    // One-shot heal: records written before orca_match_type existed (or whose
    // match has drifted since — see below) carry a `material` OrcaSlicer
    // cannot match, which it silently resolves to a PLA preset — PLA
    // temperatures on whatever is really loaded. Rewrite them so the fix
    // reaches existing installs without requiring the user to re-edit every
    // slot.
    //
    // ONLY records we authored, proven by a helix_locked_* key. AFC, Happy
    // Hare and Mainsail share this namespace and their records are not ours to
    // rewrite — same rule scan_lane_data_anomalies follows (see this file's
    // header comment on LaneDataAnomalies).
    //
    // Deliberately NOT gated on "already has helix_material". The trigger is
    // "the record resolves to a different match string than it carries" —
    // that also covers drift: a later Orca-table regeneration can drop a type
    // we previously matched, so a record healed on an earlier boot needs to be
    // healable again. Self-limiting instead via `orca_match_type(current) ==
    // current`, which terminates because every override target is itself
    // asserted to be in orca_library_types (see the Python-side idempotency
    // test) — so a record this loop just wrote is never re-written on the
    // next boot. The one case that check doesn't cover on its own — a record
    // whose `material` was omitted because nothing was safely matchable — is
    // covered by the `current.empty()` skip just below.
    //
    // Gated on orca_tables_available(): a missing or pre-change
    // assets/filaments.json makes orca_match_type() return "" for EVERY
    // input, which would make the gate below ("current == match") false for
    // every helix-authored record and strip `material` from all of them in
    // one pass. Skip the whole pass rather than heal against an empty table.
    if (!filament::orca_tables_available()) {
        spdlog::warn("[FilamentSlotOverrideStore:{}] Orca tables unavailable; skipping "
                     "lane_data heal",
                     backend_id_);
    } else {
        for (auto it = received_copy.begin(); it != received_copy.end(); ++it) {
            const std::string& key = it.key();
            if (key == "seated" || !it.value().is_object())
                continue;
            // Non-const: healed below via direct mutation, not a copy — see
            // the in-place-mutation comment further down.
            auto& rec = it.value();
            const bool ours =
                rec.contains("helix_locked_color") || rec.contains("helix_locked_material");
            if (!ours)
                continue; // not ours to rewrite
            // safe_string, not .value(): a record whose `material` was written
            // as JSON null is exactly the shape a co-author or a hand edit
            // produces, and .value() throws type_error.302 on it. Null reads as
            // "no material recorded", which correctly falls through the
            // empty-skip below — there is nothing to heal on such a record.
            const std::string current = helix::json_util::safe_string(rec, "material");
            if (current.empty())
                continue;
            if (filament::orca_match_type(current) == current)
                continue; // already the canonical matchable string — nothing to heal
            // Per-record scope: the heal is opportunistic maintenance, so a
            // record it cannot read is skipped rather than allowed to abort the
            // pass over the records that follow it.
            std::optional<std::pair<int, FilamentSlotOverride>> parsed;
            try {
                parsed = from_lane_data_record(rec);
            } catch (const std::exception& e) {
                spdlog::warn("[FilamentSlotOverrideStore:{}] skipping heal of unreadable "
                             "record {}: {}",
                             backend_id_, key, e.what());
                continue;
            }
            if (!parsed)
                continue;
            // `identity` prefers helix_material, falling back to material,
            // exactly like from_lane_data_record — so a record already healed
            // once (which now drifted) is re-keyed off our own prior identity,
            // not the possibly-stale `material` string.
            const std::string identity = parsed->second.material;
            const std::string matched = filament::orca_match_type(identity);

            spdlog::info(
                "[FilamentSlotOverrideStore:{}] healing lane_data {}: material '{}' is not "
                "Orca-matchable, rewriting with helix_material",
                backend_id_, key, current);

            // Heal by mutating `rec` in place — a reference into
            // received_copy itself — rather than regenerating the record via
            // to_lane_data_record/save_async. Two things fall out of this:
            //   1. scan_time and any field this store doesn't model (a
            //      foreign co-author's key) survive untouched, instead of
            //      being dropped by a full round-trip through
            //      FilamentSlotOverride.
            //   2. received_copy is what the Tool-key migration below reads.
            //      Mutating it here means a stale laneN record is healed
            //      BEFORE the migration moves it to T<n>, so the migration's
            //      write carries the healed body — no unordered double-write
            //      to T<n> between this heal and that migration (H2).
            rec["helix_material"] = identity;
            if (matched.empty())
                rec.erase("material");
            else
                rec["material"] = matched;

            // POST to the SAME key we read (not the key_style_-derived key
            // save_async would compute) so a stale laneN record is healed at
            // laneN, leaving the Tool-key migration below to move the
            // already-healed body — this store never writes T<n> directly
            // for a record it read under laneN.
            const std::string backend_id_copy = backend_id_;
            api_->database_post_item(
                namespace_, key, rec, []() {},
                [backend_id_copy, key](const MoonrakerError& err) {
                    spdlog::warn("[FilamentSlotOverrideStore:{}] heal failed for {}: {}",
                                 backend_id_copy, key, err.message);
                });
        }
    }

    // Tool-changer backends converge on Mainsail's T<n> key. If we (or an older
    // HelixScreen build) previously wrote laneN records, rewrite them to T<n>
    // now — the DB round-trip just proved reachability (got_copy == true). This
    // runs on a NON-EMPTY lane_data, before the legacy block below (they are
    // mutually exclusive: legacy is ace/cfs-only and result-empty-gated, this is
    // Tool-only). The returned `result` already reflects the canonical choice,
    // so the migration only reconciles the DB with what we return.
    if (key_style_ == LaneKeyStyle::Tool)
        try_migrate_lane_keys_to_tool_keys(api_, backend_id_, received_copy, load_timeout_);

    // lane_data returned empty-but-reachable — the MR DB is authoritative
    // ("no overrides configured"). Before accepting that verdict, give the
    // one-shot legacy migration a chance: ACE and CFS backends pre-Task-8
    // stored data at helix-screen:{backend_id}_slot_overrides. On first
    // startup after upgrade we copy it forward into lane_data. The migration
    // helper skips itself for IFS/Snapmaker (no legacy namespace ever
    // existed for them) and for backends where lane_data already has data
    // (guarded here by the !result.empty() early return above).
    //
    // Why here and not elsewhere: migration needs MR DB reachability (READ
    // legacy, WRITE lane_data, DELETE legacy). got_copy==true proves the
    // round-trip succeeded, so this is the right moment. In the offline
    // fallback branch above we explicitly do NOT migrate — a transient
    // network blip should not attempt destructive namespace moves.
    if (result.empty() && (backend_id_ == "ace" || backend_id_ == "cfs")) {
        auto migrated =
            try_migrate_legacy(api_, backend_id_, load_timeout_, cache_dir_effective(), key_style_);
        if (!migrated.empty())
            return migrated;
    }
    return result;
}

void FilamentSlotOverrideStore::save_async(int slot_index, const FilamentSlotOverride& ovr,
                                           SaveCallback cb) {
    if (!api_) {
        if (cb)
            cb(false, "no API");
        return;
    }
    // Reject negative slot indices symmetrically with from_lane_data_record's
    // rejection on load (matches OrcaSlicer's MoonrakerPrinterAgent.cpp:796).
    if (slot_index < 0) {
        if (cb)
            cb(false, "invalid slot_index");
        return;
    }

    // Stamp a fresh updated_at on a local copy. The caller's struct is NOT
    // mutated — callers may keep their original value for UI echo, diff checks,
    // or retry with deliberate preserved timestamps.
    FilamentSlotOverride stamped = ovr;
    stamped.updated_at = std::chrono::system_clock::now();

    nlohmann::json record = to_lane_data_record(slot_index, stamped);

    // Per-slot keys mean no read-modify-write: each slot is its own DB entry.
    // Avoids racing concurrent edits on different slots.
    const std::string key = format_lane_key(slot_index, key_style_);

    // Lifetime safety: Moonraker's request tracker can fire the error callback
    // well after save_async returns (default ~60s timeout). The store may be
    // destroyed in the meantime (backend swap, reconnect). Do NOT capture
    // `this` — only value-captured copies, which keep the lambda self-contained.
    // cache_path_copy + stamped are captured into the success lambda so the
    // cache write (write_cache_slot, a free function) runs with no `this`.
    const std::string backend_id_copy = backend_id_;
    const std::filesystem::path cache_path_copy = cache_path();

    api_->database_post_item(
        namespace_, key, record,
        [cb, cache_path_copy, backend_id_copy, slot_index, stamped]() {
            // MR DB write succeeded — refresh our local read-cache. Errors in
            // write_cache_slot are logged at warn and do NOT affect the user
            // callback: the DB is the source of truth, and a cache write
            // failure must not pretend the save failed.
            write_cache_slot(cache_path_copy, backend_id_copy, slot_index, &stamped);
            if (cb)
                cb(true, "");
        },
        [cb, backend_id_copy, key](const MoonrakerError& err) {
            // Save failures are user-visible (unlike namespace-missing on load,
            // which we swallow at debug). Warn so ops can spot persistent save
            // failures in the logs.
            spdlog::warn("[FilamentSlotOverrideStore:{}] save failed for key {}: {}",
                         backend_id_copy, key, err.message);
            if (cb)
                cb(false, err.message);
        });
}

void FilamentSlotOverrideStore::clear_async(int slot_index, SaveCallback cb) {
    if (!api_) {
        if (cb)
            cb(false, "no API");
        return;
    }
    // Reject negative slot indices symmetrically with save_async and
    // from_lane_data_record (matches OrcaSlicer's MoonrakerPrinterAgent.cpp:796).
    if (slot_index < 0) {
        if (cb)
            cb(false, "invalid slot_index");
        return;
    }

    const std::string key = format_lane_key(slot_index, key_style_);

    // Lifetime safety mirrors save_async: Moonraker's request tracker can fire
    // the error callback ~60s after this returns, well after the store may have
    // been destroyed (backend swap, reconnect). Value-capture only; no `this`.
    // cache_path_copy is captured into the success lambda so write_cache_slot
    // (a free function) runs with no `this`.
    const std::string backend_id_copy = backend_id_;
    const std::filesystem::path cache_path_copy = cache_path();

    api_->database_delete_item(
        namespace_, key,
        [cb, cache_path_copy, backend_id_copy, slot_index]() {
            // MR DB delete succeeded — erase the slot from our read-cache too.
            // Passing nullptr is the documented "erase this slot" signal.
            // Cache errors are logged at warn but never reported to cb: the DB
            // is the source of truth and we don't lie about the clear result.
            write_cache_slot(cache_path_copy, backend_id_copy, slot_index, nullptr);
            if (cb)
                cb(true, "");
        },
        [cb, backend_id_copy, key](const MoonrakerError& err) {
            // Clear failures are user-visible — warn so ops can spot persistent
            // failures in the logs. (Missing-key is mapped to success by the
            // real api layer, so reaching this lambda means a real failure.)
            spdlog::warn("[FilamentSlotOverrideStore:{}] clear failed for key {}: {}",
                         backend_id_copy, key, err.message);
            if (cb)
                cb(false, err.message);
        });
}

// =============================================================================
// Seated-lane persistence
// =============================================================================
//
// The "seated" key is a sibling scalar in the lane_data namespace holding the
// 0-based index of the lane currently loaded to the toolhead. It is NOT
// per-lane, so it does not go through format_lane_key()/to_lane_data_record()
// and is NOT mirrored into the local per-slot read-cache. The value on disk is
// a plain JSON integer.

void FilamentSlotOverrideStore::save_seated_slot_async(int slot_index, SaveCallback cb) {
    if (!api_) {
        if (cb)
            cb(false, "no API");
        return;
    }
    // Reject negative indices symmetrically with save_async — a negative seated
    // index is never a valid lane. "Nothing seated" goes through
    // clear_seated_slot_async, not a sentinel index.
    if (slot_index < 0) {
        if (cb)
            cb(false, "invalid slot_index");
        return;
    }

    // Lifetime safety mirrors save_async: Moonraker's request tracker can fire
    // the error callback ~60s after this returns, well after the store may have
    // been destroyed. Value-capture only; no `this`.
    const std::string backend_id_copy = backend_id_;

    api_->database_post_item(
        namespace_, "seated", nlohmann::json(slot_index),
        [cb]() {
            if (cb)
                cb(true, "");
        },
        [cb, backend_id_copy](const MoonrakerError& err) {
            spdlog::warn("[FilamentSlotOverrideStore:{}] save seated failed: {}", backend_id_copy,
                         err.message);
            if (cb)
                cb(false, err.message);
        });
}

void FilamentSlotOverrideStore::clear_seated_slot_async(SaveCallback cb) {
    if (!api_) {
        if (cb)
            cb(false, "no API");
        return;
    }

    // Value-capture only; no `this` (see save_seated_slot_async).
    const std::string backend_id_copy = backend_id_;

    api_->database_delete_item(
        namespace_, "seated",
        [cb]() {
            if (cb)
                cb(true, "");
        },
        [cb, backend_id_copy](const MoonrakerError& err) {
            spdlog::warn("[FilamentSlotOverrideStore:{}] clear seated failed: {}", backend_id_copy,
                         err.message);
            if (cb)
                cb(false, err.message);
        });
}

std::optional<int> FilamentSlotOverrideStore::load_seated_slot_blocking() {
    if (!api_)
        return std::nullopt;

    // Same shared_ptr<SyncState> + cv.wait_for discipline as load_blocking:
    // a callback firing after our local timeout must not touch a freed stack
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
    // Must be a plain non-negative integer. The store doesn't know NUM_PORTS,
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
        //
        // BUT user-locked fields are NEVER overwritten — set_slot_info
        // (persist=true) tags the fields it wrote, and that tag survives
        // restart via lane_data. Without this guard, a stale firmware
        // re-emission (AD5X post-print FFMInfo revert, #965) would clobber
        // the user's choice. Auto-mirror writes leave the locks false so
        // subsequent firmware changes still propagate; users restore the
        // auto-track behavior on a previously-locked slot by calling
        // clear_slot_override.
        if (!ovr.user_locked_color && (!ovr.color_set || ovr.color_rgb != firmware_color)) {
            ovr.color_rgb = firmware_color;
            ovr.color_set = true;
            changed = true;
        }
        if (!ovr.user_locked_material && ovr.material != firmware_material) {
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
        //
        // The user-lock checks are redundant with the unset checks in the
        // common path (set_slot_info sets color_set together with
        // user_locked_color), but they are the authoritative "the user chose
        // this" signal and every mirror policy honors them. Keeping both
        // policies lock-aware means a record whose locks and value-set flags
        // ever disagree — a legacy record, a hand-edited lane_data entry, a
        // third-party writer — still cannot lose the user's choice here.
        if (!ovr.user_locked_color && !ovr.color_set) {
            ovr.color_rgb = firmware_color;
            ovr.color_set = true;
            changed = true;
        }
        if (!ovr.user_locked_material && ovr.material.empty() && !firmware_material.empty()) {
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

    auto it = baseline_.find(slot_index);
    if (it == baseline_.end()) {
        baseline_[slot_index] = observed;
        return FingerprintEvent::Baseline;
    }
    if (it->second == observed)
        return FingerprintEvent::Unchanged;

    if (previous)
        *previous = it->second;
    // Advance the baseline BEFORE classifying, so a caller whose follow-up
    // action fails (a rejected clear_async, say) doesn't re-fire the same event
    // on every subsequent poll.
    it->second = observed;

    auto exp = expected_.find(slot_index);
    if (exp == expected_.end())
        return FingerprintEvent::Changed;

    // Single-shot per value: an exact match consumes only its own entry
    // (OwnWriteEcho). Any other change means the slot moved somewhere we did
    // not send it, so whatever echoes were outstanding are no longer
    // meaningful — consume them all and return to normal swap detection
    // immediately rather than staying suppressed.
    for (auto eit = exp->second.begin(); eit != exp->second.end(); ++eit) {
        if (*eit == observed) {
            exp->second.erase(eit);
            if (exp->second.empty())
                expected_.erase(exp);
            return FingerprintEvent::OwnWriteEcho;
        }
    }
    expected_.erase(exp);
    return FingerprintEvent::Changed;
}

void SlotFingerprintTracker::expect(int slot_index, std::string expected_value) {
    expected_[slot_index] = {std::move(expected_value)};
}

void SlotFingerprintTracker::expect_any_of(int slot_index,
                                           std::vector<std::string> expected_values) {
    std::vector<std::string> kept;
    kept.reserve(expected_values.size());
    for (auto& v : expected_values) {
        if (!v.empty())
            kept.push_back(std::move(v));
    }
    if (kept.empty())
        expected_.erase(slot_index);
    else
        expected_[slot_index] = std::move(kept);
}

void SlotFingerprintTracker::forget_expected(int slot_index) {
    expected_.erase(slot_index);
}

std::optional<std::string> SlotFingerprintTracker::baseline(int slot_index) const {
    auto it = baseline_.find(slot_index);
    if (it == baseline_.end())
        return std::nullopt;
    return it->second;
}

bool SlotFingerprintTracker::has_expected(int slot_index) const {
    return expected_.find(slot_index) != expected_.end();
}

void SlotFingerprintTracker::clear() {
    baseline_.clear();
    expected_.clear();
}

// =============================================================================
// merge_override — shared spec §5 implementation
// =============================================================================

MergeResult merge_override(SlotInfo& slot, const FilamentSlotOverride& o,
                           const MergeOptions& options) {
    // Rule 1 — external re-bind. Another well-behaved writer (Mainsail, the
    // AFC plugin) explicitly set a DIFFERENT spool on this lane. That is a
    // statement, not a guess: the whole record drops, firmware truth paints.
    // Never gated by the setting; never fires on eject's 0/null (#1281 step 7).
    // The two suppress ids exclude our OWN in-flight re-links: after
    // HelixScreen writes a spool id, status frames already parsed (or parsed
    // before the write lands) keep reporting the OLD firmware id for a poll
    // or two — that stale frame is us, not Mainsail (SlotFingerprintTracker
    // ::expect() semantics; see MergeOptions). Suppression only skips this
    // clear; the field merge below still paints the override.
    if (slot.spoolman_id > 0 && o.spoolman_id > 0 && slot.spoolman_id != o.spoolman_id &&
        slot.spoolman_id != options.suppress_rebind_firmware_old_id &&
        slot.spoolman_id != options.suppress_rebind_firmware_new_id) {
        MergeResult r;
        r.cleared_rebind = true;
        return r;
    }
    // Rule 2 — eject signal, setting-gated. Only meaningful where firmware
    // reports ids while loaded (AFC, Happy Hare): there, 0/null is the eject
    // the plugin itself writes. Elsewhere 0 is the everyday reading — stock
    // CFS firmware reports no ids at all, and flat-schema CFS parses a
    // per-slot id without giving 0 an eject meaning — so the rule stays
    // inert.
    if (options.printer_reports_spool_ids && slot.spoolman_id <= 0 && o.spoolman_id > 0 &&
        !options.keep_spool_info_on_eject) {
        MergeResult r;
        r.cleared_eject = true;
        return r;
    }
    // Spec §5 — override wins field-by-field; sentinels fall through.
    if (!o.brand.empty())
        slot.brand = o.brand;
    if (!o.spool_name.empty())
        slot.spool_name = o.spool_name;
    if (o.spoolman_id > 0)
        slot.spoolman_id = o.spoolman_id;
    if (o.spoolman_vendor_id > 0)
        slot.spoolman_vendor_id = o.spoolman_vendor_id;
    if (o.remaining_weight_g >= 0.0f)
        slot.remaining_weight_g = o.remaining_weight_g;
    if (o.total_weight_g >= 0.0f)
        slot.total_weight_g = o.total_weight_g;
    if (o.color_set)
        slot.color_rgb = o.color_rgb;
    if (!o.color_name.empty())
        slot.color_name = o.color_name;
    if (!o.material.empty())
        slot.material = o.material;
    if (!o.catalog_id.empty())
        slot.catalog_id = o.catalog_id;
    if (!o.product_name.empty())
        slot.product_name = o.product_name;
    return {};
}

bool publish_external_lane(FilamentSlotOverrideStore* store, int lane_index, const SlotInfo* spool,
                           const std::string& log_tag) {
    if (!store || lane_index < 0) {
        return false;
    }

    // Identity = anything a slicer could map to: a Spoolman link, a material,
    // or a picked color. SlotInfo's color default (AMS_DEFAULT_SLOT_COLOR
    // gray) is the "never picked" sentinel — black (0x000000) is a real pick
    // and must publish.
    const bool color_picked = spool != nullptr && spool->color_rgb != AMS_DEFAULT_SLOT_COLOR;
    const bool has_identity =
        spool != nullptr && (spool->spoolman_id > 0 || !spool->material.empty() || color_picked);

    if (!has_identity) {
        // Clear, not publish: an identity-less record would squat the lane
        // with an empty phantom a slicer renders as an unknown tray.
        store->clear_async(lane_index, [log_tag, lane_index](bool ok, std::string err) {
            if (!ok) {
                spdlog::warn("{} external-spool lane clear failed ({}): {}", log_tag, lane_index,
                             err);
            }
        });
        return false;
    }

    FilamentSlotOverride ovr;
    ovr.material = spool->material;
    ovr.brand = spool->brand;
    ovr.spool_name = spool->spool_name;
    ovr.spoolman_id = spool->spoolman_id;
    ovr.spoolman_vendor_id = spool->spoolman_vendor_id;
    ovr.remaining_weight_g = spool->remaining_weight_g;
    ovr.total_weight_g = spool->total_weight_g;
    ovr.catalog_id = spool->catalog_id;
    ovr.product_name = spool->product_name;
    // User-set data (settings store / Spoolman), same sentinel rule as
    // has_identity: default gray = never picked, black = picked.
    ovr.color_rgb = spool->color_rgb;
    ovr.color_set = color_picked;
    populate_temps_from_slot_info(ovr, *spool);

    store->save_async(lane_index, ovr, [log_tag, lane_index](bool ok, std::string err) {
        if (!ok) {
            spdlog::warn("{} external-spool lane publish failed ({}): {}", log_tag, lane_index,
                         err);
        }
    });
    spdlog::debug("{} published external spool as lane {} (material={}, color=#{:06X})", log_tag,
                  lane_index, spool->material, spool->color_rgb);
    return true;
}

} // namespace helix::ams
