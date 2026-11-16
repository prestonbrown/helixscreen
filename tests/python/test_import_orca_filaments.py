# SPDX-License-Identifier: GPL-3.0-or-later

"""Tests for scripts/import_orca_filaments.py.

Lives in tests/python/ (not next to the script) so that nightly CI's
`pytest tests/python/` actually runs it. While it sat in scripts/ nothing
executed it and one case silently rotted for months.
"""

import json
import sys
from pathlib import Path

# Add scripts directory to path
scripts_dir = Path(__file__).parent.parent.parent / "scripts"
sys.path.insert(0, str(scripts_dir))

import import_orca_filaments as imp  # noqa: E402

FIX = str(scripts_dir / "fixtures" / "orca")
FIX_COLLISION = str(scripts_dir / "fixtures" / "orca_collision")
FIX_VENDOR_PACK = str(scripts_dir / "fixtures" / "orca_vendor_pack")


def load_fixtures():
    return imp.load_profiles(FIX)


def test_resolve_inherits_flattens_chain():
    by_name = load_fixtures()
    leaf = by_name["Polymaker ABS Pro @System"]
    r = imp.resolve_inherits(leaf, by_name)
    assert imp.first_scalar(r["filament_type"]) == "ABS"          # from fdm_filament_abs
    assert imp.first_scalar(r["filament_vendor"]) == "Polymaker"  # from @base
    assert imp.first_scalar(r["nozzle_temperature"]) == "280"     # @base overrides


def test_first_scalar_handles_nil_and_arrays():
    assert imp.first_scalar(["220", "220"]) == "220"
    assert imp.first_scalar(["nil"]) is None
    assert imp.first_scalar([]) is None


def test_map_type_known_and_unknown(monkeypatch):
    """map_type() looks the type up, and falls back to the raw value.

    Most of TYPE_MAP's entries are identity mappings (X: X), so asserting
    map_type("PLA") == "PLA" passes whether the lookup happened or the function
    simply returned its argument. Patch in a non-identity entry (same technique
    as test_build_catalog_returns_raw_library_types) so the remap and the
    fallback produce different answers without depending on the real table.
    """
    monkeypatch.setitem(imp.TYPE_MAP, "PET-CF", "CARBON-PET")

    assert imp.map_type("PET-CF") == "CARBON-PET"   # remapped key
    assert imp.map_type("PLA") == "PLA"             # untouched identity entry
    assert imp.map_type("WeirdNew") == "WeirdNew"   # absent key -> raw fallback


def test_collapse_bed_prefers_textured_skips_nil():
    r = imp.resolve_inherits(load_fixtures()["Polymaker ABS Pro @System"], load_fixtures())
    assert imp.collapse_bed(r) == 105   # textured wins
    common = {"cool_plate_temp": ["55"], "hot_plate_temp": ["nil"], "textured_plate_temp": ["nil"]}
    assert imp.collapse_bed(common) == 55  # falls through to cool


def test_build_product_thin_when_range_matches_type():
    r = imp.resolve_inherits(load_fixtures()["Polymaker ABS Pro @System"], load_fixtures())
    p = imp.build_product(r, base_type_range=(245, 265))
    assert p["brand"] == "Polymaker"
    assert p["type"] == "ABS"
    assert p["nozzle"] == 280
    assert p["nozzle_min"] == 270 and p["nozzle_max"] == 290  # differs from type → emitted
    assert p["bed"] == 105
    assert p["orca_id"] == "OGFPMABSPRO"
    assert p["source"] == "orca"


