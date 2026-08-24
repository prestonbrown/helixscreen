// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "filament_slot_override.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "hv/json.hpp"

class IMoonrakerAPI;
class FilamentSlotOverrideStoreTestAccess;

namespace helix::ams {

// Outer Moonraker DB key convention for per-slot lane_data records. The two
// styles carry the SAME 0-based inner "lane" field but different outer keys:
//   - Lane: "laneN" (1-based, AFC/Happy Hare convention) — filament systems.
//   - Tool: "T<n>"  (0-based, Orca/Mainsail tool convention) — tool changers.
// A tool changer converging on the T<n> key style makes HelixScreen writes
// overwrite Mainsail #2510's records instead of duplicating them (both readers
// key off the inner "lane" field, so a shared outer key avoids Orca's no-dedup
// collision). See docs/specs/filament_slots.md § "Interoperating readers and
// writers".
enum class LaneKeyStyle { Lane, Tool };

// Maps an AmsType to its lane_data key style. Tool changers (Snapmaker, generic
// TOOL_CHANGER) use T<n> keys; every filament-switching system uses laneN.
// Deriving from is_tool_changer() keeps the policy in one place — never branch
// on backend_id, and never use !is_filament_system() (it includes SNAPMAKER in
// both lists, ams_types.h).
inline LaneKeyStyle lane_key_style_for(AmsType t) {
    return is_tool_changer(t) ? LaneKeyStyle::Tool : LaneKeyStyle::Lane;
}

// Counts of lane_data records that are inconsistent or invisible to other
// readers. Detected read-only at load and logged once — we do NOT auto-rewrite
// third-party/corrupt records (that would vandalize a shared namespace); the
// one-shot laneN->T<n> migration only ever touches keys HelixScreen authored.
// Note: out-of-range slots are intentionally NOT counted — the store does not
// know NUM_PORTS (the caller range-checks), so it cannot honestly detect them.
struct LaneDataAnomalies {
    int int_typed_lane = 0;     ///< inner "lane" is an int, not a string — OrcaSlicer drops these
    int key_inner_mismatch = 0; ///< key looks like laneN/T<n> but disagrees with the inner index
    int unparseable = 0;        ///< non-"seated" object carrying no valid "lane" field
    int duplicate_slot = 0;     ///< more than one record resolving to the same slot index
    [[nodiscard]] int total() const {
        return int_typed_lane + key_inner_mismatch + unparseable + duplicate_slot;
    }
};

// Read-only scan of a raw lane_data namespace document. Pure: no DB access, no
// mutation. Skips the "seated" sibling scalar. Used for the one-shot load-time
// diagnostic; also unit-tested directly.
[[nodiscard]] LaneDataAnomalies scan_lane_data_anomalies(const nlohmann::json& namespace_doc);

// Parse AFC-shaped record (+ our extensions) back into FilamentSlotOverride.
// This is the wire-format parser: the shared shape read by scan_lane_data_anomalies,
// the migration helpers, and load_blocking, and exercised directly by tests to
// verify round-tripping without going through the full async load/save path.
// Returns (slot_index, override), or nullopt if the record is malformed (non-object
// or missing/invalid "lane" field).
[[nodiscard]] std::optional<std::pair<int, FilamentSlotOverride>>
from_lane_data_record(const nlohmann::json& j);

/// Process-wide fallback dir for the store's on-disk read-cache, consulted
/// by cache_dir_effective() when an instance has no per-instance cache_dir_
/// pinned. Production leaves it empty (instances then resolve
/// helix::get_user_config_dir()). The shared test fixture points it at its
/// per-PID sandbox before main() so AMS backend tests — which construct real
/// backends, and thus real stores, without pinning a dir — cannot write
/// filament_slot_overrides.json into the repo's config/ dir. Same role
/// ToolState::set_config_dir() plays for tool_spools.json, and mutable for
/// the same reason AppConstants::Update::detail::state_dir_ref() is.
/// Write it before threads exist; afterwards it is read-only.
namespace detail {
inline std::filesystem::path& slot_override_cache_dir_ref() {
    static std::filesystem::path dir;
    return dir;
}
} // namespace detail

class FilamentSlotOverrideStore {
  public:
    // key_style defaults to Lane so the many lane-based construction sites and
    // tests need no change. Production sites pass lane_key_style_for(get_type())
    // so the correct style is derived from the backend's AmsType.
    // ns selects the Moonraker DB namespace. It defaults to the shared
    // "lane_data" for the backends that legitimately live there (IFS, ACE, CFS,
    // Snapmaker). AFC and Happy Hare MUST pass a private namespace: their own
    // Klipper plugins own lane_data, AFC deletes that whole namespace on every
    // boot and full-POSTs each lane record, and load_blocking() would otherwise
    // ingest those foreign records as if they were user overrides.
    FilamentSlotOverrideStore(IMoonrakerAPI* api, std::string backend_id,
                              LaneKeyStyle key_style = LaneKeyStyle::Lane,
                              std::string ns = "lane_data");

