// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_display_name.cpp
 * @brief Unit tests for the pure filament display-label resolver
 *
 * The resolver decides what the AMS "current loaded" card reads. It is pure —
 * no LVGL, no I/O, no singletons — so every precedence layer and every dedup
 * rule is exercised directly here.
 */

#include "filament_display_name.h"
#include "spoolman_types.h" // apply_spool_to_slot — the picker-side writer

#include "../catch_amalgamated.hpp"

using helix::compose_filament_label;
using helix::resolve_filament_label;
using helix::SpoolIdentity;

namespace {

/// A slot as AFC reports it: name + material present, brand blocked upstream.
SlotInfo make_afc_slot() {
    SlotInfo slot;
    slot.slot_index = 0;
    slot.status = SlotStatus::LOADED;
    slot.spool_name = "Ambrosia Pink";
    slot.color_name = ""; // never populated on the AFC parse path
    slot.material = "PLA";
    slot.spoolman_id = 42;
    slot.color_rgb = 0xFFB6C1; // describe_color() calls this "Light Pink"
    return slot;
}

} // namespace

// ============================================================================
// compose_filament_label — joining
// ============================================================================

TEST_CASE("compose_filament_label: joins brand, name and material", "[filament][label]") {
    SECTION("bare name") {
        REQUIRE(compose_filament_label("", "Ambrosia Pink", "") == "Ambrosia Pink");
    }

    SECTION("name plus material") {
        REQUIRE(compose_filament_label("", "Ambrosia Pink", "PLA") == "Ambrosia Pink PLA");
    }

    SECTION("brand plus name plus material") {
        REQUIRE(compose_filament_label("Polymaker", "Ambrosia Pink", "PLA") ==
                "Polymaker Ambrosia Pink PLA");
    }

    SECTION("brand plus material with no name") {
        REQUIRE(compose_filament_label("Polymaker", "", "PLA") == "Polymaker PLA");
    }

    SECTION("material only") {
        REQUIRE(compose_filament_label("", "", "PETG") == "PETG");
    }

    SECTION("brand only") {
        REQUIRE(compose_filament_label("eSUN", "", "") == "eSUN");
    }
}

// ============================================================================
// compose_filament_label — brand deduplication
// ============================================================================

TEST_CASE("compose_filament_label: brand already in the name is not repeated",
          "[filament][label]") {
    SECTION("name begins with the brand") {
        REQUIRE(compose_filament_label("Polymaker", "Polymaker PolyTerra Ambrosia Pink", "PLA") ==
                "Polymaker PolyTerra Ambrosia Pink PLA");
    }

    SECTION("name is exactly the brand") {
        REQUIRE(compose_filament_label("Polymaker", "Polymaker", "") == "Polymaker");
    }

    SECTION("brand appears mid-name") {
        REQUIRE(compose_filament_label("Polymaker", "PolyTerra by Polymaker", "") ==
                "PolyTerra by Polymaker");
    }

    SECTION("brand upper, name mixed") {
        REQUIRE(compose_filament_label("POLYMAKER", "Polymaker PolyTerra", "") ==
                "Polymaker PolyTerra");
    }

    SECTION("brand lower, name upper") {
        REQUIRE(compose_filament_label("polymaker", "POLYMAKER POLYTERRA", "") ==
                "POLYMAKER POLYTERRA");
    }

    SECTION("a different brand is still prefixed") {
        REQUIRE(compose_filament_label("eSUN", "PolyTerra Ambrosia Pink", "PLA") ==
                "eSUN PolyTerra Ambrosia Pink PLA");
    }
}

// ============================================================================
// compose_filament_label — material deduplication
// ============================================================================