def test_build_product_widens_range_when_nozzle_outside_recommended_range():
    # Some upstream Orca profiles set nozzle_temperature outside their own
    # range_low/range_high (e.g. a "recommended" temp bumped up without the
    # range being updated). The emitted product must stay internally coherent.
    resolved = {
        "filament_type": ["PLA"],
        "filament_vendor": ["Acme"],
        "filament_id": "ACMEPLAHOT",
        "_product_name": "PLA Hot",
        "nozzle_temperature": ["275"],
        "nozzle_temperature_range_low": ["220"],
        "nozzle_temperature_range_high": ["260"],
    }
    p = imp.build_product(resolved, base_type_range=(190, 220))
    assert p["nozzle"] == 275
    assert "nozzle_min" in p and "nozzle_max" in p
    assert p["nozzle_min"] <= 275 <= p["nozzle_max"]
    assert p["nozzle_max"] >= 275  # widened, not left at the stale 260


def test_build_product_emits_range_for_unmapped_type_with_no_base_range():
    # An unmapped filament type (e.g. EVA) has no base_type_range entry, and
    # this profile also has no explicit nozzle_temperature_range_*. The
    # product must still get an explicit, coherent nozzle_min/nozzle_max.
    resolved = {
        "filament_type": ["EVA"],
        "filament_vendor": ["Qidi"],
        "filament_id": "QIDIEVA",
        "_product_name": "Generic EVA",
        "nozzle_temperature": ["230"],
    }
    p = imp.build_product(resolved, base_type_range=None)
    assert p["nozzle"] == 230
    assert "nozzle_min" in p and "nozzle_max" in p
    assert p["nozzle_min"] <= 230 <= p["nozzle_max"]


def test_build_catalog_unions_orca_and_seed():
    seed = [{"id": "creality-hyper-pla", "brand": "Creality", "name": "Hyper PLA",
             "type": "PLA", "nozzle": 215, "codes": {"cfs": "01001"}, "source": "cfs-seed"}]
    cat, _ = imp.build_catalog(FIX, seed, type_ranges={"ABS": (245, 265)}, library_marker="")
    ids = {p["id"] for p in cat}
    assert "creality-hyper-pla" in ids                 # seed preserved
    assert any(p["brand"] == "Polymaker" for p in cat)  # orca product present
    assert all("P100" not in p["id"] for p in cat)      # no placeholders


def test_build_catalog_is_deterministic():
    seed = []
    a, _ = imp.build_catalog(FIX, seed, type_ranges={"ABS": (245, 265)}, library_marker="")
    b, _ = imp.build_catalog(FIX, seed, type_ranges={"ABS": (245, 265)}, library_marker="")
    assert json.dumps(a) == json.dumps(b)               # stable order + shape


def _write_library(root, profiles):
    lib = root / "OrcaFilamentLibrary" / "filament"
    lib.mkdir(parents=True)
    for i, p in enumerate(profiles):
        (lib / f"p{i}.json").write_text(json.dumps({"type": "filament", **p}))
    return str(root)


def _orca_ids(tmp_path, profiles):
    cat, _ = imp.build_catalog(_write_library(tmp_path, profiles), [], type_ranges={})
    assert not [k for p in cat for k in p if k.startswith("_")]  # no bookkeeping keys ship
    return {p["name"]: p.get("orca_id") for p in cat}


def test_inherited_filament_id_shared_by_two_products_is_dropped(tmp_path):
    ids = _orca_ids(tmp_path, [
        {"name": "Acme PLA @base", "instantiation": "false", "filament_id": "ACME00",
         "filament_vendor": ["Acme"], "filament_type": ["PLA"]},
        {"name": "Acme PLA Matte @System", "inherits": "Acme PLA @base", "instantiation": "true"},
        {"name": "Acme PLA Silk @System", "inherits": "Acme PLA @base", "instantiation": "true"},
    ])
    assert ids == {"PLA Matte": None, "PLA Silk": None}


