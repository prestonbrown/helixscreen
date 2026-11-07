// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "filament_slot_override.h"
#include "lane_source_store.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "hv/json.hpp"

namespace helix {
class IMoonrakerAPI;
}
class FilamentSlotOverrideStoreTestAccess;

namespace helix::ams {

// Outer Moonraker DB key convention for per-slot lane_data records. The two
// styles carry the SAME 0-based inner "lane" field but different outer keys:
//   - Lane: "laneN" (1-based, AFC/Happy Hare convention) — filament systems.
//   - Tool: "T<n>"  (0-based, Orca/Mainsail tool convention) — tool changers.
// A tool changer converging on the T<n> key style makes HelixScreen writes
// overwrite Mainsail #2510's records instead of duplicating them. See
// docs/specs/filament_slots.md § "Interoperating readers and writers".
enum class LaneKeyStyle { Lane, Tool };

// Maps an AmsType to its lane_data key style: tool changers use T<n> keys,
// every filament-switching system laneN.
inline LaneKeyStyle lane_key_style_for(AmsType t) {
    return is_tool_changer(t) ? LaneKeyStyle::Tool : LaneKeyStyle::Lane;
}

/// The Lane Status Surface version and writer name HelixScreen's records carry
/// (RFC 0001 §5).
inline constexpr const char* kLaneDataLssVersion = "0.1";
inline constexpr const char* kLaneDataWriter = "helixscreen";

/// The namespace every lane_data writer shares. A store on any other name holds
/// HelixScreen's extras for a backend whose firmware owns lane_data.
inline constexpr const char* kLaneDataNamespace = "lane_data";

/// The RFC 0001 §5 record HelixScreen writes for @p slot_index. Every RFC key
/// is present; an unknown value is null, never a placeholder. Keys only
/// HelixScreen reads carry the helix_ prefix.
[[nodiscard]] nlohmann::json to_lane_data_record(int slot_index, const FilamentSlotOverride& o,
                                                 LaneKeyStyle style = LaneKeyStyle::Lane);

/// Parse a lane record, ours or another writer's, back into its slot index and
/// fields. Reads the RFC spellings first and the legacy ones (`vendor`,
/// `spool_name`, `helix_material`) after. nullopt for a record with no valid
/// "lane" field.
[[nodiscard]] std::optional<std::pair<int, FilamentSlotOverride>>
from_lane_data_record(const nlohmann::json& j);

/// The record an extras namespace stores: only what the firmware cannot hold
/// (brand, name, colour name, catalog pick, total weight) and HelixScreen's
/// bookkeeping. A spool id, material or colour in it is ignored on read and
/// never written: the firmware's value is the lane's.
[[nodiscard]] FilamentSlotOverride extras_only(const FilamentSlotOverride& o);

/// True when @p wire names a writer other than HelixScreen (RFC 0001 S2). A
/// record with no writer is legacy and replaceable.
[[nodiscard]] bool written_by_another_producer(const nlohmann::json& wire);

/// One parsed record plus the JSON it came from.
struct LaneDataRecord {
    FilamentSlotOverride record;
    nlohmann::json wire;
};

/// Parse a whole namespace document, one record per slot. Records are matched
/// by their inner `lane` field, never the key (RFC 0001 S3); when two records
/// name one slot, the one under our canonical key wins. Non-object siblings
/// (`seated`) are skipped, and a record that does not parse costs its slot
/// only.
[[nodiscard]] std::unordered_map<int, LaneDataRecord>
parse_namespace_document(const nlohmann::json& namespace_doc, LaneKeyStyle key_style,
                         const std::string& log_tag = "shared");

/// Reads and writes one backend's lane records in Moonraker's database. There
/// is no local copy: what the last read returned is all it holds, and a write
/// that fails is reported, never queued.
class FilamentSlotOverrideStore {
  public:
    FilamentSlotOverrideStore(IMoonrakerAPI* api, std::string backend_id,
                              LaneKeyStyle key_style = LaneKeyStyle::Lane,
                              std::string ns = kLaneDataNamespace);

    [[nodiscard]] const std::string& namespace_name() const {
        return namespace_;
    }

    /// True on an extras namespace (AFC, Happy Hare): records hold only what
    /// extras_only() keeps.
    [[nodiscard]] bool holds_extras_only() const {
        return namespace_ != kLaneDataNamespace;
    }

    /// One database get for the whole namespace. @p cb receives the parsed
    /// records (narrowed by extras_only() on an extras namespace) on the
    /// requesting thread's callback; an absent namespace is an empty read. A
    /// failed read calls @p on_failure. Callbacks capture nothing of the store.
    using ReloadCallback = std::function<void(std::unordered_map<int, LaneDataRecord>)>;
    void reload_async(ReloadCallback cb, std::function<void()> on_failure = nullptr);

    /// Called, on whatever thread the reply lands on, after each write or clear
    /// the database accepted: the owner re-reads so the lane shows the echo.
    void set_on_written(std::function<void()> on_written) {
        on_written_ = std::move(on_written);
    }

    /// Remember what a read returned, for the writer rule. Main thread.
    void adopt_read(const std::unordered_map<int, LaneDataRecord>& records);

    using SaveCallback = std::function<void(bool success, std::string error)>;

    /// Write @p override as slot @p slot_index's record. Refused, with an
    /// error, when the record last read for the slot names another writer.
    void save_async(int slot_index, const FilamentSlotOverride& override, SaveCallback cb);

    /// Delete slot @p slot_index's record, under the same writer rule.
    void clear_async(int slot_index, SaveCallback cb);