TEST_CASE("compose_filament_label: material already in the name is not repeated",
          "[filament][label]") {
    SECTION("name ends with the material") {
        REQUIRE(compose_filament_label("", "Ambrosia Pink PLA", "PLA") == "Ambrosia Pink PLA");
    }

    SECTION("name is exactly the material") {
        REQUIRE(compose_filament_label("", "PLA", "PLA") == "PLA");
    }

    SECTION("material appears mid-name") {
        REQUIRE(compose_filament_label("", "Silk PLA Red", "PLA") == "Silk PLA Red");
    }

    SECTION("case-insensitive both directions") {
        REQUIRE(compose_filament_label("", "Ambrosia Pink pla", "PLA") == "Ambrosia Pink pla");
        REQUIRE(compose_filament_label("", "Ambrosia Pink PLA", "pla") == "Ambrosia Pink PLA");
    }

    SECTION("a different material is still appended") {
        REQUIRE(compose_filament_label("", "Ambrosia Pink PLA", "PETG") ==
                "Ambrosia Pink PLA PETG");
    }

    SECTION("brand and material both already present") {
        REQUIRE(compose_filament_label("Polymaker", "Polymaker PolyTerra Ambrosia Pink PLA",
                                       "PLA") == "Polymaker PolyTerra Ambrosia Pink PLA");
    }

    SECTION("neither brand nor material present") {
        REQUIRE(compose_filament_label("eSUN", "Ambrosia Pink", "PETG") ==
                "eSUN Ambrosia Pink PETG");
    }
}

// ============================================================================
// compose_filament_label — word-boundary traps
//
// These are the partial-word false positives a bare substring search produces.
// Each assertion here fails if the boundary check is downgraded to find().
// ============================================================================

TEST_CASE("compose_filament_label: a material the name already states is dropped",
          "[filament][label]") {
    // Materials are a closed vocabulary, so containment really is redundancy —
    // "ePLA", "PLA+", "HIPLA" and "ABS+" all mean "this is that material", and
    // appending the bare material after one of them reads like nothing a human
    // would write. Brands get word-boundary matching instead (see below).

    SECTION("a trade name ending in the polymer absorbs it") {
        REQUIRE(compose_filament_label("", "Elegoo HIPLA", "PLA") == "Elegoo HIPLA");
        REQUIRE(compose_filament_label("eSUN", "Silk Blue ePLA", "Silk PLA") ==
                "eSUN Silk Blue ePLA");
    }

    SECTION("a graded name absorbs the base polymer") {
        REQUIRE(compose_filament_label("", "Ambrosia Pink PLA+", "PLA") == "Ambrosia Pink PLA+");
        REQUIRE(compose_filament_label("", "PLA+ Matte", "PLA") == "PLA+ Matte");
        REQUIRE(compose_filament_label("eSUN", "Fire Engine Red ABS+", "ABS") ==
                "eSUN Fire Engine Red ABS+");
    }

    SECTION("an exact match still absorbs") {
        REQUIRE(compose_filament_label("", "Ambrosia Pink PLA+", "PLA+") == "Ambrosia Pink PLA+");
        REQUIRE(compose_filament_label("Kingroon", "Yellow PETG", "PETG") ==
                "Kingroon Yellow PETG");
    }

    SECTION("compound materials need every word present") {
        REQUIRE(compose_filament_label("", "Bambu PA6-CF", "PA6") == "Bambu PA6-CF");
        REQUIRE(compose_filament_label("", "Bambu PA6-CF", "PA6-CF") == "Bambu PA6-CF");
        // "Carbon" appears nowhere in the name, so the material still earns its place.
        REQUIRE(compose_filament_label("", "Bambu PA6-CF", "PA6 Carbon") ==
                "Bambu PA6-CF PA6 Carbon");
    }

    SECTION("the material need not be at the end of the name") {
        // Redundancy is about whether the name MENTIONS the material, not where.
        // Vendors write it mid-string all the time.
        REQUIRE(compose_filament_label("Polymaker", "Ambrosia PLA (Pink)", "PLA") ==
                "Polymaker Ambrosia PLA (Pink)");
        REQUIRE(compose_filament_label("Polymaker", "PLA Matte Charcoal", "PLA") ==
                "Polymaker PLA Matte Charcoal");
        REQUIRE(compose_filament_label("", "Blue PETG Translucent", "PETG") ==
                "Blue PETG Translucent");
    }

    SECTION("a partially-stated compound material is still appended") {
        // The name says PLA but not that it is the silk variant, so the
        // material still carries information and stays.
        REQUIRE(compose_filament_label("", "Ambrosia PLA (Pink)", "Silk PLA") ==
                "Ambrosia PLA (Pink) Silk PLA");
    }

    SECTION("a brand stated mid-name is not repeated either") {
        REQUIRE(compose_filament_label("Polymaker", "Ambrosia by Polymaker", "PLA") ==
                "Ambrosia by Polymaker PLA");
    }

    SECTION("a material the name does not state is kept") {
        // adamstorm's case: AFC supplies a bare colour-ish name, so "PLA" is
        // the only thing telling the user what is loaded.
        REQUIRE(compose_filament_label("Polymaker", "Ambrosia Pink", "PLA") ==
                "Polymaker Ambrosia Pink PLA");
    }

    SECTION("Poly is not absorbed by Polymaker") {
        REQUIRE(compose_filament_label("Poly", "Polymaker PolyTerra", "") ==
                "Poly Polymaker PolyTerra");
    }

    SECTION("a brand that is a prefix of the first word is still emitted") {
        REQUIRE(compose_filament_label("Sun", "Sunlu Meta", "PLA") == "Sun Sunlu Meta PLA");
    }
}