def test_own_filament_id_stands_beside_a_sibling_that_inherits_it(tmp_path):
    ids = _orca_ids(tmp_path, [
        {"name": "Acme PLA @base", "instantiation": "false", "filament_id": "ACME00",
         "filament_vendor": ["Acme"], "filament_type": ["PLA"]},
        {"name": "Acme PLA @System", "inherits": "Acme PLA @base", "instantiation": "true",
         "filament_id": "ACME00"},
        {"name": "Acme PLA Matte @System", "inherits": "Acme PLA @base", "instantiation": "true"},
        {"name": "Acme PLA Silk @System", "inherits": "Acme PLA @base", "instantiation": "true",
         "filament_id": "ACME02"},
    ])
    assert ids == {"PLA": "ACME00", "PLA Matte": None, "PLA Silk": "ACME02"}


def test_inherited_filament_id_of_a_single_product_template_is_kept(tmp_path):
    ids = _orca_ids(tmp_path, [
        {"name": "Acme ASA @base", "instantiation": "false", "filament_id": "ACME10",
         "filament_vendor": ["Acme"], "filament_type": ["ASA"]},
        {"name": "Acme ASA @System", "inherits": "Acme ASA @base", "instantiation": "true"},
    ])
    assert ids == {"ASA": "ACME10"}


def test_slug_preserves_plus_suffix():
    # "PLA+" must NOT collapse to the same slug as "PLA" — distinct products.
    assert imp._slug("Generic", "PLA+") != imp._slug("Generic", "PLA")
    assert imp._slug("Generic", "PLA+") == "generic-pla-plus"
    assert imp._slug("Generic", "PLA") == "generic-pla"


def test_dedupe_ids_disambiguates_collisions():
    products = [
        {"id": "acme-widget", "brand": "Acme", "name": "Widget A", "source": "orca"},
        {"id": "acme-widget", "brand": "Acme", "name": "Widget B", "source": "cfs-seed"},
        {"id": "acme-widget", "brand": "Acme", "name": "Widget C", "source": "cfs-seed"},
    ]
    imp._dedupe_ids(products)
    ids = [p["id"] for p in products]
    assert len(ids) == len(set(ids))                    # every id now unique
    assert ids == ["acme-widget", "acme-widget-2", "acme-widget-3"]


def test_build_catalog_scopes_inherits_to_library_avoiding_vendor_base_collision():
    # Regression for the vendor-base collision bug: a same-named base profile
    # (fdm_filament_pc) exists both inside OrcaFilamentLibrary (Generic, 280)
    # and under a sibling vendor pack (Qidi, 250). Inheritance resolution for
    # library products must use ONLY the library's own base, not whichever
    # vendor copy happened to load last into the flat name->profile map.
    cat, _ = imp.build_catalog(FIX_COLLISION, [], type_ranges={})
    assert len(cat) == 1
    product = cat[0]
    assert product["brand"] == "Generic"
    assert product["nozzle"] == 280


def _vendor_pack_catalog():
    cat, library_types = imp.build_catalog(
        FIX_VENDOR_PACK, [], type_ranges={"ASA": (240, 270), "Silk PLA": (200, 230)},
        vendor_packs=("Eryone",))
    return {(p["brand"], p["name"]): p for p in cat}, library_types


def test_build_catalog_imports_listed_vendor_pack_only():
    by_key, _ = _vendor_pack_catalog()
    assert ("Eryone", "PLA") in by_key
    assert ("Eryone", "ASA") in by_key
    assert ("Generic", "PLA") in by_key                 # the library still imports
    assert not any(brand == "Qidi" for brand, _ in by_key)  # unlisted pack stays out
    # Templates are not products.
    assert ("Eryone", "Standard PLA") not in by_key


def test_vendor_pack_skips_printer_and_nozzle_variants():
    by_key, _ = _vendor_pack_catalog()
    eryone = [k for k in by_key if k[0] == "Eryone"]
    assert len(eryone) == len(set(eryone))
    # The base profile's 265, not the "@0.2 nozzle" variant's 250.
    assert by_key[("Eryone", "ASA")]["nozzle"] == 265