    /// Test-only view of the configured namespace.
    [[nodiscard]] const std::string& namespace_for_test() const {
        return namespace_;
    }

    // Blocking load from Moonraker database (called only at backend init time).
    // Falls back to the local read-cache when the DB round-trip fails.
    //
    // Never throws. lane_data is a namespace shared with AFC, Happy Hare,
    // Mainsail and hand edits, so a malformed or null-bearing record is an
    // expected input, not a bug — it costs at most the affected slots. See the
    // exception-boundary comment on the definition.
    std::unordered_map<int, FilamentSlotOverride> load_blocking();

    using SaveCallback = std::function<void(bool success, std::string error)>;
    void save_async(int slot_index, const FilamentSlotOverride& override, SaveCallback cb);
    void clear_async(int slot_index, SaveCallback cb);

    // Seated-lane persistence. Unlike the per-lane overrides above, this is a
    // single scalar (the 0-based index of the lane currently loaded to the
    // toolhead) stored under a sibling key "seated" in the same lane_data
    // namespace. The value on disk is a plain JSON integer.

    // Persist the 0-based seated lane index to lane_data/"seated".
    // Fire-and-forget, mirrors save_async (dispatches via
    // api_->database_post_item, does not block).
    void save_seated_slot_async(int slot_index, SaveCallback cb);

    // Remove lane_data/"seated" (nothing currently seated). Mirrors clear_async.
    void clear_seated_slot_async(SaveCallback cb);

    // Blocking read of lane_data/"seated" at init time (mirrors load_blocking's
    // cv.wait_for pattern + load_timeout_). Returns nullopt if absent/unreachable
    // or the stored value is not a valid 0-based lane index. The caller is
    // responsible for range-checking against its own NUM_PORTS.
    std::optional<int> load_seated_slot_blocking();

    const std::string& backend_id() const {
        return backend_id_;
    }

  private:
    // Test-only access to mutate load_timeout_ without exposing a public
    // setter. Per L065, prefer friend-class over test-only public methods.
    friend class ::FilamentSlotOverrideStoreTestAccess;

    // Real body of load_blocking(). Split out so load_blocking() itself is a
    // thin never-throws boundary that no caller has to remember to guard.
    std::unordered_map<int, FilamentSlotOverride> load_blocking_impl();

