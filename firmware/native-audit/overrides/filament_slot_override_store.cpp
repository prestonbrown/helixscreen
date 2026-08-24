// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_slot_override_store.h"

#include "ams_types.h"
#include "data_root_resolver.h"
#include "filament_database.h"
#include "filament_slot_override.h"
#include "i_moonraker_api.h"
#include "moonraker_error.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

namespace helix::ams {

namespace {

std::string format_iso8601(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

std::chrono::system_clock::time_point parse_iso8601(const std::string& s) {
    std::tm tm{};
    std::istringstream is(s);
    is >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (is.fail())
        return {};
#ifdef ESP_PLATFORM // AUDIT OVERRIDE: newlib has no timegm(); portable days-since-epoch calc
    const int y = tm.tm_year + 1900, mo = tm.tm_mon + 1;
    const int a = (14 - mo) / 12, yy = y + 4800 - a, mm = mo + 12 * a - 3;
    const long days = tm.tm_mday + (153 * mm + 2) / 5 + 365L * yy + yy / 4 - yy / 100 + yy / 400 - 32045 - 2440588;
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
// NOTE on indexing: the Moonraker DB key is 1-based (AFC convention: lane1,
// lane2, ...) but the "lane" field inside the record is 0-based (matches Orca's
// tool-index interpretation). The 1-based key is produced by lane_key() in the
// header; this function writes the 0-based inner field.
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
    if (!o.material.empty())
        j["material"] = o.material;
    // helix_locked_* are HelixScreen-internal markers. Always emit both (even
    // when false) so a future re-load can distinguish "explicit auto-mirror,
    // safe to track" from "missing key, fall back to pessimistic default."
    // OrcaSlicer's MoonrakerPrinterAgent ignores unknown fields, so the two
    // extra booleans per slot cost nothing on its side.
    j["helix_locked_color"] = o.user_locked_color;
    j["helix_locked_material"] = o.user_locked_material;
    if (!o.brand.empty()) {
        j["vendor"] = o.brand;      // legacy/AFC key
        j["vendor_name"] = o.brand; // Happy Hare key; the one Orca is most likely
                                    // to consume as it moves toward vendor-aware
                                    // matching. Zero-cost alias today (Orca
                                    // ignores unknown keys).
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
        j["spool_name"] = o.spool_name; // HelixScreen/AFC key
        j["name"] = o.spool_name;       // Happy Hare key; alias for forward-compat
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

// Parse AFC-shaped record (+ our extensions) back into FilamentSlotOverride.
// Returns (slot_index, override) where slot_index comes from the "lane" field
// (which Orca requires). nullopt if the record is malformed (non-object or
// missing/invalid "lane" field).
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
    o.material = j.value("material", "");
    // Pessimistic legacy-default: pre-fix records have no helix_locked_* key.
    // Assume the field IS user-locked when it carries a value — protects
    // existing overrides from auto-mirror clobber after upgrade. New records
    // (post-fix) carry the explicit flag and round-trip exactly. See struct
    // doc + #965 for rationale.
    o.user_locked_color = j.value("helix_locked_color", o.color_set);
    o.user_locked_material = j.value("helix_locked_material", !o.material.empty());
    // Prefer our own `vendor` key; fall back to Happy Hare's `vendor_name` so
    // alias-only records (e.g. written by HH's mmu_server push_lane_data) read
    // correctly. Round-trips of our own records stay exact.
    o.brand = j.value("vendor", j.value("vendor_name", std::string()));
    o.spoolman_id = j.value("spool_id", 0);
    o.bed_temp = j.value("bed_temp", 0);
    o.nozzle_temp = j.value("nozzle_temp", 0);
    if (j.contains("scan_time") && j["scan_time"].is_string()) {
        o.updated_at = parse_iso8601(j["scan_time"].get<std::string>());
    }
    // Prefer `spool_name`; fall back to Happy Hare's `name` alias.
    o.spool_name = j.value("spool_name", j.value("name", std::string()));
    o.spoolman_vendor_id = j.value("spoolman_vendor_id", 0);
    o.remaining_weight_g = j.value("remaining_weight_g", -1.0f);
    o.total_weight_g = j.value("total_weight_g", -1.0f);
    o.color_name = j.value("color_name", "");
    return std::make_pair(slot_index, o);
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
        result[slot_index] = from_json(it.value());
    }
    return result;
}

} // namespace

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
        {"user_locked_color", o.user_locked_color},
        {"user_locked_material", o.user_locked_material},
        {"bed_temp", o.bed_temp},
        {"nozzle_temp", o.nozzle_temp},
        {"updated_at", format_iso8601(o.updated_at)},
    };
}