def test_vendor_pack_same_type_child_keeps_parent_material_facts():
    by_key, _ = _vendor_pack_catalog()
    pla = by_key[("Eryone", "PLA")]
    assert pla["type"] == "PLA"
    assert pla["nozzle"] == 220
    assert (pla["nozzle_min"], pla["nozzle_max"]) == (180, 260)
    assert pla["density"] == 1.24
    assert pla["orca_id"] == "EFL90"


def test_vendor_pack_retyped_child_drops_parent_material_facts():
    # Eryone ASA inherits a PLA template. Its own nozzle and bed temps stand, but
    # the PLA range, density and textured-plate temp are facts about PLA.
    by_key, _ = _vendor_pack_catalog()
    asa = by_key[("Eryone", "ASA")]
    assert asa["type"] == "ASA"
    assert asa["bed"] == 110                            # not the PLA textured 60
    assert "density" not in asa                         # not PLA's 1.24
    assert (asa.get("nozzle_min"), asa.get("nozzle_max")) == (None, None)  # thin: ASA type range


def test_vendor_pack_silk_maps_to_catalog_silk_type():
    by_key, _ = _vendor_pack_catalog()
    assert by_key[("Eryone", "Silk PLA")]["type"] == "Silk PLA"


def test_vendor_pack_types_stay_out_of_library_types():
    # orca_library_types is what Orca can match a lane's material against on any
    # printer; a vendor pack's types are only visible on that vendor's printers.
    _, library_types = _vendor_pack_catalog()
    assert library_types == ["PLA"]


def test_build_catalog_guards_against_id_collisions():
    # _merge_seed recomputes every seed id from brand+name, so a seed entry's
    # DECLARED id can never collide on its own -- the collision that _dedupe_ids
    # actually guards against is two different (brand, name) pairs that slug to the
    # same string. Here "Polymaker-ABS"/"Pro" and Orca's "Polymaker"/"ABS Pro" both
    # slug to "polymaker-abs-pro", and the differing brand means they do not merge.
    #
    # (This test previously asserted a collision that could not occur, and passed
    # nothing but its first two assertions for months because nothing ran it.)
    colliding_id = imp._slug("Polymaker", "ABS Pro")
    assert imp._slug("Polymaker-ABS", "Pro") == colliding_id
    seed = [{"id": "whatever-gets-recomputed", "brand": "Polymaker-ABS", "name": "Pro",
             "type": "ABS", "nozzle": 260, "source": "cfs-seed"}]
    cat, _ = imp.build_catalog(FIX, seed, type_ranges={"ABS": (245, 265)}, library_marker="")
    ids = [p["id"] for p in cat]
    assert len(ids) == len(set(ids))                    # no collision survives
    assert colliding_id in ids
    assert f"{colliding_id}-2" in ids


# ---------------------------------------------------------------------------
# Degenerate nozzle ranges (the generic-eva defect)
# ---------------------------------------------------------------------------

def test_build_product_discards_degenerate_orca_range():
    # The exact shape that shipped generic-eva as "200-200": Orca sets
    # range_low == range_high, which is a single temperature masquerading as a
    # range. It must be discarded so the base type range supplies a real one --
    # and here the result equals the base range, so no keys are emitted at all and
    # the product inherits from filament_database.h.
    resolved = {
        "filament_type": ["EVA"],
        "filament_vendor": ["Generic"],
        "filament_id": "OGFR99",
        "_product_name": "EVA",
        "nozzle_temperature": ["200"],
        "nozzle_temperature_range_low": ["200"],
        "nozzle_temperature_range_high": ["200"],
    }
    p = imp.build_product(resolved, base_type_range=(190, 220))
    assert "nozzle_min" not in p and "nozzle_max" not in p
    assert p["nozzle"] == 200


