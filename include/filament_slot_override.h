// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "hv/json.hpp"

// Forward declaration — full SlotInfo lives in ams_types.h. Forward-decl keeps
// this header light (it's pulled into many backends) and avoids a transitive
// pull-in of the much larger AMS type surface.
namespace helix {
struct SlotInfo;
} // namespace helix

namespace helix::ams {

struct FilamentSlotOverride;
struct Observation;
class DeclaredFields;
struct RecordAuthorship;
enum class LegacyLockKeys;

// The only three functions that may put a bit in a DeclaredFields. Each walks
// the field roster in lane_translation.cpp and admits only the rows that roster
// marks as keeping their authorship in the set. Declared here so the class
// below can befriend them.
[[nodiscard]] RecordAuthorship amend_authorship(const Observation& observed,
                                                const FilamentSlotOverride& prior,
                                                const FilamentSlotOverride& amended);
[[nodiscard]] DeclaredFields declared_fields_from_names(const nlohmann::json& names);
[[nodiscard]] DeclaredFields declared_fields_on_load(const nlohmann::json& wire,
                                                     LegacyLockKeys keys,
                                                     const FilamentSlotOverride& parsed);

// Which of a stored record's fields the user declared, one bit per row of the
// field roster in lane_translation.cpp.
//
// Authorship rides the roster's axis so a newly editable field needs no flag
// of its own: it gains a bit here by appearing on the roster, and the reader
// that routes it already walks that list. The bits are positional, but the
// wire is keyed by field NAME, so a stored record survives a reordering of the
// roster.
//
// It is the one home for every identity field's authorship, colour and
// material included. The helix_locked_color / helix_locked_material keys a
// stored record carries are written from it rather than kept beside it, since
// two homes for one concept drift. Only the three roster walks befriended below
// can set a bit, so no caller can claim a field for the user outside the
// roster's rules; withdrawing a declaration claims nothing, so reset() is open.
class DeclaredFields {
  public:
    /// Rows the bitmask can address. The roster static_asserts against it.
    static constexpr size_t kCapacity = 16;

    [[nodiscard]] bool test(size_t index) const {
        return index < kCapacity && (bits_ & (uint16_t{1} << index)) != 0;
    }
    [[nodiscard]] bool any() const {
        return bits_ != 0;
    }
    [[nodiscard]] bool operator==(const DeclaredFields& other) const {
        return bits_ == other.bits_;
    }
    /// Withdraw the declaration at roster position @p index.
    void reset(size_t index) {
        if (index < kCapacity) {
            bits_ &= static_cast<uint16_t>(~(uint16_t{1} << index));
        }
    }

  private:
    void set(size_t index) {
        if (index < kCapacity) {
            bits_ |= static_cast<uint16_t>(uint16_t{1} << index);
        }
    }

    friend RecordAuthorship amend_authorship(const Observation&, const FilamentSlotOverride&,
                                             const FilamentSlotOverride&);
    friend DeclaredFields declared_fields_from_names(const nlohmann::json&);
    friend DeclaredFields declared_fields_on_load(const nlohmann::json&, LegacyLockKeys,
                                                  const FilamentSlotOverride&);

