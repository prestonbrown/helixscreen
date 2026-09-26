// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "filament_slot_override.h"
#include "lane_source_store.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
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

// Emit the lane_data document a stored override is written as.
//
// The wire-format emitter, and the inverse of from_lane_data_record below.
// save_async is its production caller; it is declared here because the lock
// keys and the declared set live on this document and nowhere else, so a
// fixture seeding a stored override has to build the same document the store
// would have written before anything can read authorship back off it. A record
// classified against an empty document reads as one written before the
// declared set existed.
[[nodiscard]] nlohmann::json to_lane_data_record(int slot_index, const FilamentSlotOverride& o);

// Parse AFC-shaped record (+ our extensions) back into FilamentSlotOverride.
// This is the wire-format parser: the shared shape read by scan_lane_data_anomalies,
// the migration helpers, and load_blocking, and exercised directly by tests to
// verify round-tripping without going through the full async load/save path.
// Returns (slot_index, override), or nullopt if the record is malformed (non-object
// or missing/invalid "lane" field).
[[nodiscard]] std::optional<std::pair<int, FilamentSlotOverride>>
from_lane_data_record(const nlohmann::json& j);

/// A parsed lane_data record beside the object it came from.
///
/// The wire object travels with the record because sources_from_record() asks
/// the document whether it carries a declared set at all: a record written
/// before the set existed falls back to the legacy rule for its brand, spool
/// name and vendor id, and the parsed struct cannot say which it was.
struct LaneDataRecord {
    FilamentSlotOverride record;
    nlohmann::json wire;
};