def test_build_product_never_emits_a_degenerate_range():
    # Same defect, but for an unmapped type with NO base range to fall back to --
    # the one path the first gate cannot reach. It must widen rather than emit a
    # zero-width range.
    resolved = {
        "filament_type": ["Unobtanium"],
        "filament_vendor": ["Acme"],
        "filament_id": "ACME1",
        "_product_name": "Weird",
        "nozzle_temperature": ["240"],
        "nozzle_temperature_range_low": ["240"],
        "nozzle_temperature_range_high": ["240"],
    }
    p = imp.build_product(resolved, base_type_range=None)
    assert p["nozzle_min"] < p["nozzle_max"]
    assert p["nozzle_min"] <= 240 <= p["nozzle_max"]


def test_assert_no_degenerate_ranges_rejects_any_source():
    # The catch-all gate. seed entries write nozzle_min/nozzle_max straight
    # through without going near build_product(), so this is the only check that
    # covers every product regardless of where it came from.
    import pytest
    ok = [{"id": "a", "nozzle_min": 190, "nozzle_max": 220}]
    imp._assert_no_degenerate_ranges(ok)  # does not raise

    for bad in ([{"id": "zero-width", "nozzle_min": 200, "nozzle_max": 200}],
                [{"id": "inverted", "nozzle_min": 250, "nozzle_max": 240}]):
        with pytest.raises(SystemExit) as exc:
            imp._assert_no_degenerate_ranges(bad)
        assert bad[0]["id"] in str(exc.value)


# ---------------------------------------------------------------------------
# Seed merge (the duplicate Generic/PETG-CF defect)
# ---------------------------------------------------------------------------

def test_merge_seed_folds_into_orca_despite_type_disagreement():
    # Orca labels their Generic PETG-CF profile with filament_type "PETG". The seed
    # entry is correctly typed "PETG-CF". Keying the merge on type made these two
    # different products, so the seed appended and _dedupe_ids renamed it
    # "generic-petg-cf-2" -- two identical Generic/PETG-CF rows in the picker.
    #
    # They must now merge into ONE product that keeps the Orca id/orca_id AND the
    # seed's cfs code, with the seed's curated type winning.
    products = [{"id": "generic-petg-cf", "brand": "Generic", "name": "PETG-CF",
                 "type": "PETG", "nozzle": 255, "orca_id": "OGFG99", "source": "orca"}]
    seed = [{"id": "generic-generic-petg-cf", "brand": "Generic", "name": "Generic PETG-CF",
             "type": "PETG-CF", "codes": {"cfs": "00014"}, "source": "cfs-seed"}]
    imp._merge_seed(products, seed)

    assert len(products) == 1
    p = products[0]
    assert p["id"] == "generic-petg-cf"
    assert p["type"] == "PETG-CF"       # seed's curated type wins over Orca's
    assert p["orca_id"] == "OGFG99"     # Orca provenance retained
    assert p["codes"]["cfs"] == "00014"  # seed's CFS code folded in, not lost


def test_merge_seed_still_appends_unmatched_entries():
    # The relaxed key must not swallow seed entries that have no Orca counterpart
    # (CFS-only variants, and the helix-seed placeholders that make a
    # filament_database.h type reachable in the picker).
    products = [{"id": "generic-pla", "brand": "Generic", "name": "PLA",
                 "type": "PLA", "source": "orca"}]
    seed = [
        {"id": "generic-ppa", "brand": "Generic", "name": "PPA",
         "type": "PPA", "source": "helix-seed"},
        {"id": "creality-cr-abs", "brand": "Creality", "name": "CR-ABS",
         "type": "ABS", "codes": {"cfs": "07001"}, "source": "cfs-seed"},
    ]
    imp._merge_seed(products, seed)

    by_id = {p["id"]: p for p in products}
    assert set(by_id) == {"generic-pla", "generic-ppa", "creality-cr-abs"}
    assert by_id["creality-cr-abs"]["codes"]["cfs"] == "07001"


