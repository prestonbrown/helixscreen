# SPDX-License-Identifier: GPL-3.0-or-later
"""The unit art kit: camera, spools tied to the app's values, baked layers."""
import subprocess
import dataclasses
import json
import math
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from unit_art import build, kit, spool  # noqa: E402
from unit_art.units import ace_pro, ad5x_ifs, ams_ht, ams_lite, bmcu, box_turtle, cfs, elegoo_canvas, emu, htlf, mmx, night_owl, quattro_box, rail, three_ms, yammu  # noqa: E402


def test_camera_is_the_agreed_three_quarter():
    cam = kit.ART_CAMERA
    yaw, pitch = math.radians(27), math.radians(20)
    x, y = cam.project((1.0, 0.0, 0.0))
    assert x == pytest.approx(math.cos(yaw))
    assert y == pytest.approx(math.sin(yaw) * math.sin(pitch))
    assert cam.project((0.0, 0.0, 1.0))[1] == pytest.approx(-math.cos(pitch))


def test_spool_constants_match_cpp():
    c = spool.spool_constants(REPO)
    assert c["flange_lift"] == 60
    assert c["flange_drop"] == 40
    assert c["hub_radius"] == pytest.approx(0.10)
    assert c["slot_inner"] == pytest.approx(0.165)
    assert c["slot_outer"] == pytest.approx(0.335)
    assert c["slot_half_width"] == pytest.approx(0.05)
    assert c["dot_pitch"] == pytest.approx(0.03)
    assert c["flange_radius"] == pytest.approx(0.42)


def test_spool_constants_fail_when_a_value_is_gone(tmp_path):
    with pytest.raises(KeyError):
        spool.spool_constants(tmp_path)


def _scene(tier, colors=None):
    scene = kit.Scene(kit.ART_CAMERA)
    ace_pro.UNIT.draw_art(scene, colors or ["#ff0000", "#00ff00", "#0000ff", "#ffffff"], tier)
    return scene


def test_thumb_has_no_air_holes():
    full = sum(1 for i in _scene("full").items if i.tag == "air_hole")
    thumb = sum(1 for i in _scene("thumb").items if i.tag == "air_hole")
    assert full > 100
    assert thumb == 0


def test_svg_has_no_text_elements():
    assert "<text" not in _scene("full").svg(100)


def test_lane_items_are_kept_apart_from_the_shell():
    scene = _scene("full")
    assert {i.lane for i in scene.items if i.lane is not None} == {0, 1, 2, 3}
    assert not [i for i in scene.items if i.lane is not None and i.layer == "front"]
    assert any(i.layer == "front" for i in scene.items)


def test_an_empty_lane_draws_no_filament():
    scene = _scene("full", ["#ff0000", "#00ff00", "#0000ff", None])
    assert not [i for i in scene.items if i.lane == 3]


@pytest.mark.parametrize("unit", build.UNITS, ids=lambda u: u.id)
def test_every_unit_keeps_its_filament_tintable(unit):
    scene = kit.Scene(kit.ART_CAMERA)
    unit.draw_art(scene, ["#ffffff"] * unit.lanes, "full")
    assert {i.lane for i in scene.items if i.lane is not None} == set(range(unit.lanes))
    assert "<text" not in scene.svg(100)


