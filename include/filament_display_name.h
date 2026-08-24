// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

#include <string>
#include <string_view>

/**
 * @file filament_display_name.h
 * @brief Pure resolver for the filament label shown on AMS cards
 *
 * No LVGL, no I/O, no singletons. Everything the resolver needs is passed in,
 * which is what makes the precedence and deduplication rules testable in
 * isolation (`tests/unit/test_filament_display_name.cpp`).
 *
 * The algorithmic color name (`helix::get_color_name_from_hex()`) is the last
 * naming layer, but it is **not** called from here: `ui_color_picker.h` pulls in
 * `ui_modal.h` and `subject_managed_panel.h`, and dragging LVGL into this
 * translation unit would defeat the point. The caller computes it and passes it
 * as `color_fallback`.
 */

namespace helix {

/**
 * @brief Immutable identity of a Spoolman spool, cached by spool id
 *
 * The side-channel record `SpoolmanManager` keeps per `spool_id`, deliberately
 * kept out of `SlotInfo` so Spoolman never becomes a third writer into the
 * firmware-vs-override merge (see `docs/devel/FILAMENT_SLOT_METADATA.md` §5).
 *
 * Identity only — weights and other mutable spool state stay on `SlotInfo` and
 * keep their own refresh cadence.
 *
 * Lives here rather than in `spoolman_types.h` because it is the resolver's
 * input contract: this header stays free of the wider Spoolman type surface,
 * and the cache owner needs exactly one lightweight include to produce it.
 */
struct SpoolIdentity {
    std::string vendor;        ///< Vendor name (e.g. "Polymaker"); empty = unknown
    std::string filament_name; ///< Filament name (e.g. "PolyTerra Ambrosia Pink")
    std::string material;      ///< Material type (e.g. "PLA"); empty = unknown
    std::string color_hex;     ///< Hex color as Spoolman reports it (e.g. "#FFB6C1")
    int filament_id = 0;       ///< Spoolman filament definition id (0 = unknown)
    int vendor_id = 0;         ///< Spoolman vendor id (0 = unknown)

    /**
     * @brief True when this record carries at least one field a label can use
     *
     * Ids and a color hex alone cannot name anything, so a record holding only
     * those is treated exactly like a cache miss.
     */
    [[nodiscard]] bool valid() const {
        return !vendor.empty() || !filament_name.empty() || !material.empty();
    }
};

/**
 * @brief Join a brand, a name and a material into one label without repeating
 *
 * Deduplication, because Spoolman naming is inconsistent between users
 * ("Ambrosia Pink" vs "Polymaker PolyTerra Ambrosia Pink PLA"):
 * - the brand is dropped when the name already contains it,
 * - the material is dropped when the name already contains it,
 * - whitespace runs collapse and the result is trimmed.
 *
 * Matching is case-insensitive and anchored on word boundaries, so material
 * "PLA" is not stripped from "Elegoo HIPLA" or "Ambrosia Pink PLA+", and brand
 * "Poly" is not stripped from "Polymaker PolyTerra". Alphanumerics plus
 * `+ - _ #` count as word characters — those are the characters real material
 * names glue on ("PLA+", "PA6-CF").
 *
 * @param brand    Vendor/brand, may be empty
 * @param name     Filament or spool name, may be empty
 * @param material Material type, may be empty
 * @return Joined label; **empty** when all three inputs are blank. The
 *         never-empty guarantee belongs to resolve_filament_label().
 */
std::string compose_filament_label(std::string_view brand, std::string_view name,
                                   std::string_view material);

/**
 * @brief The three naming fields, resolved but not yet joined
 *
 * For callers that print the material somewhere else already. The Active Spool
 * home widget has a material row of its own, so joining the material back into
 * its brand line would print it twice.
 *
 * Blank means the field resolved to nothing, which is the caller's cue to hide
 * whatever renders it. There is no never-empty guarantee here — that belongs to
 * resolve_filament_label(), which composes these.
 */
struct FilamentLabelParts {
    std::string brand;    ///< `slot.brand` → `identity->vendor`
    std::string name;     ///< `slot.spool_name` → `identity->filament_name` →
                          ///< `slot.color_name` → `color_fallback`
    std::string material; ///< `slot.material` → `identity->material`
};

/**
 * @brief Resolve the naming fields for a slot without joining them
 *
 * The single implementation of the precedence rules; resolve_filament_label()
 * is this plus compose_filament_label() plus the never-empty fallbacks. Callers
 * that want the whole label want that one instead.
 *
 * @param slot           Slot to label
 * @param identity       Cached Spoolman identity, or nullptr on a cache miss.
 *                       A non-null record that is not `valid()` is a miss.
 * @param color_fallback Algorithmic color name, normally
 *                       `helix::get_color_name_from_hex(slot.color_rgb)`
 * @return Resolved fields; any of them may be empty.
 */
FilamentLabelParts resolve_filament_label_parts(const SlotInfo& slot, const SpoolIdentity* identity,
                                                std::string_view color_fallback);

/**
 * @brief Resolve the label shown for a slot
 *
 * Precedence, per field rather than per layer. `apply_overrides()` has already
 * merged user overrides onto `SlotInfo` by the time this runs, so "override"
 * and "firmware" are indistinguishable here and form one layer — but that layer
 * still wins field by field, with the Spoolman identity filling only the gaps.
 * Blank is the unset sentinel for every string field, matching the merge policy
 * in `docs/devel/FILAMENT_SLOT_METADATA.md` §5.
 *
 * - brand:    `slot.brand` → `identity->vendor`
 * - material: `slot.material` → `identity->material`
 * - name:     `slot.spool_name` → `identity->filament_name` → `slot.color_name`
 *             → `color_fallback`
 *
 * The three parts are then joined by compose_filament_label().
 *
 * @param slot           Slot to label
 * @param identity       Cached Spoolman identity, or nullptr on a cache miss /
 *                       Spoolman down / no spool id. A non-null record that is
 *                       not `valid()` is treated as a miss.
 * @param color_fallback Algorithmic color name for the slot's color, normally
 *                       `helix::get_color_name_from_hex(slot.color_rgb)`. May be
 *                       empty to suppress the layer.
 * @param last_resort    Emitted when nothing else resolves. Callers with a
 *                       translated string (the bypass card's "External") pass it
 *                       here.
 * @return Never empty — falls back to `last_resort`, then to "Filament".
 */
std::string resolve_filament_label(const SlotInfo& slot, const SpoolIdentity* identity,
                                   std::string_view color_fallback,
                                   std::string_view last_resort = "Filament");

} // namespace helix