def test_merge_seed_does_not_clobber_an_existing_orca_code():
    products = [{"id": "generic-pla", "brand": "Generic", "name": "PLA", "type": "PLA",
                 "codes": {"cfs": "99999"}, "source": "orca"}]
    seed = [{"id": "x", "brand": "Generic", "name": "PLA", "type": "PLA",
             "codes": {"cfs": "00001", "other": "abc"}, "source": "cfs-seed"}]
    imp._merge_seed(products, seed)
    assert len(products) == 1
    assert products[0]["codes"] == {"cfs": "99999", "other": "abc"}


def test_merge_seed_does_not_fold_plus_variant_into_base():
    # _name_key stripped "+" along with every other non-alnum character, so a seed
    # entry named "PLA+" normalized to the same key as Orca's "PLA" and merged --
    # the seed's type then won (by design, see the disagreement test above), which
    # retyped plain Generic PLA as "PLA+". "+" is a meaningful suffix (see _slug
    # and test_slug_preserves_plus_suffix): PLA+ and PLA are different products and
    # must never collide in the merge key.
    products = [{"id": "generic-pla", "brand": "Generic", "name": "PLA",
                 "type": "PLA", "source": "orca"}]
    seed = [{"id": "generic-pla-plus", "brand": "Generic", "name": "PLA+",
             "type": "PLA+", "codes": {"cfs": "10001"}, "source": "cfs-seed"}]
    imp._merge_seed(products, seed)

    by_id = {p["id"]: p for p in products}
    assert set(by_id) == {"generic-pla", "generic-pla-plus"}
    assert by_id["generic-pla"]["type"] == "PLA"            # base untouched
    assert by_id["generic-pla-plus"]["type"] == "PLA+"       # seed kept its own type
    assert by_id["generic-pla-plus"]["codes"]["cfs"] == "10001"


# ---------------------------------------------------------------------------
# Orca library type tables (the silent-PLA-fallback fix)
# ---------------------------------------------------------------------------

def test_build_catalog_returns_raw_library_types():
    # The allowlist must carry Orca's RAW filament_type, not map_type()'s
    # remapped value — Orca matches on its own vocabulary, not ours.
    #
    # Every entry in TYPE_MAP is currently an identity mapping, so asserting
    # "ABS" in library_types can't tell raw from mapped apart -- it passes
    # either way. Monkeypatch in one genuinely non-identity entry so the two
    # implementations actually diverge, and assert on that divergence: the
    # fixture's real "ABS" type must surface unchanged, while the fake mapped
    # target must NOT appear (nothing in the fixture actually has that type).
    original_type_map = dict(imp.TYPE_MAP)
    imp.TYPE_MAP["ABS"] = "NOT-ORCAS-VOCAB"
    try:
        seed = []
        products, library_types = imp.build_catalog(FIX, seed, {}, library_marker="")
        assert isinstance(library_types, list)
        assert library_types == sorted(library_types)   # deterministic output
        assert len(library_types) == len(set(library_types))  # deduped
        assert "ABS" in library_types                     # raw type survives
        assert "NOT-ORCAS-VOCAB" not in library_types      # mapped value must not leak in
    finally:
        imp.TYPE_MAP.clear()
        imp.TYPE_MAP.update(original_type_map)


def test_library_types_excludes_non_instantiable_templates():
    # @base templates are never selectable in Orca, so their types must not
    # enter the allowlist.
    seed = []
    _products, library_types = imp.build_catalog(FIX, seed, {}, library_marker="")
    by_name = imp.load_profiles(FIX)
    template_only = {
        imp.first_scalar(imp.resolve_inherits(p, by_name).get("filament_type"))
        for p in by_name.values()
        if p.get("instantiation") != "true"
    }
    selectable = {
        imp.first_scalar(imp.resolve_inherits(p, by_name).get("filament_type"))
        for p in by_name.values()
        if p.get("instantiation") == "true"
    }
    for t in template_only - selectable:
        assert t not in library_types