def test_every_unit_is_baked_through_the_one_camera(tmp_path):
    seen = []
    real = build.kit.Scene

    def spy(camera):
        seen.append(camera)
        return real(camera)

    build.kit.Scene = spy
    try:
        for unit in build.UNITS:
            build.bake_unit(unit, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    finally:
        build.kit.Scene = real
    # A rail's head-on drawing is the one other view, and the bake picks that camera too.
    assert seen and all(c is kit.ART_CAMERA or c is kit.FACE_CAMERA for c in seen)


def test_the_open_box_turtle_has_no_lid_and_the_enclosed_one_does():
    assert box_turtle.UNIT.tray.dome == []
    assert box_turtle.ENCLOSED.tray.dome


def test_a_lid_keeps_its_corners_at_the_trays_point_count():
    square = [(0.0, 0.0), (0.0, 10.0), (1.0, 10.0), (1.0, 0.0)]
    fitted = kit.fit_cap_points(square)
    assert len(fitted) == kit.CAP_POINTS
    assert all(corner in fitted for corner in square)


def test_a_long_lid_is_thinned_to_the_trays_point_count():
    long = [(i / 19, float(i)) for i in range(20)]
    fitted = kit.fit_cap_points(long)
    assert len(fitted) == kit.CAP_POINTS
    assert fitted[0] == long[0] and fitted[-1] == long[-1]


def test_the_generator_refuses_a_lid_or_cheek_the_tray_cannot_draw():
    with pytest.raises(ValueError):
        kit.fit_cap_points([(0.0, 0.0)])
    with pytest.raises(ValueError):
        kit.check_cheek([(0.0, 0.0), (0.5, 1.0), (0.5, 2.0)])


def test_the_ams_ht_lid_clears_its_spool():
    profile = ams_ht._lid_profile()
    for y, z in profile:
        dy = y - ams_ht.SPOOL_Y
        if abs(dy) < ams_ht.SPOOL_R and z > ams_ht.BODY_Z + 0.01:
            assert z >= ams_ht.SPOOL_Z + (ams_ht.SPOOL_R**2 - dy**2) ** 0.5 - 0.002


def test_units_differ_by_their_fronts():
    def colors(unit):
        return {part[5] for part in (unit.tray.front or kit.classic_front(unit.tray))}

    assert colors(ace_pro.UNIT) != colors(cfs.UNIT)
    assert 0xee6a1f in colors(ace_pro.UNIT)
    assert 0xee6a1f not in colors(cfs.UNIT)


def _saturation(fill):
    r, g, b = kit.rgb(fill)
    return max(r, g, b) - min(r, g, b)


@pytest.mark.parametrize("tier", ["full", "thumb"])
def test_only_the_bmcu_levers_carry_colour(tier):
    scene = kit.Scene(kit.ART_CAMERA)
    bmcu.UNIT.draw_art(scene, ["#ffffff"] * 4, tier)
    shell = [i for i in scene.items if i.lane is None and i.fill.startswith("#") and len(i.fill) == 7]
    levers = [i for i in shell if i.tag == "lever"]
    assert len({tuple(i.points[0]) for i in levers}) >= 4
    assert all(_saturation(i.fill) > 40 for i in levers)
    assert not [i.fill for i in shell if i.tag != "lever" and _saturation(i.fill) > 12]


def test_the_bmcu_tray_front_is_neutral_but_for_its_levers():
    tinted = {part[5] for part in bmcu.UNIT.tray.front if _saturation(kit.hexc(kit.rgb(f"{part[5]:06x}"))) > 12}
    assert tinted == {int(bmcu.LEVER[1:], 16)}


def test_the_night_owl_spools_rest_on_the_base_and_share_its_width():
    assert night_owl.UNIT.lanes == 2
    half = night_owl.SPOOL_R / 0.42 * 0.294 / 2 / night_owl.SCALE
    for xc in night_owl.CENTERS:
        assert 0 < xc - half and xc + half < night_owl.W
    touch = math.hypot(night_owl.SPOOL_Y - night_owl.ROLLER_Y, night_owl.SPOOL_Z - night_owl.ROLLER_Z)
    assert touch == pytest.approx(night_owl.LOCAL_R + night_owl.ROLLER_R)


def test_the_ams_lite_hangs_two_spools_off_each_face_of_its_slab():
    seen = {}
    real = spool.draw_spool
    ams_lite.spool.draw_spool = lambda sc, center, radius, color, c, lane, **k: seen.setdefault(lane, center)
    try:
        ams_lite.UNIT.draw_art(kit.Scene(kit.ART_CAMERA), ["#ffffff"] * 4, "full")
    finally:
        ams_lite.spool.draw_spool = real
    width = ams_lite.SPOOL_R / 0.42 * 0.294
    for lanes, side in (((0, 1), -1), ((2, 3), 1)):
        for lane in lanes:
            assert side * seen[lane][0] - width / 2 > ams_lite.SLAB_T * ams_lite.SCALE
        assert {seen[lane][1] > 0 for lane in lanes} == {True, False}
    # Holders 1 and 4 share the high back end, 2 and 3 the low front one.
    assert seen[0][1:] == seen[3][1:] and seen[1][1:] == seen[2][1:]
    assert seen[0][2] > seen[1][2]


@pytest.mark.parametrize("unit", [bmcu, night_owl, ams_lite], ids=["bmcu", "night_owl", "ams_lite"])
def test_a_scaled_unit_draws_the_one_spool_size(unit):
    assert unit.SPOOL_R == 0.25
    seen = []
    real = spool.draw_spool
    unit.spool.draw_spool = lambda sc, center, radius, *a, **k: seen.append(radius)
    try:
        unit.UNIT.draw_art(kit.Scene(kit.ART_CAMERA), ["#ffffff"] * unit.UNIT.lanes, "full")
    finally:
        unit.spool.draw_spool = real
    assert seen == [0.25] * unit.UNIT.lanes


def test_a_scaled_scene_scales_lengths_and_strokes():
    scene = kit.Scene(kit.ART_CAMERA)
    kit.ScaledScene(scene, 0.5).line([(1.0, 0.0, 0.0), (2.0, 0.0, 0.0)], "#000000", 0.02)
    direct = kit.Scene(kit.ART_CAMERA)
    direct.line([(0.5, 0.0, 0.0), (1.0, 0.0, 0.0)], "#000000", 0.01)
    assert scene.items[0].points == direct.items[0].points
    assert scene.items[0].stroke_width == direct.items[0].stroke_width


def test_generated_shell_table_is_current():
    generated = REPO / "src/generated/ams_unit_shells_gen.cpp"
    assert generated.read_text() == build.generate_shell_table(build.UNITS)


def test_manifest_lists_every_lane_mask(tmp_path):
    def fake_render(svg_text, width, png_path):
        assert "<svg" in svg_text
        png_path.write_bytes(b"png")

    build.bake_unit(ace_pro.UNIT, tmp_path, render=fake_render)
    manifest = json.loads((tmp_path / ace_pro.UNIT.id / "manifest.json").read_text())
    assert manifest["lanes"] == 4
    for tier in ("full", "thumb"):
        t = manifest["tiers"][tier]
        assert len(t["lanes"]) == 4
        for name in [t["back"], t["front"]] + [p for lane in t["lanes"] for p in lane.values()]:
            assert (tmp_path / ace_pro.UNIT.id / name).is_file()


# Selectors that stand in front of the spools draw none: the rails and the HTLF.
SPOOL_UNITS = [u for u in build.UNITS if u.carriage_step is None and u.id != htlf.UNIT.id]
# Cabinets taller than the shared frame grow it, so their spools draw smaller.
TALL_UNITS = [yammu.UNIT]


@pytest.mark.parametrize("unit", [u for u in SPOOL_UNITS if u not in TALL_UNITS], ids=lambda u: u.id)
def test_every_spool_draws_at_one_size_and_the_unit_fits_its_frame(unit):
    scene = kit.Scene(kit.ART_CAMERA)
    unit.draw_art(scene, ["#ffffff"] * unit.lanes, "full")
    assert scene.spool_radius is not None
    vx, vy, vw, vh = scene._viewbox()
    assert vw / scene.spool_radius == pytest.approx(kit.ART_FRAME[0] / kit.ART_SPOOL_R)
    assert vh / scene.spool_radius == pytest.approx(kit.ART_FRAME[1] / kit.ART_SPOOL_R)
    xs = [p[0] for i in scene.items for p in i.points]
    ys = [p[1] for i in scene.items for p in i.points]
    assert vx <= min(xs) and max(xs) <= vx + vw
    assert vy <= min(ys) and max(ys) <= vy + vh


@pytest.mark.parametrize("lanes", emu.UNIT.lane_variants)
def test_a_variable_unit_draws_each_lane_count(lanes):
    scene = kit.Scene(kit.ART_CAMERA)
    emu.UNIT.draw_art(scene, ["#ffffff"] * lanes, "full")
    assert {i.lane for i in scene.items if i.lane is not None} == set(range(lanes))


@pytest.mark.parametrize("unit", [box_turtle.UNIT, box_turtle.ENCLOSED], ids=lambda u: u.id)
def test_a_box_turtle_is_drawn_with_two_to_eight_lanes(unit):
    assert unit.lane_variants == tuple(range(2, 9))
    c = spool.spool_constants(REPO)
    half = c["spool_width"] * (box_turtle.SPOOL_R / c["flange_radius"]) / 2
    for lanes in unit.lane_variants:
        scene = kit.Scene(kit.ART_CAMERA)
        unit.draw_art(scene, ["#ffffff"] * lanes, "full")
        assert {i.lane for i in scene.items if i.lane is not None} == set(range(lanes))
        centers = box_turtle.centers(lanes)
        assert centers[0] - half > 0 and centers[-1] + half < box_turtle.width(lanes)


def test_the_quattro_box_is_drawn_with_four_lanes_only():
    assert quattro_box.UNIT.lanes == 4
    assert quattro_box.UNIT.lane_variants == ()
    scene = kit.Scene(kit.ART_CAMERA)
    quattro_box.UNIT.draw_art(scene, ["#ffffff"] * 4, "full")
    assert {i.lane for i in scene.items if i.lane is not None} == set(range(4))


def test_quattro_spools_stay_inside_the_body():
    c = spool.spool_constants(REPO)
    half = c["spool_width"] * (quattro_box.SPOOL_R / c["flange_radius"]) / 2
    centers = quattro_box.centers()
    assert len(centers) == 4
    assert centers[0] - half > quattro_box.BEVEL
    assert centers[-1] + half < quattro_box.W - quattro_box.BEVEL
    assert quattro_box.SPOOL_Y + quattro_box.SPOOL_R < quattro_box.D
    assert quattro_box.SPOOL_Z + quattro_box.SPOOL_R < quattro_box.H


def test_a_variable_unit_bakes_one_set_per_lane_count(tmp_path):
    build.bake_unit(emu.UNIT, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    root = tmp_path / emu.UNIT.id
    assert not (root / "manifest.json").exists()
    for lanes in emu.UNIT.lane_variants:
        manifest = json.loads((root / str(lanes) / "manifest.json").read_text())
        assert manifest["lanes"] == lanes
        for tier in ("full", "thumb"):
            t = manifest["tiers"][tier]
            assert len(t["lanes"]) == lanes
            for name in [t["back"], t["front"]] + [p for lane in t["lanes"] for p in lane.values()]:
                assert (root / str(lanes) / name).is_file()


def test_the_shell_table_records_which_lane_counts_have_art():
    table = build.generate_shell_table([emu.UNIT, ace_pro.UNIT])

    def row(uid):
        return next(line for line in table.splitlines() if line.strip().startswith('{"%s"' % uid))

    assert ", 0x17e, " in row("emu")
    assert row("ace_pro").rstrip().endswith("0x0},")


def test_a_unit_wider_than_the_frame_widens_it_to_fit():
    scene = kit.Scene(kit.ART_CAMERA)
    emu.UNIT.draw_art(scene, ["#ffffff"] * 8, "full")
    vx, vy, vw, vh = scene._viewbox()
    assert vw / scene.spool_radius > kit.ART_FRAME[0] / kit.ART_SPOOL_R
    xs = [p[0] for i in scene.items for p in i.points]
    assert vx <= min(xs) and max(xs) <= vx + vw


@pytest.mark.parametrize("unit", TALL_UNITS, ids=lambda u: u.id)
def test_a_unit_taller_than_the_frame_grows_it_and_keeps_its_shape(unit):
    scene = kit.Scene(kit.ART_CAMERA)
    unit.draw_art(scene, ["#ffffff"] * unit.lanes, "full")
    vx, vy, vw, vh = scene._viewbox()
    assert vh / scene.spool_radius > kit.ART_FRAME[1] / kit.ART_SPOOL_R
    assert vw / vh == pytest.approx(kit.ART_FRAME[0] / kit.ART_FRAME[1])
    ys = [p[1] for i in scene.items for p in i.points]
    assert vy <= min(ys) and max(ys) <= vy + vh


def test_each_yammu_drawing_is_its_own_drawers_slice_of_the_cabinet():
    def slice_of(unit):
        scene = kit.Scene(kit.ART_CAMERA)
        unit.draw_art(scene, ["#ffffff"] * unit.lanes, "full")
        ys = [p[1] for i in scene.items if i.lane is not None for p in i.points]
        return sum(ys) / len(ys), any(i.tag == "grill" for i in scene.items)

    upper_lanes, upper_has_skirt = slice_of(yammu.UPPER)
    lower_lanes, lower_has_skirt = slice_of(yammu.LOWER)
    # Screen y grows downward: the upper drawer's lanes sit higher, and only the lower slice
    # stands on the skirt.
    assert upper_lanes < lower_lanes
    assert lower_has_skirt and not upper_has_skirt


def test_the_whole_yammu_cabinet_numbers_its_lanes_from_the_top_drawer_down():
    scene = kit.Scene(kit.ART_CAMERA)
    yammu.UNIT.draw_art(scene, ["#ffffff"] * 8, "full")

    def height(lanes):
        ys = [p[1] for i in scene.items if i.lane in lanes for p in i.points]
        return sum(ys) / len(ys)

    assert height(range(0, 4)) < height(range(4, 8))


FEEDERS = [bmcu.UNIT, mmx.UNIT, three_ms.UNIT, ad5x_ifs.UNIT, elegoo_canvas.UNIT]


@pytest.mark.parametrize("unit", FEEDERS, ids=lambda u: u.id)
def test_a_feeders_stub_look_frames_its_own_body_with_one_tube_per_lane(unit):
    scene = kit.Scene(kit.ART_CAMERA)
    unit.stub_art(scene, ["#ffffff"] * unit.lanes, "full")
    assert scene.spool_radius is None
    tubes = [i for i in scene.items if i.lane is not None]
    assert {i.lane for i in tubes} == set(range(unit.lanes))
    assert all(not i.closed for i in tubes)
    assert all(i.layer == "back" for i in tubes)


@pytest.mark.parametrize("unit", FEEDERS, ids=lambda u: u.id)
def test_a_feeders_spool_look_stands_in_the_shared_frame(unit):
    scene = kit.Scene(kit.ART_CAMERA)
    unit.draw_art(scene, ["#ffffff"] * unit.lanes, "full")
    assert scene.spool_radius == 0.25
    assert any(i.layer == "front" for i in scene.items)


@pytest.mark.parametrize("unit", FEEDERS, ids=lambda u: u.id)
def test_a_feeder_shows_a_slim_strip_and_no_enclosure(unit):
    t = unit.tray
    assert t.dome == []
    assert t.front_wall_px <= 16
    assert not t.lid_spans_row


def test_a_feeder_bakes_both_looks(tmp_path):
    build.bake_unit(mmx.UNIT, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    root = tmp_path / mmx.UNIT.id
    for sub in (root, root / "stubs"):
        assert json.loads((sub / "manifest.json").read_text())["lanes"] == mmx.UNIT.lanes
        assert (sub / "full_lane3.png").is_file()


def test_a_unit_without_a_stub_look_bakes_only_its_own_art(tmp_path):
    build.bake_unit(ace_pro.UNIT, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    assert not (tmp_path / ace_pro.UNIT.id / "stubs").exists()


def test_the_shell_table_records_which_units_have_a_stub_look():
    table = build.generate_shell_table([mmx.UNIT, ace_pro.UNIT])

    def row(uid):
        return next(line for line in table.splitlines() if line.strip().startswith(chr(123) + chr(34) + uid))

    assert ", 2, true, 0x0" in row("mmx")
    assert row("ace_pro").rstrip().endswith("false, 0x0},")


@pytest.mark.parametrize("unit", FEEDERS, ids=lambda u: u.id)
def test_a_tube_ends_clear_of_the_inlet_it_leaves(unit):
    scene = kit.Scene(kit.ART_CAMERA)
    unit.stub_art(scene, ["#ffffff"] * unit.lanes, "full")
    for tube in (i for i in scene.items if i.lane == 0):
        assert math.dist(tube.points[0], tube.points[-1]) > 0.08


def test_the_htlf_body_continues_below_its_outlets():
    assert htlf.PANEL_TOP > 0
    assert htlf.PANEL_TOP < htlf.OUTLET_Z < htlf.LID_Z0
    assert htlf.PROFILE[0] == (0.0, 0.0)


@pytest.mark.parametrize("unit", FEEDERS + [htlf.UNIT, rail.ERCF, rail.TRADRACK], ids=lambda u: u.id)
def test_a_unit_in_front_of_loose_spools_leaves_them_the_plain_tray(unit):
    table = build.generate_shell_table([unit, ace_pro.UNIT])
    shells = table.split("const UnitShell kShells[] = {")[1].split("};")[0]
    rows = {line.split(",")[0].strip(" {\""): line.rstrip() for line in shells.splitlines() if line.startswith("    {\"")}
    assert unit.tray.plain_tray and rows[unit.id].endswith("true},")
    assert not ace_pro.UNIT.tray.plain_tray and not rows["ace_pro"].endswith("true},")


def test_the_htlf_face_has_an_outlet_under_each_inlet_and_no_carriage(tmp_path):
    scene = build.face_scene(htlf.UNIT, 4)
    row = build.face_row(scene, 4)
    gates, aspect, gate0, step, inlet_y, out_x, out_y = (float(v.rstrip("f")) for v in row.strip("{}").split(", "))
    assert gates == 4 and step > 0
    assert out_x == gate0 and out_y > inlet_y
    assert not [i for i in scene.items if i.layer == "carriage"]
    build.bake_unit(htlf.UNIT, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    face = tmp_path / htlf.UNIT.id / "face" / "4"
    assert (face / "body.png").is_file() and not (face / "carriage.png").exists()


def test_the_htlf_stands_in_front_of_its_spools_so_it_draws_a_stub_per_lane_and_none():
    scene = kit.Scene(kit.ART_CAMERA)
    htlf.UNIT.draw_art(scene, ["#ffffff"] * htlf.UNIT.lanes, "full")
    assert scene.spool_radius is None
    assert {i.lane for i in scene.items if i.lane is not None} == set(range(htlf.UNIT.lanes))
    assert htlf.UNIT.stub_art is None


def test_the_canvas_shell_lets_its_dark_frame_show():
    scene = kit.Scene(kit.ART_CAMERA)
    elegoo_canvas.UNIT.stub_art(scene, ["#ffffff"] * 4, "full")
    panes = [i for i in scene.items if i.lane is None and i.closed and i.fill == elegoo_canvas.SMOKE]
    assert panes and all(i.opacity < 0.5 for i in panes)
    assert any(i.fill == elegoo_canvas.FRAME or i.fill.startswith("#0") or i.fill.startswith("#1") for i in scene.items)
RAILS = [rail.ERCF, rail.TRADRACK]


def _rail_scene(unit, gates, tier="full"):
    scene = kit.Scene(kit.ART_CAMERA)
    unit.draw_art(scene, ["#ffffff"] * gates, tier)
    return scene


@pytest.mark.parametrize("unit", RAILS, ids=lambda u: u.id)
@pytest.mark.parametrize("gates", (4, 6, 8, 9, 12))
def test_a_rail_draws_a_filament_stub_per_gate_and_no_spools(unit, gates):
    assert gates in unit.lane_variants
    scene = _rail_scene(unit, gates)
    assert {i.lane for i in scene.items if i.lane is not None} == set(range(gates))
    assert scene.spool_radius is None
    assert not [i for i in scene.items if i.lane is not None and i.layer != "back"]


@pytest.mark.parametrize("unit", RAILS, ids=lambda u: u.id)
def test_a_rail_runs_along_x_at_the_shared_camera(unit):
    assert unit.carriage_step(6) == (rail.PITCH, 0.0, 0.0)
    step_x, step_y = kit.carriage_step(_rail_scene(unit, 6), unit.carriage_step(6))
    yaw, pitch = math.radians(27), math.radians(20)
    assert step_x > 0
    assert step_y / step_x == pytest.approx(math.tan(yaw) * math.sin(pitch))


@pytest.mark.parametrize("unit", RAILS, ids=lambda u: u.id)
@pytest.mark.parametrize("gates", (4, 12))
def test_the_carriage_stays_in_the_frame_at_every_gate(unit, gates):
    scene = _rail_scene(unit, gates)
    carriage = [i for i in scene.items if i.layer == "carriage"]
    assert carriage and all(i.lane is None for i in carriage)
    vx, vy, vw, vh = scene._viewbox()
    step_x, step_y = kit.carriage_step(scene, unit.carriage_step(gates))
    for gate in (0, gates - 1):
        dx, dy = gate * step_x * vw, gate * step_y * vw
        for item in carriage:
            for x, y in item.points:
                assert vx <= x + dx <= vx + vw
                assert vy <= y + dy <= vy + vh


def test_the_carriage_bakes_to_its_own_layer_and_only_rails_have_one(tmp_path):
    build.bake_unit(rail.ERCF, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    build.bake_unit(ace_pro.UNIT, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    manifest = json.loads((tmp_path / rail.ERCF.id / "6" / "manifest.json").read_text())
    for tier in ("full", "thumb"):
        assert (tmp_path / rail.ERCF.id / "6" / manifest["tiers"][tier]["carriage"]).is_file()
    ace = json.loads((tmp_path / ace_pro.UNIT.id / "manifest.json").read_text())
    assert "carriage" not in ace["tiers"]["full"]


def test_the_shell_table_records_the_carriage_track_per_gate_count():
    table = build.generate_shell_table([rail.ERCF])
    scene = _rail_scene(rail.ERCF, 9)
    step_x, step_y = kit.carriage_step(scene, rail.ERCF.carriage_step(9))
    _, _, vw, vh = scene._viewbox()
    assert f"{{9, {step_x:.5f}f, {step_y:.5f}f, {vw / vh:.5f}f}}" in table
    assert "k_ercf_rail_carriage, 5," in table


def test_units_without_a_carriage_keep_their_table_row_shape():
    table = build.generate_shell_table([ace_pro.UNIT])
    assert "carriage" not in table


@pytest.mark.parametrize("unit", RAILS, ids=lambda u: u.id)
def test_a_rails_face_art_puts_every_gate_in_line_and_the_coupler_inside(unit):
    assert unit.tray.combiner == "internal"
    for gates in unit.lane_variants:
        scene = build.face_scene(unit, gates)
        row = build.face_row(scene, gates)
        _, aspect, gate0, step, inlet_y, out_x, out_y = (float(v.rstrip("f")) for v in row.strip("{}").split(", "))
        projected = [scene.camera.project(p) for p in scene.anchors.inlets]
        assert len(projected) == gates
        # One step per gate, at one height: the runtime places every inlet from gate 0's.
        assert all(abs((b[0] - a[0]) - (projected[1][0] - projected[0][0])) < 1e-9 for a, b in zip(projected, projected[1:]))
        assert all(abs(p[1] - projected[0][1]) < 1e-9 for p in projected)
        assert 0 < gate0 < gate0 + (gates - 1) * step < 1 and 0 < inlet_y < 1
        assert 0 < out_x < 1 and inlet_y < out_y < 1 and aspect > 1
    table = build.generate_shell_table([unit])
    assert f"constexpr RailFaceArt k_{unit.id}_face[] = {{{{4, " in table
    assert f"k_{unit.id}_face, {len(unit.lane_variants)}," in table


def test_a_rails_face_art_bakes_a_body_and_a_carriage_per_gate_count(tmp_path):
    build.bake_unit(rail.TRADRACK, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    for gates in rail.TRADRACK.lane_variants:
        assert sorted(f.name for f in (tmp_path / rail.TRADRACK.id / "face" / str(gates)).iterdir()) == ["body.png", "carriage.png"]
    build.bake_unit(ace_pro.UNIT, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    assert not (tmp_path / ace_pro.UNIT.id / "face").exists()


def test_a_unit_without_a_combiner_stops_the_table():
    unit = dataclasses.replace(ace_pro.UNIT, tray=dataclasses.replace(ace_pro.UNIT.tray, combiner=None))
    with pytest.raises(ValueError, match="ask Preston"):
        build.generate_shell_table([unit])


def _art_scene(unit, lanes=None):
    scene = kit.Scene(kit.ART_CAMERA)
    unit.draw_art(scene, ["#ffffff"] * (lanes or unit.lanes), "full")
    return scene


def _inside_cylinder(g, x, y, radius):
    """Whether (x, y), in shares of the drawing's width, lies on the lane's spool cylinder."""
    det = g.ey[0] * g.ez[1] - g.ey[1] * g.ez[0]

    def q(px, py):
        return ((px * g.ez[1] - py * g.ez[0]) / det, (g.ey[0] * py - g.ey[1] * px) / det)

    p = q(x - g.far[0], y - g.far[1])
    d = q(g.near[0] - g.far[0], g.near[1] - g.far[1])
    t = max(0.0, min(1.0, (p[0] * d[0] + p[1] * d[1]) / (d[0] ** 2 + d[1] ** 2)))
    return math.hypot(p[0] - t * d[0], p[1] - t * d[1]) <= radius


@pytest.mark.parametrize("unit", [u for u in SPOOL_UNITS if u.id in ("ace_pro", "box_turtle", "ams_2_pro", "qidi_box", "cfs")], ids=lambda u: u.id)
def test_every_lane_records_its_spool_and_outlet(unit):
    scene = _art_scene(unit)
    geoms = build.art_lane_geometry(scene, unit.lanes)
    assert len(geoms) == unit.lanes
    vx, vy, vw, vh = scene._viewbox()
    for lane, g in enumerate(geoms):
        # Wound filament sits between the core and the flange's rim.
        assert 0 < g.core < g.wound < 1
        # Every point of the lane's filament lies on its spool's cylinder.
        for item in scene.items:
            if item.lane != lane:
                continue
            for px, py in item.points:
                assert _inside_cylinder(g, (px - vx) / vw, (py - vy) / vw, 1.0001)
        # The outlet lies in the frame, below the spool's top.
        assert 0 <= g.outlet[0] <= 1
        assert g.box[1] < g.outlet[1] <= vh / vw
        # Lanes run left to right.
        if lane:
            assert geoms[lane - 1].box[0] < g.box[0]


def test_the_shell_table_carries_each_arts_lane_geometry():
    table = build.generate_shell_table(build.UNITS)
    assert "constexpr ArtLaneGeom k_box_turtle_4_lanes[]" in table
    assert '{"box_turtle", 4, ' in table
    # A rail draws no spools and records none.
    assert "k_ercf_rail_6_lanes" not in table


@pytest.mark.parametrize("unit", [u for u in SPOOL_UNITS if u.id in ("ace_pro", "box_turtle", "ams_2_pro", "qidi_box", "cfs")], ids=lambda u: u.id)
def test_the_bake_records_the_units_bottom_edge(unit):
    scene = _art_scene(unit)
    x0, y0, x1, y1 = build.art_bottom_edge(scene)
    vx, vy, vw, vh = scene._viewbox()
    assert x0 < x1
    # Nothing of the drawing hangs below the edge across its span.
    for item in scene.items:
        if item.blur:
            continue
        for px, py in item.points:
            sx, sy = (px - vx) / vw, (py - vy) / vw
            if x0 <= sx <= x1:
                assert sy <= y0 + (y1 - y0) * (sx - x0) / (x1 - x0) + 1e-6
    # It is the unit's front bottom edge: it runs along the row, as world x projects.
    cam = kit.ART_CAMERA
    ax, ay = cam.project((0, 0, 0))
    bx, by = cam.project((1, 0, 0))
    assert math.atan2(y1 - y0, x1 - x0) == pytest.approx(math.atan2(by - ay, bx - ax), abs=math.radians(5))
    # And it spans every lane's column.
    for g in build.art_lane_geometry(scene, unit.lanes):
        assert x0 <= g.outlet[0] <= x1


def test_the_shell_table_carries_each_arts_bottom_edge():
    table = build.generate_shell_table(build.UNITS)
    row = next(line for line in table.splitlines() if line.startswith('    {"box_turtle", 4, '))
    assert row.count("{") == 3  # the row, its drawn box and its bottom edge


def test_every_item_of_a_spool_belongs_to_its_lane():
    scene = _art_scene(box_turtle.UNIT)
    spool_items = [i for i in scene.items if i.spool is not None]
    # Flanges, core, filament and vents: far more than the lane's tintable filament alone.
    for lane in range(4):
        mine = [i for i in spool_items if i.spool == lane]
        tinted = [i for i in scene.items if i.lane == lane]
        assert len(mine) > len(tinted) > 0
        assert all(i.spool == lane for i in tinted)
    # Nothing outside draw_spool claims a spool.
    assert all(i.spool is None for i in scene.items if i.layer == "front")


@pytest.mark.parametrize("unit", SPOOL_UNITS, ids=lambda u: u.id)
def test_the_back_layer_and_each_lanes_stack_split_the_drawing_in_painter_order(unit):
    scene = _art_scene(unit)
    if not scene.spools:
        pytest.skip("draws no spools")
    back = scene.back_items()
    stacks = [scene.lane_items(lane) for lane in range(unit.lanes)]
    # Every back-layer item lands in exactly one place, in its drawing order.
    painted = back + [i for stack in stacks for i in stack]
    assert painted == [i for i in scene.items if i.layer == "back"]
    # The back holds no part of any spool; a lane's stack holds its whole spool, and lanes run
    # left to right, the farthest first.
    assert all(i.spool is None for i in back)
    for lane, stack in enumerate(stacks):
        assert stack and stack[0].spool == lane
        assert all(i.spool in (None, lane) for i in stack)
    centers = [scene.spools[lane]["far"][0] for lane in range(unit.lanes)]
    assert centers == sorted(centers)


def test_each_lane_bakes_its_spool_stack_and_full_tier_glow(tmp_path):
    def fake_render(svg_text, width, png_path):
        png_path.write_bytes(b"png")

    build.bake_unit(box_turtle.UNIT, tmp_path, render=fake_render)
    manifest = json.loads((tmp_path / "box_turtle" / "manifest.json").read_text())
    for tier in ("full", "thumb"):
        for lane, names in enumerate(manifest["tiers"][tier]["lanes"]):
            assert names["body"] == f"{tier}_lane{lane}_body.png"
            assert (tmp_path / "box_turtle" / names["body"]).is_file()
    for lane, names in enumerate(manifest["tiers"]["full"]["lanes"]):
        assert names["glow"] == f"full_lane{lane}_glow.png"
        # Its stacks hold nothing but spools, so a dim recolors the stack itself.
        assert "silhouette" not in names
    assert all("glow" not in n for n in manifest["tiers"]["thumb"]["lanes"])


def test_only_a_drawing_with_more_than_spools_in_its_stacks_bakes_silhouettes(tmp_path):
    def fake_render(svg_text, width, png_path):
        png_path.write_bytes(b"png")

    emu = next(u for u in build.UNITS if u.id == "emu")
    build.bake_unit(emu, tmp_path, render=fake_render)
    manifest = json.loads((tmp_path / "emu" / "2" / "manifest.json").read_text())
    for lane, names in enumerate(manifest["tiers"]["full"]["lanes"]):
        assert names["silhouette"] == f"full_lane{lane}_silhouette.png"
    table = build.generate_shell_table(build.UNITS)
    rows = [line for line in table.splitlines() if line.startswith("    {\"") and "_lanes" in line]
    assert any(r.startswith('    {"emu", 2, ') and r.endswith("_lanes, true},") for r in rows)
    assert any(r.startswith('    {"box_turtle", 4, ') and r.endswith("_lanes},") for r in rows)


def test_a_baked_png_is_recompressed_losslessly(tmp_path):
    from PIL import Image

    scene = _art_scene(box_turtle.UNIT)
    png = tmp_path / "back.png"
    subprocess.run(["rsvg-convert", "-w", "300", "-o", str(png), "/dev/stdin"], input=scene.layer_svg("back", 300).encode(), check=True)
    plain = png.read_bytes()
    before = Image.open(png)
    before.load()
    build.optimize_png(png)
    after = Image.open(png)
    after.load()
    assert len(png.read_bytes()) < len(plain)
    assert (after.mode, after.size) == (before.mode, before.size)
    assert after.tobytes() == before.tobytes()
