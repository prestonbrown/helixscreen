# SPDX-License-Identifier: GPL-3.0-or-later
"""The unit art kit: camera, spools tied to the app's values, baked layers."""
import json
import math
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from unit_art import build, kit, spool  # noqa: E402
from unit_art.units import ace_pro, ad5x_ifs, ams_ht, bmcu, box_turtle, cfs, elegoo_canvas, htlf, mmx, night_owl, quattro_box, rail, three_ms  # noqa: E402


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
    assert seen and all(c is kit.ART_CAMERA for c in seen)


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


@pytest.mark.parametrize("unit", [bmcu, night_owl], ids=["bmcu", "night_owl"])
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


@pytest.mark.parametrize("unit", [u for u in build.UNITS if u.carriage_step is None], ids=lambda u: u.id)
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


@pytest.mark.parametrize("lanes", quattro_box.UNIT.lane_variants)
def test_a_variable_unit_draws_each_lane_count(lanes):
    scene = kit.Scene(kit.ART_CAMERA)
    quattro_box.UNIT.draw_art(scene, ["#ffffff"] * lanes, "full")
    assert {i.lane for i in scene.items if i.lane is not None} == set(range(lanes))


def test_a_variable_units_width_is_two_caps_and_a_pitch_per_lane():
    widths = [quattro_box.width(n) for n in (2, 3, 4, 5)]
    assert widths[0] == pytest.approx(2 * quattro_box.CAP + quattro_box.PITCH)
    steps = {round(b - a, 6) for a, b in zip(widths, widths[1:])}
    assert steps == {round(quattro_box.PITCH, 6)}


@pytest.mark.parametrize("lanes", (2, 5))
def test_quattro_spools_stay_inside_the_body(lanes):
    c = spool.spool_constants(REPO)
    half = c["spool_width"] * (quattro_box.SPOOL_R / c["flange_radius"]) / 2
    centers = quattro_box.centers(lanes)
    assert len(centers) == lanes
    assert centers[0] - half > 0
    assert centers[-1] + half < quattro_box.width(lanes)
    assert quattro_box.SPOOL_Y + quattro_box.SPOOL_R < quattro_box.D
    assert quattro_box.SPOOL_Z + quattro_box.SPOOL_R < quattro_box.H


def test_a_variable_unit_bakes_one_set_per_lane_count(tmp_path):
    build.bake_unit(quattro_box.UNIT, tmp_path, render=lambda svg, w, path: path.write_bytes(b"png"))
    root = tmp_path / quattro_box.UNIT.id
    assert not (root / "manifest.json").exists()
    for lanes in quattro_box.UNIT.lane_variants:
        manifest = json.loads((root / str(lanes) / "manifest.json").read_text())
        assert manifest["lanes"] == lanes
        for tier in ("full", "thumb"):
            t = manifest["tiers"][tier]
            assert len(t["lanes"]) == lanes
            for name in [t["back"], t["front"]] + [p for lane in t["lanes"] for p in lane.values()]:
                assert (root / str(lanes) / name).is_file()


def test_the_shell_table_records_which_lane_counts_have_art():
    table = build.generate_shell_table([quattro_box.UNIT, ace_pro.UNIT])

    def row(uid):
        return next(line for line in table.splitlines() if line.strip().startswith('{"%s"' % uid))

    assert row("quattro_box").rstrip().endswith("0x34},")
    assert row("ace_pro").rstrip().endswith("0x0},")


def test_a_unit_wider_than_the_frame_widens_it_to_fit():
    scene = kit.Scene(kit.ART_CAMERA)
    quattro_box.UNIT.draw_art(scene, ["#ffffff"] * 5, "full")
    vx, vy, vw, vh = scene._viewbox()
    assert vw / scene.spool_radius > kit.ART_FRAME[0] / kit.ART_SPOOL_R
    xs = [p[0] for i in scene.items for p in i.points]
    assert vx <= min(xs) and max(xs) <= vx + vw


FEEDERS = [bmcu.UNIT, mmx.UNIT, htlf.UNIT, three_ms.UNIT, ad5x_ifs.UNIT, elegoo_canvas.UNIT]


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

    assert row("mmx").rstrip().endswith("true, 0x0},")
    assert row("ace_pro").rstrip().endswith("false, 0x0},")


@pytest.mark.parametrize("unit", FEEDERS, ids=lambda u: u.id)
def test_a_tube_ends_clear_of_the_inlet_it_leaves(unit):
    scene = kit.Scene(kit.ART_CAMERA)
    unit.stub_art(scene, ["#ffffff"] * unit.lanes, "full")
    for tube in (i for i in scene.items if i.lane == 0):
        assert math.dist(tube.points[0], tube.points[-1]) > 0.08


def test_the_htlf_body_continues_below_its_inlets():
    assert htlf.PANEL_TOP > 0
    assert htlf.INLET_Z - htlf.PANEL_TOP > 0
    assert htlf.PROFILE[0] == (0.0, 0.0)


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


def test_a_rail_keeps_the_selector_route_and_names_its_flat_face():
    assert all(u.tray.lane_join == "hub" for u in RAILS)
    table = build.generate_shell_table([rail.ERCF])
    assert "constexpr RailFace k_ercf_rail_face = {0xc8211f, " in table
    assert "&k_ercf_rail_face}" in table