// ============================================================================
// compose_filament_label — whitespace
// ============================================================================

TEST_CASE("compose_filament_label: collapses and trims whitespace", "[filament][label]") {
    SECTION("leading and trailing whitespace is trimmed") {
        REQUIRE(compose_filament_label("  Polymaker  ", "  Ambrosia Pink ", " PLA ") ==
                "Polymaker Ambrosia Pink PLA");
    }

    SECTION("interior runs collapse to a single space") {
        REQUIRE(compose_filament_label("", "Ambrosia    Pink", "PLA") == "Ambrosia Pink PLA");
    }

    SECTION("tabs and newlines count as whitespace") {
        REQUIRE(compose_filament_label("", "Ambrosia\tPink\nMatte", "PLA") ==
                "Ambrosia Pink Matte PLA");
    }

    SECTION("dedup runs after normalisation") {
        REQUIRE(compose_filament_label(" Polymaker ", "Polymaker   PolyTerra  PLA", "  PLA ") ==
                "Polymaker PolyTerra PLA");
    }

    SECTION("whitespace-only fields count as empty") {
        REQUIRE(compose_filament_label("   ", "  ", "\t") == "");
        REQUIRE(compose_filament_label("   ", " Ambrosia Pink ", "\t") == "Ambrosia Pink");
    }

    SECTION("all-empty composes to empty") {
        REQUIRE(compose_filament_label("", "", "") == "");
    }
}

// ============================================================================
// resolve_filament_label — precedence
// ============================================================================

TEST_CASE("resolve_filament_label: slot fields outrank the Spoolman identity",
          "[filament][label]") {
    SlotInfo slot;
    slot.spool_name = "My Custom Name";
    slot.brand = "MyBrand";
    slot.material = "PETG";

    SpoolIdentity identity;
    identity.vendor = "Polymaker";
    identity.filament_name = "PolyTerra Ambrosia Pink";
    identity.material = "PLA";

    REQUIRE(resolve_filament_label(slot, &identity, "Light Pink") == "MyBrand My Custom Name PETG");
}

TEST_CASE("resolve_filament_label: identity fills only the fields the slot lacks",
          "[filament][label]") {
    SpoolIdentity identity;
    identity.vendor = "Polymaker";
    identity.filament_name = "PolyTerra Ambrosia Pink";
    identity.material = "PLA";

    SECTION("slot supplies name and material, identity supplies the brand") {
        SlotInfo slot = make_afc_slot();
        REQUIRE(resolve_filament_label(slot, &identity, "Light Pink") ==
                "Polymaker Ambrosia Pink PLA");
    }

    SECTION("slot supplies brand only") {
        SlotInfo slot;
        slot.brand = "eSUN";
        REQUIRE(resolve_filament_label(slot, &identity, "Light Pink") ==
                "eSUN PolyTerra Ambrosia Pink PLA");
    }

    SECTION("slot supplies material only") {
        SlotInfo slot;
        slot.material = "PETG";
        REQUIRE(resolve_filament_label(slot, &identity, "Light Pink") ==
                "Polymaker PolyTerra Ambrosia Pink PETG");
    }

    SECTION("empty slot takes the whole identity") {
        SlotInfo slot;
        REQUIRE(resolve_filament_label(slot, &identity, "Light Pink") ==
                "Polymaker PolyTerra Ambrosia Pink PLA");
    }
}