    // The "seated" sibling key: the 0-based index of the lane loaded to the
    // toolhead (AD5X IFS). A plain integer, not a lane record.
    void save_seated_slot_async(int slot_index, SaveCallback cb);
    void clear_seated_slot_async(SaveCallback cb);
    std::optional<int> load_seated_slot_blocking();

    const std::string& backend_id() const {
        return backend_id_;
    }

  private:
    friend class ::FilamentSlotOverrideStoreTestAccess;

    /// Why a write to @p slot_index is refused, or empty when it may go.
    [[nodiscard]] std::string writer_refusal(int slot_index) const;

    IMoonrakerAPI* api_;
    std::string backend_id_;
    LaneKeyStyle key_style_;
    std::string namespace_;
    std::chrono::milliseconds load_timeout_{5000};

    std::function<void()> on_written_;

    mutable std::mutex mutex_;
    /// The writer of each slot's record at the last read.
    std::unordered_map<int, std::string> writers_;
};

// =============================================================================
// Shared persist tail
// =============================================================================

/// Schedule @p store's save of @p record for @p slot_index. A failure is
/// reported to the user (a toast naming @p noun) and withdraws @p lane's
/// pending edit, so the lane falls back to what Moonraker holds.
void save_override_async(FilamentSlotOverrideStore* store, int slot_index,
                         const FilamentSlotOverride& record, const std::string& tag,
                         const char* noun, LaneId lane = kInvalidLaneId);

/// Snapshot @p overrides' record for @p slot_index under @p mutex and persist
/// it through save_override_async(). A slot with no staged record saves
/// nothing.
void persist_staged_override(FilamentSlotOverrideStore* store, std::mutex& mutex,
                             std::unordered_map<int, FilamentSlotOverride>& overrides,
                             int slot_index, const std::string& tag, const char* noun,
                             LaneId lane = kInvalidLaneId);

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
//     can safely overwrite the override with firmware values because
//     firmware-truth and user-truth converge.
//
//   - CFS: apply_user_edit does NOT touch the firmware-side material_type /
//     RFID values. If the mirror unconditionally overwrote ovr.color_rgb with
//     firmware-truth, every status poll would erase the user's color
//     override. So this backend uses FillUnsetOnly: only fill fields the
//     user hasn't explicitly set. clear_slot_override resets the entry,
//     after which auto-mirror takes over again.
enum class MirrorPolicy {
    /// Overwrite ovr.color_rgb / ovr.material with firmware values. Use when
    /// user edits propagate back to firmware so the two views stay in sync
    /// (AD5X IFS, Snapmaker paxx12).
    OverwriteAlways,
    /// Only fill ovr.color_rgb / ovr.material when they're currently UNSET.
    /// Use when user edits don't reach firmware (CFS).
    FillUnsetOnly,
};

/// Mirror firmware-detected color/material into `overrides[slot_index]` and
/// save the resulting record. Caller MUST hold the backend's mutex protecting
/// `overrides`. No-op (false) for an empty slot or when the policy leaves
/// nothing to change. Pure black is a colour, not "no signal". `store` may be
/// null, leaving only the in-memory record to move. Returns true iff the
/// record changed.
bool mirror_firmware_to_lane_data(FilamentSlotOverrideStore* store,
                                  std::unordered_map<int, FilamentSlotOverride>& overrides,
                                  int slot_index, uint32_t firmware_color,
                                  const std::string& firmware_material, bool slot_has_filament,
                                  MirrorPolicy policy, const std::string& log_tag);

/// Discard a slot's stored record, in memory and on the printer. No-op when
/// the slot has no entry. Caller MUST hold the backend's mutex protecting
/// `overrides`. Returns true iff an entry was erased.
bool clear_persisted_override(FilamentSlotOverrideStore* store,
                              std::unordered_map<int, FilamentSlotOverride>& overrides,
                              int slot_index, const std::string& log_tag);

/// Put a metered weight on a slot's stored record, in memory and on the
/// printer. Caller MUST hold the backend's mutex protecting `overrides`.
void persist_override_weight(FilamentSlotOverrideStore* store,
                             std::unordered_map<int, FilamentSlotOverride>& overrides,
                             int slot_index, float remaining_weight_g, float total_weight_g,
                             const std::string& log_tag);

/// Amend a linked slot's stored record with what Spoolman now states for the
/// spool it links. Caller MUST hold the backend's mutex protecting
/// `overrides`. Returns true when a field moved, which is also when the record
/// is saved.
bool persist_override_external_identity(FilamentSlotOverrideStore* store,
                                        std::unordered_map<int, FilamentSlotOverride>& overrides,
                                        int slot_index, const Observation& spoolman,
                                        const std::string& log_tag);

/// Publish (or clear) the external / bypass spool as an extra lane one past
/// the last physical slot, so slicers can select it as the "next tool over".
/// `spool` null or identity-less CLEARS the lane instead of publishing an
/// empty phantom. Returns true if a record was published.
bool publish_external_lane(FilamentSlotOverrideStore* store, int lane_index, const SlotInfo* spool,
                           const std::string& log_tag);

/// The marked lane_data record publish_external_lane() writes for @p spool.
[[nodiscard]] FilamentSlotOverride external_lane_record(const SlotInfo& spool);

/// Whether unmarked @p record is an external-spool mirror written before the
/// `helix_external` mark existed: it names @p spool. The same Spoolman id when
/// either side has one; otherwise the same material and colour, and the same
/// brand when both carry one.
[[nodiscard]] bool record_describes_spool(const FilamentSlotOverride& record,
                                          const SlotInfo& spool);

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

    /// Drop a slot's baseline, so its next observation is a first one. Pending
    /// expectations are kept.
    void forget_baseline(int slot_index) {
        baseline_.erase(slot_index);
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