/// Parse a raw namespace document into per-slot records, reconciling duplicate
/// slots in favour of the key canonical for @p key_style.
///
/// Pure: no DB access, no disk, no mutation. This is the read half of
/// load_blocking() without its one-shot key migration, Orca heal and cache
/// write, so a re-read can reach the same records without redoing init-time
/// work. @p log_tag attributes the duplicate-slot warnings to a backend.
[[nodiscard]] std::unordered_map<int, LaneDataRecord>
parse_namespace_document(const nlohmann::json& namespace_doc, LaneKeyStyle key_style,
                         const std::string& log_tag = "shared");

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

    using ReloadCallback = std::function<void(std::unordered_map<int, LaneDataRecord>)>;

    /// Re-fetch this store's namespace and hand the parsed records to @p cb.
    ///
    /// Unlike load_blocking() this neither blocks nor runs any of the
    /// init-time repair work, so a caller wanting a fresh view of a namespace
    /// several writers share can ask for one at any time.
    ///
    /// @p cb is never called on failure: a re-read that could not reach the
    /// database must leave what a lane already knows standing. @p cb runs on
    /// the API's callback thread; marshal from there.
    void reload_async(ReloadCallback cb);

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

    /// The lane_data records this store's last load_blocking() parsed, each
    /// paired with the raw document it came from. Migration classification
    /// needs the document beside the parsed struct: whether a record carries a
    /// declared set at all decides the legacy rule for its brand, and the
    /// struct cannot say. Empty before the first load, and empty after a load that fell back
    /// to the on-disk cache — that path has no wire document to classify
    /// against.
    [[nodiscard]] const std::unordered_map<int, LaneDataRecord>& last_lane_data_records() const {
        return lane_data_records_;
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
    // Backing store for last_lane_data_records(). Cleared at the top of every
    // load_blocking_impl() and populated only on the lane_data parse path, so
    // a cache-fallback load leaves it empty rather than stale.
    std::unordered_map<int, LaneDataRecord> lane_data_records_;
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
// Shared persist tail
// =============================================================================
//
// A backend that just staged an override record persists it with the same tail
// everywhere: snapshot the record out of the map under the lock, hand it to
// save_async, and report a failure under the backend's log tag. The callback
// captures nothing but its by-value arguments because it can fire long after
// the caller returns (the Moonraker tracker holds it ~60s), and it must not
// touch the backend: the backend may outlive its store, but the store
// outlives the scheduled save by design.

/// Schedule @p store's save of @p record for @p slot_index, warning with
/// "<tag> <noun> persist failed for slot <n>: <err>" when it fails. @p noun
/// names what was persisted ("Override", "weight", "lock release").
void save_override_async(FilamentSlotOverrideStore* store, int slot_index,
                         const FilamentSlotOverride& record, const std::string& tag,
                         const char* noun);

/// Snapshot @p overrides' record for @p slot_index under @p mutex and persist
/// it. A slot with no staged record saves nothing. This is the tail every
/// apply_user_edit() ends with, after stage_user_override() staged the edit.
void persist_staged_override(FilamentSlotOverrideStore* store, std::mutex& mutex,
                             std::unordered_map<int, FilamentSlotOverride>& overrides,
                             int slot_index, const std::string& tag, const char* noun);

// =============================================================================
// Shared construct-and-load
// =============================================================================
//
// Every backend that persists user slot identity holds the same pair: a store
// on its own namespace, and the map that store was loaded into. Building one
// without loading it is not a state any backend wants, so this returns both or
// neither and there is no way to spell the half-built version.

struct LoadedOverrideStore {
    /// Null when `api` was null (a backend constructed without a connection).
    std::unique_ptr<FilamentSlotOverrideStore> store;
    /// What the store held, keyed by global slot index. Empty when store is null.
    std::unordered_map<int, FilamentSlotOverride> overrides;
};

/// Build the store for `backend_id` on namespace `ns` and load it.
///
/// `type` picks the outer key style via lane_key_style_for(), so a backend
/// never restates that rule. `ns` defaults to the shared "lane_data"; a backend
/// whose own Klipper plugin owns that namespace (AFC, Happy Hare) MUST name a
/// private one, or the plugin's records load back as if the user authored them
/// and ours are deleted on the plugin's next boot.
///
/// Call this with NO lock held: the DB round-trip blocks for up to 5s and the
/// caller's status subscription may already be live, so holding the backend's
/// mutex across it stalls the parse path (and self-deadlocks a caller that
/// takes a non-recursive mutex before asking). Publish the result under the
/// lock afterwards, which is one move of each field.
[[nodiscard]] LoadedOverrideStore make_loaded_override_store(IMoonrakerAPI* api,
                                                             std::string backend_id, AmsType type,
                                                             const std::string& log_tag,
                                                             std::string ns = "lane_data");

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
//   - IFS: apply_user_edit writes to Adventurer5M.json — firmware re-reads it
//     and reports the user's chosen color on the next status poll. The mirror
//     can safely overwrite the override with firmware values (except a colour
//     or material the record declares, per #965; see MirrorPolicy::OverwriteAlways
//     below) because firmware-truth and user-truth converge.
//
//   - CFS: apply_user_edit does NOT touch the firmware-side material_type /
//     RFID values. If the mirror unconditionally overwrote ovr.color_rgb with
//     firmware-truth, every status poll would erase the user's color
//     override. So this backend uses FillUnsetOnly: only fill fields the
//     user hasn't explicitly set. clear_slot_override resets the entry,
//     after which auto-mirror takes over again.
enum class MirrorPolicy {
    /// Overwrite ovr.color_rgb / ovr.material with firmware values, EXCEPT for
    /// a colour or material the record declares (declares_color /
    /// declares_material, see #965). Use when user edits propagate back to
    /// firmware so the two views stay in sync (AD5X IFS, Snapmaker paxx12).
    /// A field the caller reports as declared on the lane is left alone the
    /// same way; see DeclaredOnLane.
    OverwriteAlways,
    /// Only fill ovr.color_rgb / ovr.material when they're currently UNSET
    /// (color_rgb == 0, empty material). Use when user edits don't reach
    /// firmware (CFS).
    FillUnsetOnly,
};

/// Which of colour and material a declaring lane source holds: the lane's
/// Spoolman record, or its LocalUser record, carrying a value for the field.
///
/// Neither reaches firmware, so firmware's reading does not stand for such a
/// field, and every mirror policy leaves it in the override exactly as it
/// leaves a field the record itself declares. Build one with declared_on_lane().
struct DeclaredOnLane {
    bool color = false;
    bool material = false;
};

/// What @p lane's declaring sources hold, read from the lane source store.
[[nodiscard]] DeclaredOnLane declared_on_lane(LaneId lane);

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
/// `declared` names the fields a declaring lane source holds; build it with
/// declared_on_lane(). It has no default, so no caller can pass "nothing
/// declared" by leaving it out.
///
/// Returns true iff `overrides[slot_index]` was actually mutated. Callers
/// (e.g. IFS) use this to drive secondary side-effects like _IFS_VARS sync.
bool mirror_firmware_to_lane_data(FilamentSlotOverrideStore* store,
                                  std::unordered_map<int, FilamentSlotOverride>& overrides,
                                  int slot_index, uint32_t firmware_color,
                                  const std::string& firmware_material, bool slot_has_filament,
                                  MirrorPolicy policy, const std::string& log_tag,
                                  DeclaredOnLane declared);

/// Discard a slot's stored override, in memory and on the printer.
///
/// The persisted half of a lane invalidation. Dropping a lane's declaring
/// sources settles what the UI paints now; without this the same record is
/// read back off the printer by ingest_legacy_records() at the next backend
/// start and the identity returns. `overrides` also feeds AFC's spool-id
/// re-assert, which would otherwise push an id for a spool that was swapped
/// away.
///
/// No-op when the slot has no entry, which is also what keeps this from
/// issuing a second DELETE for a record another path already cleared. Caller
/// MUST hold the backend's mutex protecting `overrides`; `store` may be null
/// (a test fixture with no Moonraker API), leaving the in-memory erase alone
/// to happen. `log_tag` attributes the warn on a failed persist.
///
/// Returns true iff an entry was erased.
bool clear_persisted_override(FilamentSlotOverrideStore* store,
                              std::unordered_map<int, FilamentSlotOverride>& overrides,
                              int slot_index, const std::string& log_tag);

/// Put a metered weight on a slot's stored override, in memory and on the
/// printer.
///
/// The record half of a weight persist: stage_weight_override() moves the
/// weights and nothing else, and the result is saved. Caller MUST hold the
/// backend's mutex protecting `overrides`; `store` may be null (a test fixture
/// with no Moonraker API), leaving the in-memory record alone to move.
/// `log_tag` attributes the warn on a failed persist.
void persist_override_weight(FilamentSlotOverrideStore* store,
                             std::unordered_map<int, FilamentSlotOverride>& overrides,
                             int slot_index, float remaining_weight_g, float total_weight_g,
                             const std::string& log_tag);

/// Amend a linked slot's stored record with what Spoolman now states.
///
/// Caller MUST hold the backend's mutex protecting `overrides`. Returns true
/// when a field moved, which is also when the record is saved.
bool persist_override_external_identity(FilamentSlotOverrideStore* store,
                                        std::unordered_map<int, FilamentSlotOverride>& overrides,
                                        int slot_index, const Observation& spoolman,
                                        const std::string& log_tag);

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
/// Beyond the plain baseline compare it carries an expectation set: a backend
/// that writes a value back to firmware (CFS's BOX_MODIFY_TN_DATA identity
/// push) registers the values it expects to see echoed, via expect_any_of().
/// Because the write is asynchronous, firmware keeps reporting the OLD value
/// for an unknown number of polls before the echo lands, so the expectation
/// must SURVIVE those polls rather than overwrite the baseline immediately.
/// Those intervening polls classify as Unchanged; the echo itself classifies
/// as OwnWriteEcho.
///
/// Each expectation is single-shot per value: an exact match consumes only its
/// own entry, and a change to a value no write asked for consumes them all, so
/// a genuine physical swap that lands while a write is in flight is still
/// reported as Changed and never permanently blinds swap detection for that
/// slot.
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

    /// Register every value the slot may report between the first and last
    /// echo of a multi-field write (the intermediate composites and the final
    /// one). Values accumulate with any still pending from an earlier write on
    /// the same slot: a second edit dispatched before the first echo lands
    /// leaves both echoes expected, so neither is misread as a swap when it
    /// arrives. Each value is consumed only by an exact match; any other
    /// change clears them all. Empty strings are dropped;
    /// forget_expected() is the explicit drop.
    ///
    /// Returns the values actually staged, for handing back to
    /// forget_expected() when the write they were armed for fails to
    /// dispatch. A value an earlier in-flight write also expects is staged
    /// as a second claim on one entry, so that write's echo stays expected
    /// after this one gives up.
    std::vector<std::string> expect_any_of(int slot_index,
                                           std::vector<std::string> expected_values);

    /// Drop the claims one expect_any_of() call staged - that write failed
    /// to dispatch, so its echo is never coming and the next change is
    /// genuinely external. Values another in-flight write also expects
    /// survive: each write holds its own claim.
    void forget_expected(int slot_index, const std::vector<std::string>& staged_values);

    /// Current baseline for a slot, or nullopt when none observed yet.
    [[nodiscard]] std::optional<std::string> baseline(int slot_index) const;

    /// Seed a baseline for a slot from a persisted fingerprint. No-op when the
    /// slot already has a baseline (a live observation always outranks a
    /// stored one) or the value is empty (a record without a fingerprint
    /// starts as a first-observation baseline, as it did before persistence
    /// existed).
    void seed_baseline(int slot_index, const std::string& value) {
        if (!value.empty())
            baseline_.try_emplace(slot_index, value);
    }

    /// Notified on every observation that establishes or confirms a baseline:
    /// Baseline, Unchanged and OwnWriteEcho. Not NoSignal (nothing was
    /// observed) and not Changed: the caller is about to erase the record the
    /// fingerprint travels in, and a save racing that erase would resurrect it.
    /// Unchanged firing is load-bearing: a record can come into existence
    /// after the Baseline event (the auto-mirror, a user edit), and the next
    /// confirming poll is what heals the fingerprint into it.
    ///
    /// The sink runs inline in observe(), under whatever lock the caller
    /// already holds there, so it may touch the caller's override map without
    /// taking its own.
    using BaselineSink = std::function<void(int, const std::string&)>;
    void set_baseline_sink(BaselineSink sink) {
        baseline_sink_ = std::move(sink);
    }

    /// Whether an unconsumed expectation is pending for a slot.
    [[nodiscard]] bool has_expected(int slot_index) const;

    void clear();

  private:
    std::unordered_map<int, std::string> baseline_;
    /// Pending expected values per slot, each paired with the number of
    /// in-flight writes that expect it: the intermediate and final composites
    /// expect_any_of() registered, claim-counted so one write's failed
    /// dispatch drops only its own claim.
    std::unordered_map<int, std::vector<std::pair<std::string, int>>> expected_;
    BaselineSink baseline_sink_;
};

/// Give a tracker's baselines the lifetime of the records they guard: seed
/// each slot's baseline from the loaded override's persisted fingerprint, and
/// install the sink that keeps that fingerprint current as the tracker
/// observes the slot.
///
/// The sink no-ops for a slot with no override entry (a fingerprint only
/// matters while there is a user edit for a swap to clear) and when the
/// record already carries the observed value, so steady-state polls save
/// nothing.
///
/// `overrides` must outlive the binding (the sink holds a reference to it);
/// a backend passes its own member, so that is its lifetime. Call with the
/// same lock discipline the backend uses around observe(): the seeding is
/// synchronous here, the sink later runs inline under that lock.
void bind_fingerprint_persistence(SlotFingerprintTracker& tracker, FilamentSlotOverrideStore* store,
                                  std::unordered_map<int, FilamentSlotOverride>& overrides);

} // namespace helix::ams