TEST_CASE("resolve_filament_label: a null identity falls through to the slot",
          "[filament][label]") {
    SlotInfo slot = make_afc_slot();
    REQUIRE(resolve_filament_label(slot, nullptr, "Light Pink") == "Ambrosia Pink PLA");
}

TEST_CASE("resolve_filament_label: an identity with no usable field is ignored",
          "[filament][label]") {
    SpoolIdentity empty_identity;
    empty_identity.filament_id = 7; // ids alone are not a label
    empty_identity.vendor_id = 3;
    empty_identity.color_hex = "#FFB6C1";
    REQUIRE_FALSE(empty_identity.valid());

    SlotInfo slot = make_afc_slot();
    REQUIRE(resolve_filament_label(slot, &empty_identity, "Light Pink") == "Ambrosia Pink PLA");

    SECTION("and with nothing on the slot either, the color fallback is used") {
        SlotInfo bare;
        bare.material = "PLA";
        REQUIRE(resolve_filament_label(bare, &empty_identity, "Light Pink") == "Light Pink PLA");
    }
}

TEST_CASE("resolve_filament_label: slot color_name is used when no spool name exists",
          "[filament][label]") {
    SlotInfo slot;
    slot.color_name = "Jet Black";
    slot.material = "PLA";
    slot.spoolman_id = 9;

    REQUIRE(resolve_filament_label(slot, nullptr, "Dark Gray") == "Jet Black PLA");

    SECTION("but a spool name outranks it") {
        slot.spool_name = "Ambrosia Pink";
        REQUIRE(resolve_filament_label(slot, nullptr, "Dark Gray") == "Ambrosia Pink PLA");
    }

    SECTION("and an identity name outranks it") {
        SpoolIdentity identity;
        identity.filament_name = "PolyTerra Ambrosia Pink";
        REQUIRE(resolve_filament_label(slot, &identity, "Dark Gray") ==
                "PolyTerra Ambrosia Pink PLA");
    }
}

TEST_CASE("resolve_filament_label: the algorithmic color name is the last naming layer",
          "[filament][label]") {
    SlotInfo slot;
    slot.material = "PLA";
    slot.color_rgb = 0xFFB6C1;

    REQUIRE(resolve_filament_label(slot, nullptr, "Light Pink") == "Light Pink PLA");

    SECTION("brand still prefixes it") {
        slot.brand = "eSUN";
        REQUIRE(resolve_filament_label(slot, nullptr, "Light Pink") == "eSUN Light Pink PLA");
    }

    SECTION("it is not used once any real name exists") {
        slot.spool_name = "Ambrosia Pink";
        REQUIRE(resolve_filament_label(slot, nullptr, "Light Pink") == "Ambrosia Pink PLA");
    }

    SECTION("a blank fallback is skipped, not emitted") {
        REQUIRE(resolve_filament_label(slot, nullptr, "   ") == "PLA");
    }
}

// ============================================================================
// resolve_filament_label — the never-empty guarantee
// ============================================================================

TEST_CASE("resolve_filament_label: never returns an empty string", "[filament][label]") {
    SlotInfo bare;
    bare.color_rgb = 0; // sentinel: no color recorded

    SECTION("default slot, no identity, no fallback") {
        REQUIRE(resolve_filament_label(bare, nullptr, "") == "Filament");
    }

    SECTION("whitespace-only everything") {
        SlotInfo blank;
        blank.spool_name = "   ";
        blank.brand = "\t";
        blank.material = "  ";
        blank.color_name = "\n";
        REQUIRE(resolve_filament_label(blank, nullptr, "   ") == "Filament");
    }

    SECTION("caller-supplied last resort is honoured") {
        REQUIRE(resolve_filament_label(bare, nullptr, "", "External") == "External");
    }

    SECTION("a blank last resort still yields the built-in default") {
        REQUIRE(resolve_filament_label(bare, nullptr, "", "   ") == "Filament");
    }

    SECTION("the last resort is not used once anything else resolves") {
        SlotInfo slot;
        slot.material = "PLA";
        REQUIRE(resolve_filament_label(slot, nullptr, "", "External") == "PLA");
    }
}