def test_orca_type_overrides_target_only_real_library_types():
    # orca_match_type() (C++) returns an override's target verbatim, with no
    # check that the target is itself a real library type. That's the
    # invariant that makes orca_match_type IDEMPOTENT: if a target ever fell
    # outside the library, orca_match_type(target) could resolve to something
    # else again, and repeated application (a follow-up heal-on-boot loop)
    # would never converge. Guard it here, where the override table lives.
    #
    # This must check against the REAL shipped library, not the tiny
    # scripts/fixtures/orca/ tree used elsewhere in this file -- the fixture's
    # library_types set is deliberately minimal (a handful of test profiles)
    # and doesn't contain ASA/PLA/etc, so every real override would look
    # invalid against it. assets/filaments.json is regenerated by
    # `make regen-filaments` (network + Orca checkout required), so read the
    # committed file rather than regenerating it here.
    project_root = Path(__file__).parent.parent.parent
    with open(project_root / "assets" / "filaments.json", encoding="utf-8") as f:
        catalog = json.load(f)
    library_types = set(catalog["orca_library_types"])

    for orca_type, target in imp.ORCA_TYPE_OVERRIDES.items():
        assert target == "" or target in library_types, (
            f"override {orca_type!r} -> {target!r} targets a type outside "
            "orca_library_types; orca_match_type would not be idempotent"
        )


def test_main_carries_the_types_table_through_a_regen(tmp_path, monkeypatch):
    # `types` lives only in the output file, so a regen that dropped it would
    # delete the runtime's per-type table. Its ranges also seed products.
    types = [{"name": "ABS", "nozzle_min": 245, "nozzle_max": 265, "bed": 100}]
    out = tmp_path / "filaments.json"
    out.write_text(json.dumps({"types": types, "filaments": []}))
    seed = tmp_path / "seed.json"
    seed.write_text("[]")
    seen = {}

    def fake_build(orca, seed, type_ranges, library_marker="x", id_products=None):
        seen["ranges"] = type_ranges
        return [], []

    monkeypatch.setattr(imp, "build_catalog", fake_build)
    assert imp.main(["--orca", FIX, "--seed", str(seed), "--out", str(out)]) == 0
    assert seen["ranges"] == {"ABS": (245, 265)}
    assert json.loads(out.read_text())["types"] == types


def test_main_refuses_an_output_without_a_types_table(tmp_path):
    out = tmp_path / "filaments.json"
    out.write_text(json.dumps({"filaments": []}))
    seed = tmp_path / "seed.json"
    seed.write_text("[]")
    try:
        imp.main(["--orca", FIX, "--seed", str(seed), "--out", str(out)])
    except SystemExit:
        pass
    else:
        raise AssertionError("main wrote an output with no types table")
    assert json.loads(out.read_text()) == {"filaments": []}


# --- Ids from a second Orca tag, and the refusal to change it silently (#2026) ---

def _orca_tree(root, profiles):
    """A minimal resources/profiles root: one library profile per (name, id) pair."""
    lib = root / "OrcaFilamentLibrary" / "filament"
    lib.mkdir(parents=True)
    for name, filament_id in profiles:
        profile = {
            "type": "filament", "name": f"{name} @System", "instantiation": "true",
            "filament_vendor": ["Acme"], "filament_type": ["PLA"],
            "nozzle_temperature": ["210"],
        }
        if filament_id:
            profile["filament_id"] = filament_id
        (lib / f"{name}.json").write_text(json.dumps(profile))
    return str(root)


ID_TAG_ATTRIBUTION = imp.attribution("v1", "v2")
TYPES = [{"name": "PLA", "nozzle_min": 190, "nozzle_max": 230, "bed": 60}]