    IMoonrakerAPI* api_;
    std::string backend_id_;
    // Outer-key style for this backend's lane_data records (laneN vs T<n>). Set
    // once at construction from the backend's AmsType via lane_key_style_for().
    LaneKeyStyle key_style_;
    // Adopts the AFC/OrcaSlicer lane_data Moonraker convention. Each slot is
    // stored under key "laneN" where N is the 1-based slot index (lane1, lane2,
    // ...), or "T<n>" (0-based) on tool changers. Slot index 0 maps to "lane1"
    // (Lane style) or "T0" (Tool style) on disk. See format_lane_key in the
    // .cpp for the exact rule.
    std::string namespace_ = "lane_data";
    // Local timeout for load_blocking()'s cv.wait_for. Defaults to 5 seconds;
    // overridable by FilamentSlotOverrideStoreTestAccess for timeout tests.
    // Stored as milliseconds (not seconds) because tests need sub-second
    // resolution — a chrono::seconds member would truncate 50ms to 0s.
    std::chrono::milliseconds load_timeout_{5000};
    // On-disk read-cache directory. Empty = use helix::get_user_config_dir().
    // Overridable by FilamentSlotOverrideStoreTestAccess so tests write to a
    // per-PID tmp dir instead of polluting the user's config. The cache is
    // NEVER authoritative — the Moonraker DB on the printer is the source of
    // truth. The cache exists only so the UI can show last-known metadata
    // when Moonraker is unreachable at backend init.
    std::filesystem::path cache_dir_;
    // Absolute path to the cache JSON file. Computed from cache_dir_ (or
    // get_user_config_dir() if empty). One file serves all backends; each
    // backend's slots live under doc[backend_id]["slots"].
    std::filesystem::path cache_path() const;
    // Absolute path to the directory used for on-disk caches. Same resolution
    // as cache_path(): cache_dir_ if set, otherwise get_user_config_dir().
    // Migration uses this to locate legacy "{backend_id}_slot_overrides.json"
    // files that pre-date the unified filament_slot_overrides.json format.
    std::filesystem::path cache_dir_effective() const;
};

// =============================================================================
// Shared firmware -> lane_data mirror helper
// =============================================================================
//
// AFC and Happy Hare publish lane_data themselves (their Klipper plugins write
// directly to the Moonraker DB), so OrcaSlicer's MoonrakerPrinterAgent can
// read filament state without HelixScreen's involvement.
//
// CFS, AD5X IFS, and Snapmaker firmware do NOT publish lane_data. HelixScreen
// has to mirror firmware-detected color/material into the lane_data namespace
// so OrcaSlicer's "Sync filaments from Printer" works. This helper centralizes
// that mirror so the three backends share one implementation.
//
// Why a policy enum: backends differ in whether user UI edits propagate back
// to firmware:
//
//   - IFS: set_slot_info writes to Adventurer5M.json — firmware re-reads it
//     and reports the user's chosen color on the next status poll. The mirror
//     can safely overwrite the override with firmware values (except fields
//     the user explicitly locked, per #965 — see MirrorPolicy::OverwriteAlways
//     below) because firmware-truth and user-truth converge.
//
//   - CFS / Snapmaker: set_slot_info does NOT touch the firmware-side
//     material_type / RFID values. If the mirror unconditionally overwrote
//     ovr.color_rgb with firmware-truth, every status poll would erase the
//     user's color override. So these backends use FillUnsetOnly: only fill
//     fields the user hasn't explicitly set. clear_slot_override resets the
//     entry, after which auto-mirror takes over again.
enum class MirrorPolicy {
    /// Overwrite ovr.color_rgb / ovr.material with firmware values, EXCEPT for
    /// fields the user explicitly locked (user_locked_color /
    /// user_locked_material — see #965). Use when user edits propagate back to
    /// firmware so the two views stay in sync (AD5X IFS, Snapmaker paxx12).
    OverwriteAlways,
    /// Only fill ovr.color_rgb / ovr.material when they're currently UNSET
    /// (color_rgb == 0, empty material). Use when user edits don't reach
    /// firmware (CFS, Snapmaker).
    FillUnsetOnly,
};

/// Mirror firmware-detected color/material into `overrides[slot_index]` and
/// fire `store->save_async` to push the resulting record to the lane_data
/// namespace. Caller MUST hold the backend's mutex protecting `overrides`.
///
/// No-op (returns false without writing) when:
///   - !slot_has_filament  (empty / unread slot — no signal)
///   - the chosen policy leaves nothing to change (e.g. FillUnsetOnly when
///     ovr already has both fields set, or OverwriteAlways when ovr already
///     matches firmware)
///
/// Note: `firmware_color == 0` is NOT treated as "no signal" — pure black is
/// a legitimate color the user can load. Backends whose parse path may run
/// before colors are populated (e.g. AD5X IFS) must apply their own
/// color-zero guard upstream of this helper.
///
/// `store` may be null (init-time race / test fixture without MR API) — the
/// in-memory override is still updated, but no save_async is fired.
///
/// `log_tag` is included in the warn log on save failure so multi-backend
/// logs stay attributable.
///
/// Returns true iff `overrides[slot_index]` was actually mutated. Callers
/// (e.g. IFS) use this to drive secondary side-effects like _IFS_VARS sync.
bool mirror_firmware_to_lane_data(FilamentSlotOverrideStore* store,
                                  std::unordered_map<int, FilamentSlotOverride>& overrides,
                                  int slot_index, uint32_t firmware_color,
                                  const std::string& firmware_material, bool slot_has_filament,
                                  MirrorPolicy policy, const std::string& log_tag);

/// Publish (or clear) the external / bypass spool as an extra lane one past
/// the last physical slot, so slicers (OrcaSlicer's MoonrakerPrinterAgent)
/// can select it as the "next tool over" (T4 beside T0-T3). The record rides
/// the same lane_data format as every other lane; readers (Orca) key off the
/// inner 0-based `lane` field, which is `lane_index`.
///
/// Who calls this: AmsBackend::publish_external_spool_lane overrides — the
/// backend gates on its own supports_bypass + store, then hands off here.
/// Backends whose firmware owns the lane_data namespace (AFC, Happy Hare)
/// must pass a store constructed on the SHARED "lane_data" namespace, not
/// their private override namespace.
///
/// `spool` null or identity-less (no Spoolman id, no material, default-gray
/// color) CLEARS the lane instead of publishing an empty phantom. Pure black
/// (0x000000) is a real pick and publishes.
///
/// Returns true if a record was published (false for the clear path).
bool publish_external_lane(FilamentSlotOverrideStore* store, int lane_index, const SlotInfo* spool,
                           const std::string& log_tag);

// =============================================================================
// Shared override-wins merge (filament_slots.md §5)
// =============================================================================
//
// Every AMS backend merges a loaded FilamentSlotOverride onto firmware-reported
// SlotInfo values before the UI paints a lane. This is the ONE implementation
// of that policy plus the two cross-field rules (external re-bind, eject) that
// previously lived as hand-rolled if-chains per backend.

struct MergeOptions {
    /// From SettingsManager::get_ams_keep_spool_info_on_eject().
    /// Default true = today's designed retention across eject.
    bool keep_spool_info_on_eject = true;
    /// True only on backends whose firmware reports a spool id while a spool
    /// is loaded (AFC, Happy Hare). There — and only there — a firmware id of
    /// 0/null means "ejected". Elsewhere 0 is the everyday reading and MUST
    /// NOT be treated as eject — flat-schema CFS does parse a per-slot spool
    /// id (arming Rule 1's re-bind) but gives 0 no eject meaning.
    bool printer_reports_spool_ids = false;
    /// Own-write echo suppression for Rule 1, mirroring
    /// SlotFingerprintTracker::expect() semantics. When HelixScreen itself
    /// just (re)linked a spool id on this slot, in-flight status frames keep
    /// reporting the OLD firmware id for a poll or two; Rule 1 must not read
    /// such a stale frame as an external re-bind and destroy the just-saved
    /// override. Non-zero values are the ids firmware may legitimately
    /// report while the write is in flight: the id it last reported before
    /// the write and the id we just wrote. Suppression affects ONLY the
    /// re-bind clear — the §5 field merge paints the override normally
    /// either way. 0 = none.
    int suppress_rebind_firmware_old_id = 0;
    /// The just-written id (see suppress_rebind_firmware_old_id). 0 = none.
    int suppress_rebind_firmware_new_id = 0;
};

struct MergeResult {
    bool cleared_rebind = false; ///< firmware re-bound to a different spool; record dropped
    bool cleared_eject = false;  ///< eject signal + setting OFF; record dropped
};

/// Single implementation of filament_slots.md §5 plus the two cross-field
/// rules. `slot` carries FIRMWARE-reported values on entry; on return it
/// carries the values the UI should paint. When either cleared_* is true the
/// caller must drop its in-memory override and persist the clear.
MergeResult merge_override(SlotInfo& slot, const FilamentSlotOverride& o,
                           const MergeOptions& options);

// =============================================================================
// Shared per-slot firmware-observation baseline tracker
// =============================================================================

/// Classification of one firmware observation against the per-slot baseline.
enum class FingerprintEvent {
    /// Empty observation: no tag, unread reader, or the slot wasn't included in
    /// this (incremental) status update. Baseline is left untouched — otherwise
    /// a tag-less poll would overwrite a real prior value and mask a genuine
    /// change on the next good read.
    NoSignal,
    /// First real observation for this slot. Establishes the baseline; NEVER an
    /// event, even when a previously-loaded override disagrees with it.
    Baseline,
    /// Identical to the baseline — the same physical spool re-observed.
    Unchanged,
    /// Changed to exactly the value a prior expect() said to await, i.e. this
    /// is the firmware echoing back a write HelixScreen itself made.
    OwnWriteEcho,
    /// Changed for some reason other than our own pending write — for the RFID
    /// backends this means a physical spool swap.
    Changed,
};

/// Per-slot "what did firmware last report for this slot?" tracker, shared by
/// the RFID-fingerprint backends (CFS, Snapmaker). It owns only the
/// bookkeeping — deciding what a given event *means* (clear the override, sync
/// lane_data, log) stays in each backend, so their policies can differ.
///
/// Beyond the plain baseline compare it carries an `expect()` slot: backends
/// that write a value back to firmware (CFS's BOX_MODIFY_TN_DATA identity
/// push) record the value they expect to see echoed. Because the write is
/// asynchronous, firmware keeps reporting the OLD value for an unknown number
/// of polls before the echo lands — so the expectation must SURVIVE those
/// polls rather than overwrite the baseline immediately. Those intervening
/// polls classify as Unchanged; the echo itself classifies as OwnWriteEcho.
///
/// Each expectation is single-shot and is consumed by the first change of any
/// kind, so a genuine physical swap that lands while a write is in flight is
/// still reported as Changed and never permanently blinds swap detection for
/// that slot.
///
/// A backend that writes TWO fields with one dispatch (CFS writes
/// material_type then color_value in one script) can land a poll between the
/// two echoes, observing an intermediate value neither write alone produces.
/// `expect_any_of()` registers the full set of values the slot may transiently
/// or finally report; each observed value consumes only its own entry, so the
/// intermediate echo and the final echo both classify as OwnWriteEcho. A
/// change to a value NOT in the set still consumes everything and reports
/// Changed — the physical-swap guarantee above is unchanged.
class SlotFingerprintTracker {
  public:
    /// Feed one observation. When the result is OwnWriteEcho or Changed and
    /// `previous` is non-null, it receives the superseded baseline value (for
    /// logging). The baseline is advanced BEFORE returning a change event so a
    /// caller whose follow-up action fails doesn't re-fire on every poll.
    FingerprintEvent observe(int slot_index, const std::string& observed,
                             std::string* previous = nullptr);

    /// Record the value this slot is expected to report once a write we just
    /// issued reaches firmware. Replaces any prior unconsumed expectation.
    void expect(int slot_index, std::string expected_value);

    /// Multi-write variant of expect(): registers every value the slot may
    /// report between the first and last echo of a multi-field write (the
    /// intermediate composites and the final one). Each is consumed only by an
    /// exact match; any other change clears them all. Empty strings are
    /// dropped; an all-empty input is equivalent to forget_expected().
    void expect_any_of(int slot_index, std::vector<std::string> expected_values);

    /// Drop a pending expectation (e.g. the write failed to dispatch, so no
    /// echo is coming and the next change is genuinely external).
    void forget_expected(int slot_index);

    /// Current baseline for a slot, or nullopt when none observed yet.
    [[nodiscard]] std::optional<std::string> baseline(int slot_index) const;

    /// Whether an unconsumed expectation is pending for a slot.
    [[nodiscard]] bool has_expected(int slot_index) const;

    void clear();

  private:
    std::unordered_map<int, std::string> baseline_;
    /// Pending expected values per slot. Single-element for expect(); the
    /// intermediate+final composites for expect_any_of().
    std::unordered_map<int, std::vector<std::string>> expected_;
};

} // namespace helix::ams