// ============================================================================
// resolve_filament_label — dedup reached through the resolver
// ============================================================================

TEST_CASE("resolve_filament_label: dedup applies across the merged layers", "[filament][label]") {
    SECTION("identity vendor already inside the slot spool name") {
        SlotInfo slot;
        slot.spool_name = "Polymaker PolyTerra Ambrosia Pink";
        slot.material = "PLA";

        SpoolIdentity identity;
        identity.vendor = "Polymaker";
        identity.filament_name = "PolyTerra Ambrosia Pink";
        identity.material = "PLA";

        REQUIRE(resolve_filament_label(slot, &identity, "Light Pink") ==
                "Polymaker PolyTerra Ambrosia Pink PLA");
    }

    SECTION("identity material already at the end of the identity name") {
        SlotInfo slot;
        SpoolIdentity identity;
        identity.vendor = "Polymaker";
        identity.filament_name = "Polymaker PolyTerra Ambrosia Pink PLA";
        identity.material = "PLA";

        REQUIRE(resolve_filament_label(slot, &identity, "Light Pink") ==
                "Polymaker PolyTerra Ambrosia Pink PLA");
    }
}

// ============================================================================
// Regression
// ============================================================================

TEST_CASE("resolve_filament_label: AFC BoxTurtle slot renders its Spoolman name, not the "
          "algorithmic color",
          "[filament][label][regression]") {
    // adamstorm [AT3D] report: the current-loaded card read "Light Pink PLA"
    // because the AFC parse path never populates color_name, so the old builder
    // fell straight through to describe_color() and ignored spool_name.
    SlotInfo slot = make_afc_slot();
    REQUIRE(slot.spoolman_id > 0);
    REQUIRE(slot.color_name.empty());

    const std::string label = resolve_filament_label(slot, nullptr, "Light Pink");

    REQUIRE(label == "Ambrosia Pink PLA");
    REQUIRE(label != "Light Pink PLA");

    SECTION("and once the Spoolman identity cache lands, the vendor joins it") {
        SpoolIdentity identity;
        identity.vendor = "Polymaker";
        identity.filament_name = "PolyTerra Ambrosia Pink";
        identity.material = "PLA";
        identity.filament_id = 12;
        identity.vendor_id = 4;
        REQUIRE(identity.valid());

        REQUIRE(resolve_filament_label(slot, &identity, "Light Pink") ==
                "Polymaker Ambrosia Pink PLA");
    }
}

TEST_CASE("resolve_filament_label: the AFC writer and the picker writer agree",
          "[filament][label][regression]") {
    // Two routes carry the same physical spool onto a slot. AFC parses
    // filament_name off the firmware and never learns the vendor; the Spoolman
    // picker (and the QR scanner, and the external-spool sync) runs the spool
    // through apply_spool_to_slot(). Both write spool_name, so if they disagree
    // on what that field means the same spool renders two different labels.
    SpoolInfo spool;
    spool.id = 42;
    spool.filament_id = 12;
    spool.vendor_id = 4;
    spool.vendor = "Polymaker";
    spool.material = "PLA";
    spool.filament_name = "Ambrosia Pink"; // parse_spool_info maps filament.name here

    SlotInfo picked;
    apply_spool_to_slot(picked, spool);

    const SlotInfo afc = make_afc_slot();

    SpoolIdentity identity;
    identity.vendor = "Polymaker";
    identity.filament_name = "Ambrosia Pink";
    identity.material = "PLA";
    REQUIRE(identity.valid());

    const std::string picker_label = resolve_filament_label(picked, &identity, "Light Pink");
    const std::string afc_label = resolve_filament_label(afc, &identity, "Light Pink");

    CHECK(picker_label == "Polymaker Ambrosia Pink PLA");
    CHECK(afc_label == picker_label);
    // The synthesized "vendor material" collapsed to exactly the brand and the
    // material, because compose_filament_label() dedups both back out again.
    CHECK(picker_label != "Polymaker PLA");

    SECTION("cold identity cache — the picker slot still carries name and brand itself") {
        // apply_spool_to_slot() copies the vendor onto slot.brand, so a
        // picker-linked slot never needs the cache to render in full.
        CHECK(resolve_filament_label(picked, nullptr, "Light Pink") ==
              "Polymaker Ambrosia Pink PLA");
    }
}