def _regen_inputs(tmp_path, attribution=ID_TAG_ATTRIBUTION):
    old = _orca_tree(tmp_path / "v1", [("Acme Red", "OLD1"), ("Acme Blue", "OLD2"),
                                       ("Acme Gone", "OLD3")])
    new = _orca_tree(tmp_path / "v2", [("Acme Red", "NEW1"), ("Acme Blue", None)])
    seed = tmp_path / "seed.json"
    seed.write_text(json.dumps([{
        "id": "acme-hand-added", "brand": "Acme", "name": "Hand Added", "type": "PLA",
        "nozzle": 215, "source": "vendor-spec",
    }]))
    out = tmp_path / "filaments.json"
    out.write_text(json.dumps({"_attribution": attribution, "types": TYPES, "filaments": []}))
    return old, new, str(seed), out


def test_attributed_id_tag_reads_back_what_attribution_writes():
    assert imp.attributed_id_tag({"_attribution": imp.attribution("v2.4.1", "v2.5.0-beta")}) \
        == "v2.5.0-beta"
    assert imp.attributed_id_tag({"_attribution": imp.attribution("v2.4.1")}) is None
    assert imp.attributed_id_tag({}) is None


def test_main_refuses_to_drop_the_catalogs_ids_tag(tmp_path):
    old, _new, seed, out = _regen_inputs(tmp_path)
    before = out.read_text()
    try:
        imp.main(["--orca", old, "--seed", seed, "--orca-tag", "v1", "--out", str(out)])
    except SystemExit as e:
        assert "v2" in str(e.code)
    else:
        raise AssertionError("a single-tag regen overwrote a catalog with an ids tag")
    assert out.read_text() == before


def test_main_refuses_a_different_ids_tag_unless_allowed(tmp_path):
    old, new, seed, out = _regen_inputs(tmp_path)
    before = out.read_text()
    args = ["--orca", old, "--orca-ids", new, "--orca-id-tag", "v3", "--seed", seed,
            "--orca-tag", "v1", "--out", str(out)]
    try:
        imp.main(args)
    except SystemExit:
        pass
    else:
        raise AssertionError("a regen moved the ids tag without --allow-id-tag-change")
    assert out.read_text() == before
    assert imp.main(args + ["--allow-id-tag-change"]) == 0
    assert imp.attributed_id_tag(json.loads(out.read_text())) == "v3"


def test_main_takes_ids_from_the_ids_tag_and_keeps_seed_rows(tmp_path):
    old, new, seed, out = _regen_inputs(tmp_path)
    assert imp.main(["--orca", old, "--orca-ids", new, "--orca-id-tag", "v2",
                     "--seed", seed, "--orca-tag", "v1", "--out", str(out)]) == 0
    doc = json.loads(out.read_text())
    by_name = {p["name"]: p for p in doc["filaments"]}
    assert by_name["Red"]["orca_id"] == "NEW1"
    # No id at the ids tag, or absent from it: the old-scheme id must not survive.
    assert "orca_id" not in by_name["Blue"]
    assert "orca_id" not in by_name["Gone"]
    assert by_name["Hand Added"]["source"] == "vendor-spec"
    assert doc["_attribution"] == ID_TAG_ATTRIBUTION


def test_main_without_an_ids_tag_regens_a_catalog_that_never_had_one(tmp_path):
    old, _new, seed, out = _regen_inputs(tmp_path, attribution=imp.attribution("v1"))
    assert imp.main(["--orca", old, "--seed", seed, "--orca-tag", "v1", "--out", str(out)]) == 0
    by_name = {p["name"]: p for p in json.loads(out.read_text())["filaments"]}
    assert by_name["Red"]["orca_id"] == "OLD1"


def test_main_wants_ids_root_and_ids_tag_together(tmp_path):
    old, new, seed, out = _regen_inputs(tmp_path)
    try:
        imp.main(["--orca", old, "--orca-ids", new, "--seed", seed, "--out", str(out)])
    except SystemExit:
        return
    raise AssertionError("--orca-ids without --orca-id-tag was accepted")