FilamentSlotOverride from_json(const nlohmann::json& j) {
    FilamentSlotOverride o;
    o.brand = j.value("brand", "");
    o.spool_name = j.value("spool_name", "");
    o.spoolman_id = j.value("spoolman_id", 0);
    o.spoolman_vendor_id = j.value("spoolman_vendor_id", 0);
    o.remaining_weight_g = j.value("remaining_weight_g", -1.0f);
    o.total_weight_g = j.value("total_weight_g", -1.0f);
    o.color_rgb = j.value("color_rgb", 0u);
    // Pre-fix caches don't have color_set; reconstruct from the old "0 = unset"
    // convention so a returning user's pre-existing overrides keep their
    // intended meaning. New caches always emit the explicit boolean.
    o.color_set = j.value("color_set", o.color_rgb != 0u);
    o.color_name = j.value("color_name", "");
    o.material = j.value("material", "");
    // Pessimistic legacy-default — see from_lane_data_record + #965.
    o.user_locked_color = j.value("user_locked_color", o.color_set);
    o.user_locked_material = j.value("user_locked_material", !o.material.empty());
    o.bed_temp = j.value("bed_temp", 0);
    o.nozzle_temp = j.value("nozzle_temp", 0);
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

FilamentSlotOverrideStore::FilamentSlotOverrideStore(IMoonrakerAPI* api, std::string backend_id)
    : api_(api), backend_id_(std::move(backend_id)) {}

std::filesystem::path FilamentSlotOverrideStore::cache_dir_effective() const {
    return cache_dir_.empty() ? std::filesystem::path(helix::get_user_config_dir()) : cache_dir_;
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
                   std::chrono::milliseconds timeout, const std::filesystem::path& cache_dir) {
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

        FilamentSlotOverride o = from_json(it.value());
        o.updated_at = std::chrono::system_clock::now();
        migrated[slot_index] = o;
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
        // Local name is `lane_data_key` (NOT `lane_key`) to avoid shadowing
        // the static member FilamentSlotOverrideStore::lane_key().
        const std::string lane_data_key = "lane" + std::to_string(slot_index + 1);
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

} // namespace

std::unordered_map<int, FilamentSlotOverride> FilamentSlotOverrideStore::load_blocking() {
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

    for (auto it = received_copy.begin(); it != received_copy.end(); ++it) {
        const std::string& key = it.key();
        // Only consider lane-prefixed keys (AFC convention). Ignore any
        // unrelated data that may live in the lane_data namespace.
        if (key.rfind("lane", 0) != 0) {
            spdlog::debug("[FilamentSlotOverrideStore:{}] skipping non-lane key: {}", backend_id_,
                          key);
            continue;
        }
        auto parsed = from_lane_data_record(it.value());
        if (!parsed) {
            spdlog::debug("[FilamentSlotOverrideStore:{}] from_lane_data_record failed for {}",
                          backend_id_, key);
            continue;
        }
        result[parsed->first] = parsed->second;
    }

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
        auto migrated = try_migrate_legacy(api_, backend_id_, load_timeout_, cache_dir_effective());
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
    const std::string key = lane_key(slot_index);

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

    const std::string key = lane_key(slot_index);

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
// per-lane, so it does not go through lane_key()/to_lane_data_record() and is
// NOT mirrored into the local per-slot read-cache. The value on disk is a
// plain JSON integer.

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

} // namespace helix::ams