// ============================================================================
// resolve_filament_label_parts — the split form
// ============================================================================

TEST_CASE("resolve_filament_label_parts: exposes the resolved fields separately",
          "[filament][label]") {
    // The Active Spool home widget prints the material on its own row, so it
    // needs the same precedence as the AMS card but must join brand and name
    // without re-appending the material underneath a row that already says it.
    const SlotInfo slot = make_afc_slot();

    SpoolIdentity identity;
    identity.vendor = "Polymaker";
    identity.filament_name = "PolyTerra Ambrosia Pink";
    identity.material = "PLA";
    REQUIRE(identity.valid());

    const auto parts = helix::resolve_filament_label_parts(slot, &identity, "Light Pink");

    CHECK(parts.brand == "Polymaker");    // filled from the identity — AFC has none
    CHECK(parts.name == "Ambrosia Pink"); // slot.spool_name outranks identity.filament_name
    CHECK(parts.material == "PLA");

    SECTION("joining brand and name alone drops the material") {
        CHECK(compose_filament_label(parts.brand, parts.name, "") == "Polymaker Ambrosia Pink");
    }

    SECTION("the full resolver is the same parts, composed") {
        CHECK(resolve_filament_label(slot, &identity, "Light Pink") ==
              compose_filament_label(parts.brand, parts.name, parts.material));
    }
}

TEST_CASE("resolve_filament_label_parts: color name is the last naming layer",
          "[filament][label][regression]") {
    // #1264: the widget hid its brand row whenever brand, color_name and
    // spool_name were all blank. A slot that knows only its color still has a
    // name to show, and a Spoolman-linked lane still has a vendor to show.
    SlotInfo bare;
    bare.slot_index = 0;
    bare.status = SlotStatus::LOADED;
    bare.material = "PLA";
    bare.color_rgb = 0xFFB6C1;

    SECTION("cold cache — the algorithmic color name carries the row") {
        const auto parts = helix::resolve_filament_label_parts(bare, nullptr, "Light Pink");
        CHECK(parts.brand.empty());
        CHECK(parts.name == "Light Pink");
        CHECK(compose_filament_label(parts.brand, parts.name, "") == "Light Pink");
    }

    SECTION("Spoolman supplies the vendor the backend never reported") {
        SpoolIdentity identity;
        identity.vendor = "Polymaker";
        identity.filament_name = "PolyTerra Ambrosia Pink";
        REQUIRE(identity.valid());

        const auto parts = helix::resolve_filament_label_parts(bare, &identity, "Light Pink");
        CHECK(parts.brand == "Polymaker");
        CHECK(parts.name == "PolyTerra Ambrosia Pink");
        // "Polymaker" is not a word inside "PolyTerra", so brand-dedup leaves it
        // alone -- the same boundary rule that keeps "Sun" out of "Sunlu".
        CHECK(compose_filament_label(parts.brand, parts.name, "") ==
              "Polymaker PolyTerra Ambrosia Pink");
    }

    SECTION("nothing at all — the caller gets empties and can hide the row") {
        SlotInfo empty;
        empty.slot_index = 0;
        const auto parts = helix::resolve_filament_label_parts(empty, nullptr, "");
        CHECK(parts.brand.empty());
        CHECK(parts.name.empty());
        CHECK(compose_filament_label(parts.brand, parts.name, "").empty());
    }
}