    uint16_t bits_ = 0;
};

/// Everything a record says about who authored its fields.
struct RecordAuthorship {
    DeclaredFields declared;
};

struct FilamentSlotOverride {
    // User metadata
    std::string brand;
    std::string spool_name;
    int spoolman_id = 0;
    std::string spoolman_orca_filament_id; // Explicit Orca preset id of the linked spool
    int spoolman_filament_id = 0;
    int spoolman_vendor_id = 0;
    float remaining_weight_g = -1.0f;
    float total_weight_g = -1.0f;
    // Hardware-truth fields, override-wins.
    //
    // color_rgb stores the 24-bit RGB color (high byte unused). `color_set`
    // is the explicit "this slot has a color recorded" signal — pure black
    // (#000000) is a legitimate value (firmware reports loaded black PLA as
    // 0x000000), so we cannot use color_rgb == 0 as an "unset" sentinel.
    // Always check color_set before applying / emitting the color; when
    // false, color_rgb is undefined and must be ignored.
    //
    // Lifecycle: starts false; flipped to true by apply_user_edit (any user
    // edit, even setting black), by auto-mirror when firmware fills it, and
    // by from_lane_data_record / from_json when the on-disk record contains
    // a color. clear_slot_override erases the whole entry; nothing else
    // resets color_set to false.
    uint32_t color_rgb = 0;
    bool color_set = false;
    std::string color_name;
    std::string material;
    // Catalog product identity, mirroring SlotInfo::catalog_id / product_name
    // (see ams_types.h for why both are stored rather than one derived from the
    // other). `material` alone cannot tell SUNLU "PLA+ 2.0" from SUNLU "PLA
    // Marble", so without these the editor re-opens on whichever variant sorts
    // first and silently relabels the lane.
    //
    // Persistence: emitted as `helix_catalog_id` / `helix_product_name` in the
    // lane_data record. The helix_ prefix is required there — lane_data is a
    // SHARED Moonraker namespace (AFC, Happy Hare, Mainsail and Orca all read
    // and write it), same rule as helix_material / helix_locked_*. Both keys are
    // omitted when empty so a lane with no catalog pick does not squat names in
    // that namespace. The local cache (to_json / from_json) is HelixScreen-
    // private and uses the bare names, matching user_locked_color.
    //
    // Auto-mirror never populates these: firmware has no concept of a catalog
    // product, so a non-empty value always means a user pick.
    std::string catalog_id;
    std::string product_name;
    // Authorship for the identity fields: colour, material, brand, spool name
    // and Spoolman vendor id. A field in this set was the user's word, so the
    // reader files it as a declaration rather than as something the store
    // merely remembered, and a later firmware frame stating the same field
    // does not displace it. The auto-mirror policies leave a declared colour or
    // material alone for the same reason, so a stale firmware report cannot
    // silently overwrite the user's choice (#965). Auto-mirror writes declare
    // nothing, and clear_slot_override erases the whole entry.
    //
    // Persistence: `helix_declared` in the lane_data record, `declared` in the
    // local cache, both an array of field names. Emitted even when empty, so a
    // reader can tell "this record declares nothing" from "this record predates
    // the key" and apply the legacy rule only to the latter. The colour and
    // material bits are also written as `helix_locked_color` /
    // `helix_locked_material` (bare `user_locked_*` in the local cache) for a
    // reader that predates the set; declared_fields_on_load() is the rule that
    // reads them back.
    DeclaredFields declared;
    // Recommended print temperatures, written into the lane_data record so
    // OrcaSlicer 2.3.2+ can sync them onto the filament preset. Source order
    // (highest to lowest priority): explicit user entry > Spoolman spool's
    // filament profile > internal material database default. The first two
    // land here via populate_temps_from_slot_info() from the backend's
    // apply_user_edit; the material-DB fallback is applied at *emit* time via
    // resolved_temps() so a later material change always picks up fresh
    // defaults instead of carrying stale values forward. 0 = unset.
    int bed_temp = 0;
    int nozzle_temp = 0;
    // The slot identity (RFID UID, composite material/brand/name/color key)
    // the SlotFingerprintTracker last observed on this lane. Bookkeeping, not
    // user data: it exists so a restart can tell "same spool" from "swapped
    // while the app was down" instead of treating the first observation as a
    // baseline. Written by bind_fingerprint_persistence's sink, carried
    // forward by user edits (user_override_from_slot_info), erased with the
    // rest of the entry on a swap clear. Empty on records that pre-date the
    // field, which read back as first-observation baselines.
    // Persistence: `helix_fingerprint` in the lane_data record (the shared
    // namespace demands the prefix, and the key is omitted when empty so a
    // lane without swap detection squats no names), bare `fingerprint` in the
    // local cache.
    std::string fingerprint;
    // This record is the external-spool mirror published for slicers, not a
    // bay's record. The publisher writes or clears a key only when it is empty
    // or carries this mark, so a bay that shares the key keeps its record.
    // Persistence: `helix_external: true` in the lane_data record, omitted
    // otherwise. Not in the local cache: the mirror is republished from
    // settings, never read back as a bay.
    bool external_mirror = false;
    // The firmware spool id the user unlinked from this lane while firmware
    // kept stating it (AFC lanes with remember_spool, #1717). A frame naming
    // this id is the stale restatement and reads as no link; any other id, a
    // link made here or (unless the lane remembers its spool) the lane
    // emptying ends it. A re-link to the same id made outside HelixScreen
    // stays hidden: it is indistinguishable from the restatement.
    // Bookkeeping, not identity: a Clear Spool keeps it and an edit carries
    // it forward unless it links.
    // Persistence: `helix_unlinked_spool_id` in the lane_data record, omitted
    // when 0, and `unlinked_spool_id` in the local cache.
    int unlinked_spool_id = 0;
    // Conflict avoidance for third-party writers.
    // ISO-8601 UTC on the wire. Second precision only — sub-second fractions
    // are truncated on format/parse.
    std::chrono::system_clock::time_point updated_at{};
};

// Effective (bed_temp, nozzle_temp) for an override. The struct stores
// *intent* (0 = "use the material's default"); resolved_temps() is the
// canonical accessor that materializes the effective values: explicit
// non-zero fields pass through unchanged, and 0 fields fall back to the
// internal material-database recommendation when `material` is set. Use
// this anywhere downstream code wants the "what would actually get
// printed at?" answer — including external-facing emits like
// to_lane_data_record (the OrcaSlicer-visible Moonraker DB record).
//
// Crucially, the fallback is *not* baked into the struct on read or write:
// if the user changes material PLA → PETG, the next call to resolved_temps()
// picks up PETG's defaults automatically. Storing the resolved values would
// freeze stale defaults and is a known anti-pattern here.
//
// The local cache (to_json / from_json) intentionally round-trips the *intent*
// values, not resolved values, so the round-trip preserves the "0 = default"
// signal across reboots. Anyone reading from cache who wants effective values
// must call resolved_temps().
struct ResolvedTemps {
    int bed_temp = 0;
    int nozzle_temp = 0;
};
ResolvedTemps resolved_temps(const FilamentSlotOverride& o);

// Populate the override's temp fields from a SlotInfo carrying user/Spoolman
// values. Called from each backend's apply_user_edit to centralize the SlotInfo
// → FilamentSlotOverride temp wiring (previously this was an 11-line block
// duplicated across all four AMS backends). nozzle_temp is the midpoint of
// nozzle_temp_min/max when both differ, else nozzle_temp_min when set, else
// 0 (which signals to resolved_temps that the material-DB default should win).
void populate_temps_from_slot_info(FilamentSlotOverride& ovr, const SlotInfo& info);

// The record a backend persists for a user's edit that left the lane at
// @p edited, where @p declaration is what the user declared in it. One shape for
// every AMS backend, so the rules a backend must not get wrong live here rather
// than in seven near-identical blocks.
//
// The record carries every identity field @p edited holds, because that is what
// the lane must show. Which of them the record claims as the USER'S word is a
// narrower question, and @p declaration answers it: a field is the user's
// exactly when they moved it, which user_edit_observation() works out from the
// editor's two snapshots. The editor seeds its working copy from the lane's
// current state, so a firmware-sourced brand, colour or material arrives in
// @p edited untouched, and a record claiming those would outrank the firmware
// that supplied them and refuse every later correction (#965).
//
// Authorship therefore lands in the declared set, from that one answer. A
// colour or material declaration needs a value to stand over, so an empty
// material is never declared however the edit moved it: every mirror policy
// leaves a declared field alone, and a declaration over nothing would stop
// firmware from ever filling that lane.
//
// @p prior is the record this lane already had, or nullptr for a lane with
// none. One edit speaks only about the fields it moved, so this edit's
// authorship is AMENDED onto that record's rather than replacing it: an
// earlier choice the edit never mentioned stays the user's word while the
// value it stood over is still the one the record holds. Without it a brand-
// only edit would drop a colour declared before it. An edit that changes the
// binding is the exception: a different spool is on the lane, so no earlier
// choice stands and the record declares only what that edit did.
// amend_authorship() (lane_translation.h) is that rule.
//
// `material` is recorded in place of edited.material, for a backend that
// persists firmware's normalized spelling rather than the string the user
// typed: AD5X stores the firmware-valid value its own normalize_material()
// produced, so the raw SlotInfo string is the wrong thing to record and the
// wrong thing to declare. Whether the material was declared still follows
// @p declaration, since the normalized spelling has no before-value to compare
// against. A record with a spool id records edited.material whatever `material`
// says: a linked spool owns its material, so the record keeps the spool's
// spelling while firmware receives the backend's own.
//
// The colour records only when it is a reading rather than the SlotInfo "no
// colour" sentinel, the question is_declarable_color() answers; a deliberate
// pure black (#000000) is a reading and records. color_name travels
// regardless, because it is the user's own text either way.
//
// Temps come from populate_temps_from_slot_info(). updated_at is left default
// so save_async stamps a fresh value.
FilamentSlotOverride user_override_from_slot_info(const Observation& declaration,
                                                  const SlotInfo& edited,
                                                  const std::string& material,
                                                  const FilamentSlotOverride* prior);

// As above, declaring what user_edit_observation(original, edited) answers and
// recording edited.material.
FilamentSlotOverride user_override_from_slot_info(const SlotInfo& original, const SlotInfo& edited,
                                                  const FilamentSlotOverride* prior);

// As above, recording `material` in place of edited.material.
FilamentSlotOverride user_override_from_slot_info(const SlotInfo& original, const SlotInfo& edited,
                                                  const std::string& material,
                                                  const FilamentSlotOverride* prior);

// Build the record for a user's edit and put it in @p overrides under @p
// slot_index, amending whatever that lane already held there. Returns the
// staged record.
//
// Every backend stages a user's edit into a map of exactly this shape, and the
// prior record it must amend is the entry this call is about to replace, so
// finding it belongs here rather than in seven places that would each have to
// remember to look.
//
// What the user declared is @p declared, which AmsBackend::commit_user_edit()
// answers once from the editor's own snapshot.
FilamentSlotOverride& stage_user_override(std::unordered_map<int, FilamentSlotOverride>& overrides,
                                          int slot_index, const SlotInfo& edited,
                                          const Observation& declared);

// As above, for a backend that records a normalized material spelling.
FilamentSlotOverride& stage_user_override(std::unordered_map<int, FilamentSlotOverride>& overrides,
                                          int slot_index, const SlotInfo& edited,
                                          const std::string& material, const Observation& declared);

// Put a metered weight on the record @p overrides holds for @p slot_index and
// return it. Only the two weights move: a meter states no identity, so the
// record's values and its authorship stand as they were. A lane with no record
// yet gets one carrying the weight alone, and a total below zero leaves the
// record's total as it was.
FilamentSlotOverride&
stage_weight_override(std::unordered_map<int, FilamentSlotOverride>& overrides, int slot_index,
                      float remaining_weight_g, float total_weight_g);

nlohmann::json to_json(const FilamentSlotOverride& o);
FilamentSlotOverride from_json(const nlohmann::json& j);

} // namespace helix::ams
