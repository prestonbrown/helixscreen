# SPDX-License-Identifier: GPL-3.0-or-later
"""
Power-loss recovery's save side (helix_plr.py): storage-safety verdict, the
PLR profile and switch, XY-homing safety, and the `plr` feature entry.

Run with: pytest tests/test_helix_plr.py -v
"""

import asyncio
import json
import os
import time
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))
sys.path.insert(0, str(Path(__file__).parent))

import helix_plr
from helix_plr import (
    HomingCheck,
    PlrProfile,
    StorageVerdict,
    check_xy_homing,
    classify_block_device,
    effective_tier,
    parse_profile,
    unlisted_guard,
    saving_enabled,
    storage_verdict,
    user_enabled,
)


def run(coro):
    return asyncio.run(coro)


# --------------------------------------------------------------------------
# A fake /sys
# --------------------------------------------------------------------------


class FakeSys:
    """Builds /sys/dev/block/<maj>:<min> links into a fake /sys/devices tree."""

    def __init__(self, root: Path):
        self.root = root
        (root / "dev" / "block").mkdir(parents=True)
        self.devices = root / "devices"
        self.devices.mkdir()
        self._minor = 0

    def disk(self, name, files=None):
        disk = self.devices / "platform" / "block" / name
        disk.mkdir(parents=True)
        for rel, text in (files or {}).items():
            path = disk / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text + "\n")
        return disk

    def link(self, target, major=179):
        self._minor += 1
        dev = os.makedev(major, self._minor)
        (self.root / "dev" / "block" / ("%d:%d" % (major, self._minor))).symlink_to(target)
        return dev

    def partition(self, disk, name):
        part = disk / name
        part.mkdir()
        (part / "partition").write_text("1\n")
        return part


@pytest.fixture
def fake_sys(tmp_path):
    return FakeSys(tmp_path / "sys")


@pytest.mark.parametrize(
    "name, files, expected",
    [
        ("mmcblk0", {"device/type": "MMC"}, "emmc"),
        ("mmcblk1", {"device/type": "SD"}, "sd"),
        ("mmcblk2", {}, "unknown"),
        ("nvme0n1", {}, "ssd"),
        ("sda", {"removable": "0", "queue/rotational": "0"}, "ssd"),
        ("sdb", {"removable": "1", "queue/rotational": "0"}, "unknown"),
        ("sdc", {"removable": "0", "queue/rotational": "1"}, "unknown"),
        ("ubiblock0_1", {}, "nand"),
        ("mtdblock3", {}, "nand"),
        ("dm-0", {}, "unknown"),
    ],
)
def test_classify_block_device(fake_sys, name, files, expected):
    disk = fake_sys.disk(name, files)
    assert classify_block_device(fake_sys.root, fake_sys.link(disk)) == (expected, name)


def test_partition_resolves_to_its_disk(fake_sys):
    disk = fake_sys.disk("mmcblk0", {"device/type": "MMC"})
    part = fake_sys.partition(disk, "mmcblk0p10")
    assert classify_block_device(fake_sys.root, fake_sys.link(part)) == ("emmc", "mmcblk0")


def test_no_block_device_is_unknown(fake_sys):
    assert classify_block_device(fake_sys.root, os.makedev(0, 42)) == ("unknown", None)


def test_unreadable_type_is_unknown(fake_sys):
    disk = fake_sys.disk("mmcblk0", {})
    (disk / "device").mkdir()
    (disk / "device" / "type").mkdir()  # reading a directory fails
    assert classify_block_device(fake_sys.root, fake_sys.link(disk)) == ("unknown", "mmcblk0")


@pytest.mark.parametrize(
    "runtime, hint, expected",
    [
        ("emmc", "sd", StorageVerdict("emmc", True, "runtime", "dev")),
        ("sd", "emmc", StorageVerdict("sd", False, "runtime", "dev")),
        ("ssd", "unknown", StorageVerdict("ssd", True, "runtime", "dev")),
        ("unknown", "emmc", StorageVerdict("emmc", True, "printer_db", "dev")),
        ("unknown", "sd", StorageVerdict("sd", False, "printer_db", "dev")),
        ("nand", "nand", StorageVerdict("nand", True, "printer_db", "dev")),
        ("nand", "unknown", StorageVerdict("nand", False, "runtime", "dev")),
        ("unknown", "unknown", StorageVerdict("unknown", False, "none", "dev")),
        ("unknown", "bogus", StorageVerdict("unknown", False, "none", "dev")),
    ],
)
def test_storage_verdict(runtime, hint, expected):
    assert storage_verdict(runtime, "dev", hint) == expected


@pytest.mark.parametrize(
    "wear, tier, user, expected",
    [
        (True, 1, None, True),
        (True, 2, None, True),
        (True, 0, None, False),
        (False, 2, None, False),
        (False, 0, True, True),
        (True, 2, False, False),
    ],
)
def test_saving_enabled(wear, tier, user, expected):
    verdict = StorageVerdict("emmc" if wear else "sd", wear, "runtime", None)
    assert saving_enabled(verdict, tier, user) is expected


# --------------------------------------------------------------------------
# Profile and switch
# --------------------------------------------------------------------------


def test_missing_profile_is_tier_zero_with_defaults():
    assert parse_profile(None) == PlrProfile()
    assert PlrProfile().tier == 0
    assert PlrProfile().xy_home_gcode == "G28 X Y"


def test_profile_reads_every_key():
    profile = parse_profile({
        "tier": 2, "backend": "qidi_print_state", "storage_hint": "emmc",
        "z_holds_unpowered": True, "xy_home_gcode": "HOME_XY", "z_lift_mm": 3,
        "purge_gcode": "PURGE", "resume_skip_fraction": 0.25,
        "macro_vars": {"_STATE": ["a", "b", 3]}, "nozzle_standby_drop_c": 50,
    })
    assert profile == PlrProfile(2, "qidi_print_state", "emmc", True, "HOME_XY", 3.0,
                                 "PURGE", 0.25, {"_STATE": ("a", "b")}, 50.0)


def test_profile_bad_types_take_defaults():
    profile = parse_profile({
        "tier": "2", "backend": "marlin", "storage_hint": 4, "z_holds_unpowered": 1,
        "xy_home_gcode": "  ", "z_lift_mm": -1, "purge_gcode": "", "resume_skip_fraction": 2,
        "macro_vars": [], "nozzle_standby_drop_c": -5,
    })
    assert profile == PlrProfile()
    assert parse_profile({"tier": 9}).tier == 3
    assert parse_profile({}).resume_skip_fraction == 0.0      # re-trace
    assert parse_profile({"tier": True}).tier == 0


@pytest.mark.parametrize(
    "record, expected",
    [(None, None), ({}, None), ({"enabled": "yes"}, None), ({"enabled": True}, True),
     ({"enabled": False}, False)],
)
def test_user_enabled(record, expected):
    assert user_enabled(record) is expected


# --------------------------------------------------------------------------
# XY homing
# --------------------------------------------------------------------------


@pytest.mark.parametrize(
    "settings, kin, expected",
    [
        ({}, "corexy", HomingCheck(True, None)),
        ({"homing_override": {"axes": "z", "gcode": "G28 Z"}}, "corexy", HomingCheck(True, None)),
        ({"homing_override": {"axes": "xyz", "gcode": "G28 X\nG28 Y"}}, "corexy",
         HomingCheck(True, None)),
        ({"homing_override": {"axes": "xyz", "gcode": "G28 X Y\nG1 X100 Y100\nG28 Z"}},
         "corexy", HomingCheck(False, "override_homes_z")),
        ({"homing_override": {"gcode": "G28 ; all"}}, "cartesian",
         HomingCheck(False, "override_homes_z")),
        ({"homing_override": {"axes": "xy", "gcode": ["G28 X0", "g28 z0"]}}, "corexy",
         HomingCheck(False, "override_homes_z")),
        ({}, "delta", HomingCheck(False, "delta_kinematics")),
    ],
)
def test_check_xy_homing(settings, kin, expected):
    assert check_xy_homing(settings, kin)[:2] == expected[:2]


def test_unsafe_homing_drops_tier_unless_the_profile_names_a_command():
    unsafe = HomingCheck(False, "override_homes_z")
    assert effective_tier(PlrProfile(tier=2), unsafe) == (0, "override_homes_z")
    assert effective_tier(PlrProfile(tier=2, xy_home_gcode="HOME_XY_ONLY"), unsafe) == (2, None)
    assert effective_tier(PlrProfile(tier=1), HomingCheck(True, None)) == (1, None)


# --------------------------------------------------------------------------
# The `plr` feature through helix_print
# --------------------------------------------------------------------------

from test_helix_print import (  # noqa: E402
    MockConfigHelper,
    MockDatabase,
    MockFileManager,
    MockHistory,
    MockKlippyApis,
    MockServer,
)
from helix_print import load_component  # noqa: E402


def make_helix(tmp_path, records=None):
    """A started component whose database holds `records`
    ({(namespace, key): value}) before component_init."""
    server = MockServer()
    gcodes = tmp_path / "printer_data" / "gcodes"
    gcodes.mkdir(parents=True)
    server.components["file_manager"] = MockFileManager(str(gcodes))
    server.components["database"] = MockDatabase()
    server.components["klippy_apis"] = MockKlippyApis()
    server.components["history"] = MockHistory()
    server.get_app_args = lambda: {"data_path": str(tmp_path / "printer_data")}
    for (namespace, key), value in (records or {}).items():
        run(server.components["database"].insert_item(namespace, key, value))
    component = load_component(MockConfigHelper(server, {
        "temp_dir": ".helix_temp", "symlink_dir": ".helix_print", "cleanup_delay": 3600,
        "enabled": True,
    }))
    run(component.component_init())
    return component


@pytest.fixture
def helix(tmp_path, fake_sys):
    component = make_helix(tmp_path)
    # start() probed the real /sys; the host's own disk must not decide the
    # storage verdict, so a test that wants one builds it in fake_sys.
    component.plr.sys_root = fake_sys.root
    component.plr.runtime_storage = component.plr.detect_storage()
    return component


def test_plr_feature_is_unavailable_without_a_profile(helix):
    status = run(helix._handle_status(None))
    assert status["features"]["plr"] == {
        "version": 3, "available": False, "enabled": False, "reason": "not_listed",
        "saving": False}


def test_plr_feature_is_unavailable_at_tier_0(helix):
    run(helix.database.insert_item("helix-screen", "plr_profile", {"tier": 0}))
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert (entry["available"], entry["reason"]) == (False, "not_listed")
    assert run(helix.feature_enabled("plr")) is False


def test_saving_stops_at_tier_0_even_when_the_user_turned_it_on(helix):
    db = helix.database
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 0}))
    run(db.insert_item("helix-screen", "plr_user", {"enabled": True}))
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert (entry["enabled"], entry["saving"]) == (False, False)
    assert run(helix.plr._saving()) is False


def test_plr_feature_defaults_off_on_a_listed_printer(helix):
    run(helix.database.insert_item("helix-screen", "plr_profile", {"tier": 1}))
    status = run(helix._handle_status(None))
    assert status["features"]["plr"] == {
        "version": 3, "available": True, "enabled": False, "reason": None, "saving": False}


def test_plr_feature_follows_a_profile_written_after_start(helix):
    db = helix.database
    assert run(helix._handle_status(None))["features"]["plr"]["available"] is False
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 2}))
    assert run(helix._handle_status(None))["features"]["plr"]["available"] is True
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 0}))
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert (entry["available"], entry["reason"]) == (False, "not_listed")


# --------------------------------------------------------------------------
# The default profile of a printer the app's database does not list
# --------------------------------------------------------------------------

# What the app writes for a printer without a `plr` block (#1863).
UNLISTED = {"tier": 1, "backend": "generic", "storage_hint": "unknown",
            "z_holds_unpowered": False, "listed": False, "reason": None}


def test_parse_profile_reads_listed_and_reason():
    assert parse_profile(UNLISTED).listed is False
    # Every record an app wrote before `listed` came from a database block.
    assert parse_profile({"tier": 1}).listed is True
    assert parse_profile({"tier": 1, "listed": "no"}).listed is True
    assert parse_profile({"tier": 0, "reason": "gantry_drops"}).reason == "gantry_drops"
    assert parse_profile({"tier": 0, "reason": ""}).reason is None
    assert parse_profile({"tier": 0, "reason": 7}).reason is None


@pytest.mark.parametrize(
    "settings, status, qidi, expected",
    [
        ({}, {}, False, None),
        ({"printer": {"kinematics": "corexy"}, "z_tilt": {}}, {}, False, None),
        ({"quad_gantry_level": {"gantry_corners": []}}, {}, False, "gantry_drops"),
        ({}, {"print_stats": {"state": "standby", "power_loss": 0}}, False, "firmware_recovery"),
        ({}, {"print_stats": {"state": "standby", "power_loss": None}}, False, None),
        ({}, {"virtual_sdcard": {"pl_env_valid": 0}}, False, "firmware_recovery"),
        ({}, {}, True, "firmware_recovery"),
        (None, None, False, None),
    ],
)
def test_unlisted_guard(settings, status, qidi, expected):
    assert unlisted_guard(settings, status, qidi) == expected


def ready_with(helix, settings=None, status=None, kinematics="corexy"):
    """Klippy ready on a printer whose configfile holds `settings` and whose
    subscription answers `status`."""
    apis = helix.klippy_apis
    cfg = {"printer": {"kinematics": kinematics}}
    cfg.update(settings or {})

    async def query(objects):
        return {"configfile": {"settings": cfg},
                "toolhead": {"axis_minimum": [0, 0, 0, 0], "axis_maximum": [220, 220, 250, 0]}}

    apis.query_objects.side_effect = query
    apis.get_object_list.return_value = ["print_stats", "virtual_sdcard", "toolhead"]
    apis.subscribe_objects.return_value = status or {}
    run(helix.plr.on_klippy_ready())


def plr_entry(helix):
    return run(helix._handle_status(None))["features"]["plr"]


def test_an_unlisted_printer_acts_at_tier_1(helix):
    run(helix.database.insert_item("helix-screen", "plr_profile", UNLISTED))
    ready_with(helix)
    entry = plr_entry(helix)
    assert (entry["available"], entry["reason"]) == (True, None)
    assert helix.plr.backend(run(helix.plr.profile())) == ("generic", 1)
    reply = run(helix.plr.status_reply())
    assert (reply["tier"], reply["backend"]) == (1, "generic")


def test_an_unlisted_printer_saves_nothing_by_default_on_an_sd_card(helix, fake_sys):
    disk = fake_sys.disk("mmcblk0", {"device/type": "SD"})
    helix.plr.runtime_storage = classify_block_device(fake_sys.root, fake_sys.link(disk))
    run(helix.database.insert_item("helix-screen", "plr_profile", UNLISTED))
    ready_with(helix)
    entry = plr_entry(helix)
    assert (entry["available"], entry["saving"]) == (True, False)


def test_an_unlisted_printer_saves_by_default_on_emmc(helix, fake_sys):
    disk = fake_sys.disk("mmcblk0", {"device/type": "MMC"})
    helix.plr.runtime_storage = classify_block_device(fake_sys.root, fake_sys.link(disk))
    run(helix.database.insert_item("helix-screen", "plr_profile", UNLISTED))
    ready_with(helix)
    assert plr_entry(helix)["saving"] is True


@pytest.mark.parametrize(
    "settings, status, reason",
    [
        ({"quad_gantry_level": {}}, None, "gantry_drops"),
        ({}, {"print_stats": {"power_loss": 0}}, "firmware_recovery"),
        ({}, {"virtual_sdcard": {"pl_env_valid": 1}}, "firmware_recovery"),
    ],
)
def test_an_unlisted_printer_klipper_rules_out_is_unavailable(helix, settings, status, reason):
    db = helix.database
    run(db.insert_item("helix-screen", "plr_profile", UNLISTED))
    run(db.insert_item("helix-screen", "plr_user", {"enabled": True}))
    events = []
    helix.server.send_event = lambda name, payload: events.append(name)
    ready_with(helix, settings, status)
    entry = plr_entry(helix)
    assert (entry["available"], entry["reason"], entry["saving"]) == (False, reason, False)
    assert helix.plr.backend(run(helix.plr.profile())) == ("none", 0)
    assert run(helix.plr._saving()) is False
    # The screens read the map before Klippy was ready; they read it again.
    assert "helix_print:helix_features_changed" in events


def test_a_listed_printer_keeps_its_tier_on_a_levelled_gantry(helix):
    run(helix.database.insert_item("helix-screen", "plr_profile",
                                   {"tier": 1, "listed": True}))
    ready_with(helix, {"quad_gantry_level": {}}, {"print_stats": {"power_loss": 0}})
    assert plr_entry(helix)["available"] is True
    assert helix.plr.backend(run(helix.plr.profile())) == ("generic", 1)


@pytest.mark.parametrize(
    "settings, kinematics, reason",
    [
        ({}, "delta", "delta_kinematics"),
        ({"homing_override": {"axes": "xyz", "gcode": "G28 Z\nG28 X Y"}}, "corexy",
         "override_homes_z"),
        ({"homing_override": {"gcode": "MY_HOME_ALL"}}, "corexy", "override_unknown"),
    ],
)
def test_an_unlisted_printer_with_unsafe_xy_homing_acts_at_tier_0(helix, settings, kinematics,
                                                                   reason):
    run(helix.database.insert_item("helix-screen", "plr_profile", UNLISTED))
    ready_with(helix, settings, kinematics=kinematics)
    assert helix.plr.homing.reason == reason
    assert helix.plr.backend(run(helix.plr.profile())) == ("none", 0)
    reply = run(helix.plr.status_reply())
    assert reply["tier"] == 0 and "not_supported" in reply["offer"]["blocked_by"]


@pytest.mark.parametrize("reason", ["gantry_drops", "delta_kinematics", "firmware_recovery"])
def test_a_model_listed_at_tier_0_reports_its_reason(helix, reason):
    run(helix.database.insert_item("helix-screen", "plr_profile",
                                   {"tier": 0, "listed": True, "reason": reason}))
    ready_with(helix)
    entry = plr_entry(helix)
    assert (entry["available"], entry["reason"]) == (False, reason)


def test_a_vendor_listing_never_acts(helix):
    run(helix.database.insert_item("helix-screen", "plr_profile",
                                   {"tier": 0, "backend": "vendor", "listed": True,
                                    "reason": "firmware_recovery"}))
    ready_with(helix, {}, {"print_stats": {"power_loss": 0}})
    assert plr_entry(helix)["reason"] == "firmware_recovery"
    assert helix.plr.backend(run(helix.plr.profile())) == ("none", 0)


def test_plr_profile_tier_is_read_at_init(tmp_path, fake_sys):
    component = make_helix(tmp_path, {("helix-screen", "plr_profile"): {"tier": 1}})
    # The sync probe alone, with no status read since init.
    assert component.plr.availability() == (True, None)


def test_plr_feature_follows_storage_tier_and_switch(helix, fake_sys):
    db = helix.database
    disk = fake_sys.disk("mmcblk0", {"device/type": "MMC"})
    helix.plr.sys_root = fake_sys.root
    helix.plr.runtime_storage = classify_block_device(fake_sys.root, fake_sys.link(disk))
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 1}))
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert entry["enabled"] is True and entry["saving"] is True
    run(db.insert_item("helix-screen", "plr_user", {"enabled": False}))
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert entry["enabled"] is False and entry["saving"] is False
    # plugin_features is not PLR's switch
    run(db.insert_item("helix-screen", "plugin_features", {"plr": True}))
    assert run(helix._handle_status(None))["features"]["plr"]["enabled"] is False


def test_plr_feature_on_sd_needs_the_user(helix, fake_sys):
    db = helix.database
    disk = fake_sys.disk("mmcblk0", {"device/type": "SD"})
    helix.plr.runtime_storage = classify_block_device(fake_sys.root, fake_sys.link(disk))
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 2, "storage_hint": "emmc"}))
    assert run(helix._handle_status(None))["features"]["plr"]["enabled"] is False
    run(db.insert_item("helix-screen", "plr_user", {"enabled": True}))
    assert run(helix._handle_status(None))["features"]["plr"]["enabled"] is True


def test_plr_feature_reports_why_it_cannot_run(helix):
    helix.plr.has_virtual_sdcard = False
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert (entry["available"], entry["enabled"], entry["reason"]) == (
        False, False, "no_virtual_sdcard")


def test_storage_detection_reads_the_data_dir(tmp_path, fake_sys, monkeypatch):
    disk = fake_sys.disk("mmcblk0", {"device/type": "MMC"})
    part = fake_sys.partition(disk, "mmcblk0p10")
    dev = fake_sys.link(part)
    real_stat = os.stat

    def fake_stat(path, *a, **k):
        st = real_stat(path, *a, **k)
        if str(path) == str(tmp_path):
            return os.stat_result((st.st_mode, st.st_ino, dev) + tuple(st)[3:])
        return st

    monkeypatch.setattr(helix_plr.os, "stat", fake_stat)

    class Stub:
        server = MockServer()
        database = None

    plr = helix_plr.HelixPlr(Stub(), tmp_path, sys_root=fake_sys.root)
    plr.start()
    assert plr.runtime_storage == ("emmc", "mmcblk0")


# --------------------------------------------------------------------------
# Slot store
# --------------------------------------------------------------------------

from helix_plr import (  # noqa: E402
    SLOT_PAYLOAD_MAX,
    SLOT_SIZE,
    SavePolicy,
    SlotStore,
    build_record,
    decode_slot,
    encode_slot,
    fit_record,
    mesh_cfg,
    mesh_record,
    snapshot_objects,
)


@pytest.fixture
def store(tmp_path):
    s = SlotStore(tmp_path / "helix-plr" / "snapshot.bin")
    s.open()
    yield s
    s.close()


def payload(n):
    return json.dumps({"n": n}).encode()


def test_slot_round_trip_and_bounds():
    assert decode_slot(encode_slot(7, b'{"a":1}')) == (7, b'{"a":1}')
    assert len(encode_slot(1, b"x" * SLOT_PAYLOAD_MAX)) == SLOT_SIZE
    with pytest.raises(ValueError):
        encode_slot(1, b"x" * (SLOT_PAYLOAD_MAX + 1))
    assert decode_slot(b"\0" * SLOT_SIZE) is None


def test_store_is_preallocated_and_alternates(store):
    assert store.path.stat().st_size == 2 * SLOT_SIZE
    assert store.load() is None
    for n in range(1, 6):
        assert store.write(payload(n)) == n
        assert store.load() == {"n": n, "seq": n}
    assert store.path.stat().st_size == 2 * SLOT_SIZE


def test_reopen_continues_the_sequence(tmp_path):
    path = tmp_path / "snapshot.bin"
    a = SlotStore(path)
    a.open()
    a.write(payload(1))
    a.write(payload(2))
    a.close()
    b = SlotStore(path)
    b.open()
    assert b.seq == 2
    assert b.write(payload(3)) == 3
    assert b.load() == {"n": 3, "seq": 3}
    b.close()


def _damage(path, slot, how):
    data = bytearray(path.read_bytes())
    base = slot * SLOT_SIZE
    if how == "crc":
        data[base + 22] ^= 0xFF  # inside the payload
    elif how == "truncated":
        for i in range(base + 24, base + SLOT_SIZE):
            data[i] = 0
    elif how == "magic":
        data[base] = ord("X")
    elif how == "length":
        data[base + 12:base + 16] = (SLOT_SIZE * 2).to_bytes(4, "little")
    path.write_bytes(bytes(data))


@pytest.mark.parametrize("how", ["crc", "truncated", "magic", "length"])
def test_a_torn_newer_slot_falls_back_to_the_older(store, how):
    store.write(payload(1))  # slot 1
    store.write(payload(2))  # slot 0
    _damage(store.path, 0, how)
    assert store.load() == {"n": 1, "seq": 1}


@pytest.mark.parametrize("how", ["crc", "truncated"])
def test_a_torn_older_slot_does_not_matter(store, how):
    store.write(payload(1))
    store.write(payload(2))
    _damage(store.path, 1, how)
    assert store.load() == {"n": 2, "seq": 2}


def test_both_slots_torn_is_no_snapshot(store):
    store.write(payload(1))
    store.write(payload(2))
    _damage(store.path, 0, "crc")
    _damage(store.path, 1, "crc")
    assert store.load() is None


def test_clear_leaves_no_snapshot(store):
    store.write(payload(1))
    store.clear()
    assert store.load() is None


def test_a_wrong_sized_file_is_reinitialised(tmp_path):
    path = tmp_path / "snapshot.bin"
    path.write_bytes(b"junk")
    s = SlotStore(path)
    s.open()
    assert path.stat().st_size == 2 * SLOT_SIZE
    assert s.load() is None
    s.close()


def test_oversized_records_move_objects_and_macro_vars_out():
    small = {"layer": 3, "objects": [], "macro_vars": {}}
    data, moved = fit_record(small)
    assert moved == {} and json.loads(data) == small
    big = {"layer": 3, "objects": [{"name": "o%d" % i, "polygon": [[i, i]] * 20}
                                   for i in range(40)], "macro_vars": {"M": {"v": 1}}}
    data, moved = fit_record(big)
    assert len(data) <= SLOT_PAYLOAD_MAX
    assert json.loads(data) == {"layer": 3, "overflow": ["macro_vars", "objects"]}
    assert moved == {"objects": big["objects"], "macro_vars": {"M": {"v": 1}}}


# --------------------------------------------------------------------------
# Record, mesh and policy
# --------------------------------------------------------------------------

STATUS = {
    "print_stats": {"state": "printing", "print_duration": 120.5, "filament_used": 33.0,
                    "info": {"current_layer": 4, "total_layer": 90}},
    "virtual_sdcard": {"file_position": 123456},
    "toolhead": {"position": [10, 20, 0.8, 500], "extruder": "extruder", "max_velocity": 300,
                 "max_accel": 5000, "square_corner_velocity": 5, "minimum_cruise_ratio": 0.5},
    "motion_report": {"live_position": [9.5, 19.5, 0.8, 498.2]},
    "gcode_move": {"gcode_position": [10, 20, 0.6, 12], "homing_origin": [0, 0, 0.2, 0],
                   "absolute_coordinates": True, "absolute_extrude": False, "speed": 100,
                   "speed_factor": 1.0, "extrude_factor": 0.95},
    "extruder": {"target": 220, "temperature": 219.6, "pressure_advance": 0.04,
                 "smooth_time": 0.04},
    "extruder_stepper belt": {"pressure_advance": 0.1},
    "heater_bed": {"target": 60, "temperature": 60.1},
    "heater_generic chamber": {"target": 40, "temperature": 38},
    "fan": {"speed": 0.5},
    "fan_generic aux": {"speed": 0.2},
    "firmware_retraction": {"retract_length": 0.8, "retract_speed": 35,
                            "unretract_extra_length": 0, "unretract_speed": 30},
    "exclude_object": {"objects": [{"name": "A"}, {"name": "B"}], "excluded_objects": ["B"]},
    "gcode_macro _STATE": {"count": 3, "other": 1},
    "bed_mesh": {"profile_name": "default", "mesh_min": [10, 10], "mesh_max": [200, 200],
                 "probed_matrix": [[0.1, 0.2], [0.0, -0.1]],
                 "profiles": {"default": {"mesh_params": {"x_count": 2}}}},
}


def test_build_record_reads_every_section_4_1_field():
    r = build_record(STATUS, 1000.0, {"_STATE": ("count",)})
    assert r == {
        "schema": 1, "saved_at": 1000.0, "file_position": 123456,
        "toolhead_pos": [10.0, 20.0, 0.8, 500.0], "live_pos": [9.5, 19.5, 0.8, 498.2],
        "gcode_pos": [10.0, 20.0, 0.6, 12.0], "layer": 4, "total_layers": 90,
        "print_duration": 120.5, "filament_used": 33.0,
        "gcode_offset": [0.0, 0.0, 0.2, 0.0], "abs_coord": True, "abs_extrude": False,
        "speed": 100.0, "speed_factor": 1.0, "extrude_factor": 0.95,
        "heaters": {"extruder": {"target": 220.0, "temp": 219.6},
                    "heater_bed": {"target": 60.0, "temp": 60.1},
                    "heater_generic chamber": {"target": 40.0, "temp": 38.0}},
        "fans": {"fan": 0.5, "fan_generic aux": 0.2},
        "active_extruder": "extruder",
        "limits": {"velocity": 300.0, "accel": 5000.0, "scv": 5.0, "min_cruise_ratio": 0.5},
        "pa": {"extruder": {"advance": 0.04, "smooth_time": 0.04}},
        "retraction": {"length": 0.8, "speed": 35.0, "extra": 0.0, "unretract_speed": 30.0},
        "objects": [{"name": "A"}, {"name": "B"}], "excluded": ["B"],
        "macro_vars": {"_STATE": {"count": 3}}, "power_fail_seen": False,
    }


def test_build_record_tolerates_missing_objects():
    r = build_record({}, 5.0)
    assert r["file_position"] is None and r["retraction"] is None and r["heaters"] == {}


def test_snapshot_objects_subscribes_the_families():
    subs = snapshot_objects(["print_stats", "toolhead", "extruder", "extruder1",
                             "extruder_stepper belt", "heater_generic chamber", "fan_generic x",
                             "gcode_macro M", "virtual_sdcard", "webhooks"])
    assert sorted(subs) == ["extruder", "extruder1", "fan_generic x", "gcode_macro M",
                            "heater_generic chamber", "print_stats", "toolhead", "virtual_sdcard"]
    assert all(v is None for v in subs.values())


def test_mesh_record_and_cfg():
    saved = mesh_record(STATUS)
    assert (saved["profile"], saved["saved"], saved["params"]) == ("default", True, {"x_count": 2})
    adaptive = dict(STATUS, bed_mesh=dict(STATUS["bed_mesh"], profile_name="adaptive-1"))
    unsaved = mesh_record(adaptive)
    assert unsaved["saved"] is False
    text = mesh_cfg(unsaved)
    assert text.splitlines()[:5] == ["[bed_mesh helix_plr]", "version = 1", "points =",
                                     "\t  0.100000, 0.200000", "\t  0.000000, -0.100000"]
    assert "x_count = 2" in text and "algo = lagrange" in text and "max_y = 200.000000" in text
    big = dict(unsaved, points=[[0.0] * 7] * 7)
    assert "algo = bicubic" in mesh_cfg(big)
    assert mesh_record({"bed_mesh": {"probed_matrix": [[]]}}) is None


def rec(pos, **kw):
    r = build_record(STATUS, 0.0)
    r["file_position"] = pos
    r.update(kw)
    return r


def test_policy_interval_and_events():
    p = SavePolicy(1.0)
    assert p.decide(0.0, "printing", rec(10)) == "first"
    p.saved(0.0, rec(10))
    assert p.decide(0.5, "printing", rec(20)) is None       # under the interval
    assert p.decide(1.0, "printing", rec(10)) is None       # position unchanged
    assert p.decide(1.0, "printing", rec(30)) == "interval"
    p.saved(1.0, rec(30))
    assert p.decide(1.1, "printing", rec(31, layer=5)) == "event"
    assert p.decide(1.1, "printing", rec(31, excluded=["A", "B"])) == "event"
    assert p.decide(1.1, "printing", rec(31, speed_factor=1.2)) == "event"
    assert p.decide(1.1, "printing", rec(31, gcode_offset=[0, 0, 0.25, 0])) == "event"
    hot = rec(31)
    hot["heaters"] = dict(hot["heaters"], extruder={"target": 230.0, "temp": 219.0})
    assert p.decide(1.1, "printing", hot) == "event"
    warm = rec(31)
    warm["heaters"] = dict(warm["heaters"], extruder={"target": 220.0, "temp": 225.0})
    assert p.decide(1.1, "printing", warm) is None          # a temperature is not an event
    assert p.decide(1.1, "printing", rec(31, fans={"fan": 1.0})) == "event"


def test_policy_saves_once_on_pause_and_never_idle():
    p = SavePolicy(1.0)
    p.decide(0.0, "printing", rec(10))
    p.saved(0.0, rec(10))
    assert p.decide(0.2, "paused", rec(10)) == "pause"
    assert p.decide(5.0, "paused", rec(10)) is None
    assert p.decide(9.0, "standby", rec(99)) is None
    assert p.decide(9.0, "complete", rec(99)) is None


# --------------------------------------------------------------------------
# The snapshot loop inside the component
# --------------------------------------------------------------------------


class Clock:
    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t


@pytest.fixture
def plr(helix, tmp_path):
    config = tmp_path / "printer_data" / "config"
    config.mkdir()
    fm = helix.file_manager
    fm.get_directory = lambda name: str(config) if name == "config" else fm._gcodes_path
    db = helix.database
    run(db.insert_item("helix-screen", "plr_profile", {"tier": 2, "z_holds_unpowered": True}))
    run(db.insert_item("helix-screen", "plr_user", {"enabled": True}))
    (helix.gc_path / "sub").mkdir()
    (helix.gc_path / "sub" / "part.gcode").write_bytes(b"G1 X1\n" * 50000)
    p = helix.plr
    p.clock = Clock()
    return p


def added(plr, job_id="0000A1", filename="sub/part.gcode"):
    run(plr.on_history_changed({"action": "added", "job": {"job_id": job_id, "filename": filename}}))


def finished(plr, status, job_id="0000A1"):
    run(plr.on_history_changed({"action": "finished",
                                "job": {"job_id": job_id, "status": status}}))


def tick(plr, dt=1.0, **objects):
    plr.clock.t += dt
    run(plr.on_status_update(objects))


def printing_status(position, layer=1, state="printing"):
    s = json.loads(json.dumps(STATUS))
    s["print_stats"]["state"] = state
    s["print_stats"]["info"]["current_layer"] = layer
    s["virtual_sdcard"]["file_position"] = position
    return s


def test_job_start_records_identity(plr, helix):
    plr.store.write(b'{"stale": 1}')
    added(plr)
    job = run(helix.database.get_item("helix_plr", "job"))
    path = helix.gc_path / "sub" / "part.gcode"
    assert job["job_id"] == "0000A1" and job["filename"] == "sub/part.gcode"
    assert job["file_size"] == path.stat().st_size
    assert job["file_hash"] == helix_plr.file_hash(path)
    assert run(helix.database.get_item("helix_plr", "newest_job")) == {
        "job_id": "0000A1", "filename": "sub/part.gcode"}
    assert plr.store.load() is None


def test_file_hash_reads_both_ends(tmp_path):
    a = tmp_path / "a.gcode"
    b = tmp_path / "b.gcode"
    body = bytearray(b"G1 X1\n" * 50000)
    a.write_bytes(bytes(body))
    body[-3] = ord("9")
    b.write_bytes(bytes(body))
    assert helix_plr.file_hash(a) != helix_plr.file_hash(b)


def test_snapshots_are_saved_while_printing(plr):
    added(plr)
    tick(plr, **printing_status(100))
    assert plr.store.load()["file_position"] == 100
    tick(plr, 0.3, **printing_status(200))
    assert plr.store.load()["file_position"] == 100          # within the interval
    tick(plr, 1.0, **printing_status(300))
    assert plr.store.load()["file_position"] == 300
    tick(plr, 0.1, **printing_status(310, layer=2))
    assert plr.store.load()["layer"] == 2                    # layer change saves at once


def test_nothing_is_saved_when_saving_is_off_or_idle(plr, helix):
    added(plr)
    tick(plr, **printing_status(100, state="standby"))
    assert plr.store.load() is None
    run(helix.database.insert_item("helix-screen", "plr_user", {"enabled": False}))
    plr._switch = (True, float("-inf"))
    tick(plr, **printing_status(200))
    assert plr.store.load() is None


def test_nothing_is_saved_without_a_job(plr):
    tick(plr, **printing_status(100))
    assert plr.store.load() is None


@pytest.mark.parametrize("status", ["completed", "cancelled"])
def test_finished_jobs_clear_the_snapshot(plr, helix, status):
    added(plr)
    tick(plr, **printing_status(100))
    finished(plr, status)
    assert plr.store.load() is None
    assert run(helix.database.get_item("helix_plr", "job")) is None


@pytest.mark.parametrize("status", ["error", "klippy_shutdown", "klippy_disconnect"])
def test_failed_jobs_keep_the_snapshot(plr, helix, status):
    added(plr)
    tick(plr, **printing_status(100))
    finished(plr, status)
    assert plr.store.load()["file_position"] == 100
    assert run(helix.database.get_item("helix_plr", "job"))["finished_status"] == status


def test_another_jobs_finish_is_ignored(plr):
    added(plr)
    tick(plr, **printing_status(100))
    finished(plr, "completed", job_id="0000FF")
    assert plr.store.load()["file_position"] == 100


def test_the_job_record_survives_a_restart(plr, helix):
    added(plr)
    plr.job = None
    run(plr.load_job())
    assert plr.job["job_id"] == "0000A1"


def test_mesh_is_kept_after_layer_one(plr, helix, tmp_path):
    added(plr)
    adaptive = printing_status(100)
    adaptive["bed_mesh"]["profile_name"] = "adaptive"
    tick(plr, **adaptive)
    assert "mesh" not in plr.job
    adaptive["print_stats"]["info"]["current_layer"] = 2
    tick(plr, **adaptive)
    job = run(helix.database.get_item("helix_plr", "job"))
    assert job["mesh"]["saved"] is False and job["mesh"]["profile"] == "adaptive"
    cfg = tmp_path / "printer_data" / "config" / "helix-plr" / "mesh.cfg"
    assert cfg.read_text().startswith("[bed_mesh helix_plr]\n")
    finished(plr, "completed")
    assert not cfg.exists()


def test_a_saved_mesh_writes_no_cfg(plr, tmp_path):
    added(plr)
    tick(plr, **printing_status(100, layer=2))
    assert plr.job["mesh"]["saved"] is True
    assert not (tmp_path / "printer_data" / "config" / "helix-plr" / "mesh.cfg").exists()


def test_overflowing_objects_go_to_the_job_record(plr, helix):
    added(plr)
    s = printing_status(100)
    s["exclude_object"]["objects"] = [{"name": "obj%d" % i, "polygon": [[i, i]] * 30}
                                      for i in range(30)]
    tick(plr, **s)
    assert plr.store.load()["overflow"] == ["macro_vars", "objects"]
    job = run(helix.database.get_item("helix_plr", "job"))
    assert len(job["overflow"]["objects"]) == 30


def test_klippy_ready_subscribes_and_checks_virtual_sdcard(plr, helix):
    apis = helix.klippy_apis
    apis.get_object_list.return_value = ["print_stats", "toolhead", "extruder", "webhooks"]
    apis.subscribe_objects.return_value = {"toolhead": {"extruder": "extruder"}}
    run(plr.on_klippy_ready())
    assert plr.has_virtual_sdcard is False
    subs = apis.subscribe_objects.call_args[0][0]
    assert sorted(subs) == ["extruder", "print_stats", "toolhead"]
    assert plr.status == {"toolhead": {"extruder": "extruder"}}


def test_every_write_is_synced(store, monkeypatch):
    synced = []
    real = helix_plr._sync_data
    monkeypatch.setattr(helix_plr, "_sync_data", lambda fd: (synced.append(fd), real(fd)))
    store.write(payload(1))
    store.write(payload(2))
    assert len(synced) == 2


def test_a_platform_without_fdatasync_syncs_with_fsync(store, monkeypatch):
    synced = []
    real = os.fsync
    monkeypatch.delattr(helix_plr.os, "fdatasync", raising=False)
    monkeypatch.setattr(helix_plr.os, "fsync", lambda fd: (synced.append(fd), real(fd)))
    store.write(payload(1))
    assert synced == [store._fd]


def test_idle_updates_do_not_read_the_switch(plr):
    added(plr)

    async def boom():
        raise AssertionError("switch read while idle")

    plr.saving_enabled = boom
    tick(plr, **printing_status(100, state="standby"))
    tick(plr, **printing_status(100, state="complete"))


# --------------------------------------------------------------------------
# Layer follower
# --------------------------------------------------------------------------

import io  # noqa: E402

import helix_gcode_reader  # noqa: E402
from helix_plr import LayerFollower  # noqa: E402


def layered_gcode(layers=40, lines=30):
    out = ["M83"]
    for n in range(1, layers + 1):
        out.append(";LAYER_CHANGE")
        out.append("G1 Z%.1f" % (n * 0.2))
        out.extend("G1 X%d Y%d E.05" % (i, n) for i in range(lines))
    return ("\n".join(out) + "\n").encode()


class NoThrottle:
    def __init__(self):
        self.calls = 0

    def pace(self, n):
        self.calls += 1


@pytest.fixture
def gfile(tmp_path):
    path = tmp_path / "gcodes" / "part.gcode"
    path.parent.mkdir()
    path.write_bytes(layered_gcode())
    return path


def ident(path):
    return helix_plr.file_identity(path)


def full_scan_starts(data):
    starts = {}
    helix_gcode_reader.scan_to_offset(
        io.BytesIO(data), len(data),
        on_layer_start=lambda layer, off, st: starts.__setitem__(layer, (off, st)))
    return starts


def test_follower_builds_the_index_in_steps(gfile):
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    f = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    for pos in (1000, 5000, 5001, len(data) // 2, len(data)):
        f.step(pos)
    assert f.offset == len(data)
    f.finish(len(data))
    summary = helix_gcode_reader.read_layer_index(index, ident(gfile))
    assert summary.complete is True
    expected = full_scan_starts(data)
    for layer in (1, 17, 40):
        cp = helix_gcode_reader.read_layer_index(index, ident(gfile),
                                                 max_offset=expected[layer][0])
        assert cp.at_or_before.layer == layer
        assert cp.at_or_before.offset == expected[layer][0]
        assert cp.at_or_before.state == expected[layer][1]


def test_follower_never_reads_past_file_position(gfile):
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    f = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    f.step(3000)
    assert f.offset <= 3000
    summary = helix_gcode_reader.read_layer_index(index, ident(gfile))
    assert summary.last.offset < 3000


def test_follower_stops_once_a_complete_index_appears(gfile, monkeypatch):
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    f = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    f.step(0)                                       # loaded: no index yet
    helix_gcode_reader.write_layer_index(index, ident(gfile), [], [], len(data))
    before = open(index, "rb").read()
    f.step(len(data) // 4)
    assert f.complete
    reads = []
    scan = helix_gcode_reader.scan_to_offset
    monkeypatch.setattr(helix_gcode_reader, "scan_to_offset",
                        lambda *a, **k: (reads.append(a), scan(*a, **k))[1])
    f.step(len(data) // 2)
    f.finish(None)
    assert reads == [] and open(index, "rb").read() == before


def test_follower_resumes_from_its_index_after_a_restart(gfile):
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    a = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    a.step(len(data) // 2)
    a.finish(len(data))
    b = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    b.step(0)
    assert b.offset == helix_gcode_reader.read_layer_index(index, ident(gfile)).last.offset
    b.step(len(data))
    b.finish(len(data))
    lines = open(index).read().splitlines()
    layers = [json.loads(l)["layer"] for l in lines[1:] if "layer" in json.loads(l)]
    assert layers == list(range(1, 41))          # no layer twice, none missing


def test_a_complete_index_is_not_followed_again(gfile):
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    a = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    a.step(len(data))
    a.finish(len(data))
    throttle = NoThrottle()
    b = LayerFollower(gfile, index, ident(gfile), limiter=throttle)
    b.step(len(data))
    assert b.complete is True and b.offset < len(data) and throttle.calls == 0


def test_follower_is_throttled(gfile, monkeypatch):
    monkeypatch.setattr(helix_gcode_reader, "CHECK_INTERVAL_BYTES", 256)
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    throttle = NoThrottle()
    LayerFollower(gfile, index, ident(gfile), limiter=throttle).step(len(data))
    assert throttle.calls >= len(data) // 256 - 2 > 10


class SyncExecutor:
    """Runs follower steps at once on the calling thread."""

    def submit(self, fn, *args):
        from concurrent.futures import Future
        fut = Future()
        try:
            fut.set_result(fn(*args))
        except Exception as e:
            fut.set_exception(e)
        return fut

    def shutdown(self, wait=True):
        pass


def test_component_follows_the_print_and_closes_the_index(plr, helix, monkeypatch):
    monkeypatch.setattr(helix_plr, "FOLLOW_MIN_BYTES", 1000)
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(layered_gcode())
    size = path.stat().st_size
    plr._follow_executor = SyncExecutor()
    added(plr)
    plr.follower = None
    tick(plr, **printing_status(500))
    assert plr.follower is not None and plr.follower.offset == 0   # under FOLLOW_MIN_BYTES
    tick(plr, 0.1, **printing_status(size // 2, layer=2))
    assert 0 < plr.follower.offset <= size // 2
    tick(plr, 0.1, **printing_status(size, layer=3))
    follower = plr.follower
    finished(plr, "completed")
    index = helix_gcode_reader.layer_index_path(helix.gc_path, "sub/part.gcode")
    assert helix_gcode_reader.read_layer_index(index, ident(path)).complete is True
    assert plr.follower is None and follower.complete is True


def test_component_follows_on_the_interval_too(plr, helix, monkeypatch):
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(layered_gcode())
    plr._follow_executor = SyncExecutor()
    added(plr)
    tick(plr, **printing_status(600))
    assert plr.follower.offset == 0
    tick(plr, helix_plr.FOLLOW_INTERVAL_S, **printing_status(700))
    assert 0 < plr.follower.offset <= 700


def test_a_cancelled_print_leaves_the_index_open(plr, helix):
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(layered_gcode())
    plr._follow_executor = SyncExecutor()
    added(plr)
    tick(plr, helix_plr.FOLLOW_INTERVAL_S, **printing_status(path.stat().st_size))
    finished(plr, "cancelled")
    index = helix_gcode_reader.layer_index_path(helix.gc_path, "sub/part.gcode")
    summary = helix_gcode_reader.read_layer_index(index, ident(path))
    assert summary.last is not None and summary.complete is False


# --------------------------------------------------------------------------
# Offer rules
# --------------------------------------------------------------------------

from helix_plr import OfferInputs, offer_verdict  # noqa: E402

OK_INPUTS = OfferInputs("interrupted", "A1", "A1", True, 5, 2, True, 300.0)


@pytest.mark.parametrize(
    "change, blocked",
    [
        ({}, []),
        ({"newest_job_status": "klippy_shutdown"}, ["not_interrupted"]),
        ({"newest_job_status": "completed"}, ["not_interrupted"]),
        ({"snapshot_job_id": None}, ["no_snapshot"]),
        ({"snapshot_job_id": "A0"}, ["no_snapshot"]),
        ({"identity_ok": False}, ["file_changed"]),
        ({"layer": 1}, ["too_early"]),
        ({"layer": None, "print_duration": 119.0}, ["too_early"]),
        ({"layer": None, "print_duration": 120.0}, []),
        ({"tier": 0}, ["not_supported"]),
        ({"klippy_ready": False}, ["klippy_not_ready"]),
        ({"tier": 0, "klippy_ready": False}, ["not_supported", "klippy_not_ready"]),
    ],
)
def test_offer_verdict(change, blocked):
    verdict = offer_verdict(OK_INPUTS._replace(**change))
    assert verdict.blocked_by == blocked and verdict.available is (not blocked)


# --------------------------------------------------------------------------
# Endpoints: status, plan, prepare, commit, abort, discard
# --------------------------------------------------------------------------

from helix_plr import RESUME_LINK  # noqa: E402


class Req:
    def __init__(self, **params):
        self.params = params

    def get(self, key, default=None):
        return self.params.get(key, default)


def straight_layers(object_name=None):
    """Five layers of 100 moves; with object_name, each layer's moves are
    one EXCLUDE_OBJECT_START/END block, followed by a travel and a short
    second object, `peg`."""
    out = ["M83", "G1 F600"]
    for n in range(1, 6):
        out += [";LAYER_CHANGE", "SET_PRINT_STATS_INFO CURRENT_LAYER=%d" % n,
                "G1 Z%.1f" % (n * 0.2)]
        if object_name:
            out.append("EXCLUDE_OBJECT_START NAME=%s" % object_name)
        out += ["G1 X%d Y10 E.05" % i for i in range(1, 101)]
        if object_name:
            out += ["EXCLUDE_OBJECT_END NAME=%s" % object_name,
                    "G1 X120 Y30 F6000", "EXCLUDE_OBJECT_START NAME=peg",
                    "G1 X121 Y30 E.05", "G1 X122 Y30 E.05", "EXCLUDE_OBJECT_END NAME=peg",
                    "G1 F600"]
    return ("\n".join(out) + "\n").encode()


@pytest.fixture
def resume(plr, helix):
    """A tier-2 printer after a power cut during layer 4 of sub/part.gcode."""
    return power_cut_in_layer_4(plr, helix, straight_layers())


@pytest.fixture
def resume_in_object(plr, helix):
    """The same cut, in a file whose layers are each one object's block: the
    resume point falls inside the block."""
    return power_cut_in_layer_4(plr, helix, straight_layers("box.stl_id_0_copy_0"))


def power_cut_in_layer_4(plr, helix, data):
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(data)
    plr._follow_executor = SyncExecutor()
    plr._plan_executor = SyncExecutor()
    events = []
    helix.server.send_event = lambda name, payload: events.append((name, payload))
    plr.events = events
    added(plr, job_id="0000B2")
    helix.history.add_job("0000B2", filename="sub/part.gcode", status="interrupted")
    # The nozzle on X=50 of layer 4, the head 20 lines (1 mm of E) further on.
    layer4 = data.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    head = data.index(b"G1 X70 Y10", layer4)
    record = build_record(printing_status(head, layer=4), 1000.0)
    record.update({
        "toolhead_pos": [69.0, 10.0, 0.8, 1000.0],
        "live_pos": [49.5, 10.0, 0.8, 1000.0 - 20 * 0.05 - 0.025],
        "gcode_offset": [0.0, 0.0, 0.0, 0.0], "extrude_factor": 1.0, "speed_factor": 1.0,
        "active_extruder": "extruder", "retraction": None, "objects": [
            {"name": "A", "polygon": [[40, 0], [120, 0], [120, 40], [40, 40]]}],
        "excluded": [], "macro_vars": {}})
    record["heaters"]["heater_generic chamber"] = {"target": 0.0, "temp": 25.0}
    plr.store.write(fit_record(record)[0])
    plr.klippy_ready = True
    plr.policy.reset()

    printer = FakePrinter()
    helix.klippy_apis.query_objects.side_effect = printer.query
    helix.klippy_apis.run_gcode.side_effect = printer.run_gcode
    plr.printer = printer

    async def no_wait(seconds):
        plr.clock.t += seconds

    plr._sleep = no_wait
    return data


class FakePrinter:
    """Klipper as PLR sees it: heaters that reach a target once set (when
    `heats`), and homed axes that follow G28 X Y and SET_KINEMATIC_POSITION.
    With `nozzle_rate` (`bed_rate`) set, the nozzle (bed) climbs that many
    degrees per query toward its target instead. `log` holds each script with
    the nozzle and bed temperatures it was sent at."""

    def __init__(self):
        self.bed = 55.0
        self.chamber = 25.0
        self.nozzle = 25.0
        self.z = 0.0
        self.relative = False
        self.old_klipper = False          # ignores SET_HOMED, marks X, Y and Z homed
        self.heats = True
        self.homed = ""
        self.state = "standby"
        self.fail_on = None
        self.nozzle_rate = None
        self.nozzle_target = 0.0
        self.bed_rate = None
        self.bed_target = 0.0
        self.log = []

    async def run_gcode(self, script):
        self.log.append((script, self.nozzle, self.bed))
        if self.fail_on and script.startswith(self.fail_on):
            raise RuntimeError(self.fail_message)
        for line in script.splitlines():
            words = line.split()
            if not words:
                continue
            if words[0] == "M140" and self.bed_rate is not None:
                self.bed_target = float(words[1][1:])
            elif words[0] == "M140" and self.heats:
                self.bed = float(words[1][1:])
            elif words[0] == "M104" and self.nozzle_rate is not None:
                self.nozzle_target = float(words[1][1:])
            elif words[0] == "M104" and self.heats:
                self.nozzle = float(words[1][1:])
            elif words[0] == "SET_HEATER_TEMPERATURE" and self.heats:
                self.chamber = float(line.split("TARGET=")[1])
            elif words[0] == "G28":
                self.homed = "".join(sorted(set(self.homed) | {"x", "y"}))
            elif words[0] == "SET_KINEMATIC_POSITION":
                homed = line.split("SET_HOMED=")[1].split()[0].lower() \
                    if "SET_HOMED=" in line and not self.old_klipper else "xyz"
                self.homed = "".join(sorted(set(self.homed) | set(homed)))
                if "Z=" in line:
                    self.z = float(line.split("Z=")[1].split()[0])
            elif words[0] in ("G90", "G91"):
                self.relative = words[0] == "G91"
            elif words[0] in ("G0", "G1"):
                for w in words[1:]:
                    if w.startswith("Z"):
                        self.z = self.z + float(w[1:]) if self.relative else float(w[1:])
        return "ok"

    def fail(self, prefix, message):
        self.fail_on, self.fail_message = prefix, message

    async def query(self, objects):
        out = {}
        if "print_stats" in objects:
            out["print_stats"] = {"state": self.state}
        if "heater_bed" in objects:
            if self.bed_rate is not None and self.bed < self.bed_target:
                self.bed = min(self.bed_target, self.bed + self.bed_rate)
            out["heater_bed"] = {"temperature": self.bed, "target": 0.0}
        if "extruder" in objects:
            if self.nozzle_rate is not None and self.nozzle < self.nozzle_target:
                self.nozzle = min(self.nozzle_target, self.nozzle + self.nozzle_rate)
            out["extruder"] = {"temperature": self.nozzle}
        if "heater_generic chamber" in objects:
            out["heater_generic chamber"] = {"temperature": self.chamber}
        if "bed_mesh" in objects:
            out["bed_mesh"] = {"profiles": {"default": {}}}
        if "toolhead" in objects:
            out["toolhead"] = {"axis_minimum": [0, 0, 0, 0], "axis_maximum": [220, 220, 250, 0],
                               "homed_axes": self.homed, "position": [0.0, 0.0, self.z, 0.0]}
        if "configfile" in objects:
            out["configfile"] = {"settings": {
                "printer": {"kinematics": "corexy"},
                "bed_mesh": {"mesh_min": [10.0, 10.0], "mesh_max": [210.0, 210.0]},
                "stepper_x": {"position_endstop": 220.0},
                "stepper_y": {"position_endstop": 220.0}}}
        return out


HEATERS_OFF = "M104 S0\nM140 S0\nSET_HEATER_TEMPERATURE HEATER=chamber TARGET=0\nM107"


def sent(helix):
    return [c.args[0] for c in helix.klippy_apis.run_gcode.call_args_list]


def test_status_offers_the_interrupted_job(plr, resume):
    reply = run(plr.handle_status(Req()))
    assert reply["offer"] == {"available": True, "blocked_by": []}
    assert (reply["tier"], reply["backend"], reply["saving"]) == (2, "generic", True)
    assert reply["snapshot"]["job_id"] == "0000B2" and reply["snapshot"]["layer"] == 4
    assert reply["snapshot"]["z"] == 0.8 and reply["snapshot"]["age_s"] == 0.0
    assert reply["storage"]["storage_class"] in ("emmc", "sd", "ssd", "nand", "unknown")


def test_status_blocks_a_job_that_finished_normally(plr, helix, resume):
    helix.history.jobs.clear()
    helix.history.add_job("0000B2", status="completed")
    assert run(plr.handle_status(Req()))["offer"]["blocked_by"] == ["not_interrupted"]


def test_status_blocks_an_edited_file(plr, helix, resume):
    (helix.gc_path / "sub" / "part.gcode").write_bytes(resume + b"G1 X1\n")
    assert run(plr.handle_status(Req()))["offer"]["blocked_by"] == ["file_changed"]


def test_a_qidi_profile_without_qidis_firmware_is_not_resumed(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_profile",
                                   {"tier": 2, "backend": "qidi_print_state"}))
    reply = run(plr.handle_status(Req()))
    assert reply["backend"] == "none" and reply["tier"] == 0
    assert reply["offer"]["blocked_by"] == ["not_supported"]


def test_status_for_an_unlisted_printer(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_profile", {"tier": 0}))
    reply = run(plr.handle_status(Req()))
    assert reply["backend"] == "none" and reply["offer"]["blocked_by"] == ["not_supported"]


def test_plan_finds_the_measured_point(plr, resume):
    reply = run(plr.handle_plan(Req()))
    assert reply["method"] == "measured"
    layer4 = resume.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    # Re-trace by default: the executing line (E puts the nozzle on the one
    # ending at X=49), from its start.
    assert reply["resume_offset"] == resume.index(b"G1 X49 Y10", layer4)
    assert reply["filename"] == "sub/part.gcode" and reply["layer"] == 4
    assert reply["z_gcode"] == pytest.approx(0.8) and reply["z_physical"] == 0.8
    assert reply["requires"] == ["part_attached", "z_confirmed"]
    assert reply["warnings"] == [] and reply["bed"] == {"target": 60.0, "current": 55.0}
    assert "gcode_offset" in reply["restores"] and reply["plan_id"].startswith("0000B2-")
    plr.clock.t += 5.0
    assert run(plr.handle_plan(Req()))["plan_id"] == reply["plan_id"]   # same snapshot


def test_plan_refused_without_an_offer(plr, helix, resume):
    helix.history.jobs.clear()
    helix.history.add_job("0000B2", status="completed")
    with pytest.raises(Exception, match="plr_no_plan: not_interrupted"):
        run(plr.handle_plan(Req()))


def test_plan_refuses_multi_material(plr, resume):
    record = plr.store.load()
    record["active_extruder"] = "extruder1"
    plr.store.write(fit_record(record)[0])
    with pytest.raises(Exception, match="plr_scan_failed: multi_material"):
        run(plr.handle_plan(Req()))


def test_plan_warns_about_a_lost_mesh_and_a_cold_bed(plr, helix, resume):
    plr.job["mesh"] = {"profile": "adaptive", "saved": False, "min": [0, 0], "max": [1, 1],
                       "points": [[0.0]]}

    base = plr.printer.query

    async def query(objects):
        out = await base(objects)
        out.update({k: v for k, v in {"heater_bed": {"temperature": 30.0},
                                       "bed_mesh": {"profiles": {}}}.items() if k in objects})
        return out

    helix.klippy_apis.query_objects.side_effect = query
    reply = run(plr.handle_plan(Req()))
    assert reply["warnings"] == ["mesh_unavailable", "cold_bed"]
    assert reply["requires"] == ["part_attached", "z_confirmed", "accept_no_mesh",
                                 "accept_cold_bed"]


def test_a_kept_plan_reports_the_bed_as_it_is_now(plr, helix, resume):
    """The plan asked again 90 minutes on, same snapshot: the bed has cooled
    since the first answer."""
    first = run(plr.handle_plan(Req()))
    assert first["bed"]["current"] == 55.0 and "cold_bed" not in first["warnings"]
    plr.clock.t += 90 * 60
    plr.printer.bed = 27.0
    again = run(plr.handle_plan(Req()))
    assert again["plan_id"] == first["plan_id"]
    assert again["bed"] == {"target": 60.0, "current": 27.0}
    assert again["warnings"] == ["cold_bed"] and again["requires"][-1] == "accept_cold_bed"
    plr.printer.bed = 58.0
    warm = run(plr.handle_plan(Req()))
    assert warm["bed"]["current"] == 58.0 and "accept_cold_bed" not in warm["requires"]
    plr.printer.bed = 27.0
    run(plr.handle_plan(Req()))
    run(plr.handle_prepare(Req(plan_id=again["plan_id"])))
    with pytest.raises(Exception, match="plr_missing_confirmation: accept_cold_bed"):
        commit(plr, again["plan_id"])


def test_a_prepare_during_the_bed_read_keeps_the_plan_the_app_was_shown(plr, helix, resume):
    """plan re-reads the bed (an await) while a prepare takes the plan: the
    prepare's plan, and so commit's confirmations, must not change under it."""
    shown = run(plr.handle_plan(Req()))
    plr.printer.bed = 27.0
    query = plr.printer.query

    async def yielding(objects):
        await asyncio.sleep(0)
        return await query(objects)

    helix.klippy_apis.query_objects.side_effect = yielding

    async def both():
        return await asyncio.gather(plr.handle_plan(Req()),
                                    plr.handle_prepare(Req(plan_id=shown["plan_id"])))

    again, prepared = run(both())
    assert prepared == {"ok": True}
    assert again["requires"] == shown["requires"] == plr._plan.requires
    assert "accept_cold_bed" not in plr._plan.requires
    assert commit(plr, shown["plan_id"]) == {"ok": True, "job_started": True}


def test_a_failed_bed_read_answers_with_the_kept_plan(plr, helix, resume):
    shown = run(plr.handle_plan(Req()))
    query = plr.printer.query

    async def bed_read_fails(objects):
        if list(objects) == ["heater_bed"]:
            raise RuntimeError("Klippy Disconnected")
        return await query(objects)

    helix.klippy_apis.query_objects.side_effect = bed_read_fails
    assert run(plr.handle_plan(Req())) == shown


def test_include_file_mesh_needs_no_extra_confirmation(plr, helix, resume):
    plr.job["mesh"] = {"profile": "adaptive", "saved": False, "min": [0, 0], "max": [1, 1],
                       "points": [[0.0]]}
    original = helix.klippy_apis.query_objects.side_effect

    async def query(objects):
        out = await original(objects)
        if "bed_mesh" in out:
            out["bed_mesh"] = {"profiles": {"helix_plr": {}}}
        return out

    helix.klippy_apis.query_objects.side_effect = query
    reply = run(plr.handle_plan(Req()))
    assert "mesh_unavailable" not in reply["warnings"] and "bed_mesh" in reply["restores"]


def full_resume(plr, plan_id, **commit):
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    params = {"plan_id": plan_id, "confirmations": {"part_attached": True, "z_confirmed": True},
              "z_reference": "trusted"}
    params.update(commit)
    return run(plr.handle_commit(Req(**params)))


def test_prepare_then_commit_starts_the_job(plr, helix, resume, monkeypatch):
    marks = []
    monkeypatch.setattr(helix, "mark_helix_initiated", lambda *a: marks.append(a))
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    assert run(plr.handle_prepare(Req(plan_id=plan_id))) == {"ok": True}
    prepare = sent(helix)
    assert prepare[0] == "M140 S60"
    assert prepare[1] == "M104 S220"                        # in place, before anything moves
    assert prepare[2].startswith("SET_KINEMATIC_POSITION Z=0.8 SET_HOMED=Z\nG91\nG1 Z5 F300")
    assert prepare[3] == "M83\nG1 E-1 F2100\nM104 S150"       # lifted: standby
    assert prepare[4] == "G28 X Y"
    assert not any("M109" in s for s in prepare)
    reply = run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                      confirmations={"part_attached": True, "z_confirmed": True})))
    assert reply == {"ok": True, "job_started": True}
    start = sent(helix)[-1]
    assert start.startswith("M23 %s\nM26 S" % RESUME_LINK) and start.endswith("\nM24")
    assert marks == [(RESUME_LINK, "plr_resume", "sub/part.gcode")]
    link = helix.gc_path / RESUME_LINK
    assert link.is_symlink() and link.resolve() == (helix.gc_path / "sub" / "part.gcode").resolve()
    steps = [p["step"] for e, p in plr.events if e == "helix_print:plr_progress"
             and p["state"] == "done"]
    assert steps == ["bed_wait", "nozzle_in_place", "lift", "nozzle_standby", "home_xy",
                     "bed_wait", "nozzle_in_place", "restore", "heat_nozzle", "purge",
                     "move_to_resume", "start"]
    commit = sent(helix)[len(prepare):]
    assert commit[0] == "M140 S60"                          # the bed is re-asserted first


def test_a_resume_inside_an_object_enters_it_again(plr, helix, resume_in_object):
    reply = run(plr.handle_plan(Req()))
    layer4 = resume_in_object.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    assert reply["resume_offset"] == resume_in_object.index(b"G1 X49 Y10", layer4)
    full_resume(plr, reply["plan_id"])
    start = sent(helix)[-1].split("\n")
    assert start[0].startswith("M23 ") and start[1].startswith("M26 S")
    assert start[-2:] == ["EXCLUDE_OBJECT_START NAME=box.stl_id_0_copy_0", "M24"]


def exclude(plr, *names):
    record = plr.store.load()
    record["excluded"] = list(names)
    plr.store.write(fit_record(record)[0])


def peg_in_layer4(data):
    """The first move that prints after layer 4's box block: peg's first."""
    return data.index(b"G1 X121 Y30", data.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4"))


def test_an_excluded_object_open_at_the_resume_point_is_skipped(plr, helix, resume_in_object):
    """The resume starts past the excluded block, at the next object's first
    printed move, and lands where that move starts, never on the box."""
    exclude(plr, "BOX.STL_ID_0_COPY_0")
    reply = run(plr.handle_plan(Req()))
    assert reply["resume_offset"] == peg_in_layer4(resume_in_object)
    assert reply["retrace_estimate_mm"] == 0.0
    full_resume(plr, reply["plan_id"])
    start = sent(helix)[-1].split("\n")
    assert "EXCLUDE_OBJECT NAME=BOX.STL_ID_0_COPY_0" in start
    assert start[-2:] == ["EXCLUDE_OBJECT_START NAME=peg", "M24"]
    assert any("G1 X120 Y30 F6000" in c for c in sent(helix))
    assert not any("G1 X100 Y10 F6000" in c for c in sent(helix))


def test_an_excluded_block_with_no_end_refuses_the_plan(plr, helix):
    data = straight_layers("box.stl_id_0_copy_0")
    layer4 = data.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    power_cut_in_layer_4(plr, helix, data[:data.index(b"EXCLUDE_OBJECT_END", layer4)])
    exclude(plr, "BOX.STL_ID_0_COPY_0")
    with pytest.raises(Exception, match="plr_scan_failed: excluded_object_open"):
        run(plr.handle_plan(Req()))


def test_a_quoted_name_excluded_and_open_at_the_resume_point_is_skipped(plr, helix):
    data = power_cut_in_layer_4(plr, helix, straight_layers('"my part"'))
    exclude(plr, "MY PART")
    assert run(plr.handle_plan(Req()))["resume_offset"] == peg_in_layer4(data)


def test_a_quoted_name_open_at_the_resume_point_is_entered_again(plr, helix):
    power_cut_in_layer_4(plr, helix, straight_layers("'my part'"))
    full_resume(plr, run(plr.handle_plan(Req()))["plan_id"])
    assert sent(helix)[-1].split("\n")[-2:] == ['EXCLUDE_OBJECT_START NAME="my part"', "M24"]


def test_the_plan_layer_is_the_files_count_at_the_resume_line():
    state = helix_gcode_reader.ModalState()
    assert helix_plr._resume_layer(state, 4) == 4
    state.layer, state.layer_source = 5, "print_stats"
    assert helix_plr._resume_layer(state, 4) == 5
    state.layer_source = "count"              # not a number print_stats ever showed
    assert helix_plr._resume_layer(state, 4) == 4


def test_a_name_no_command_can_carry_refuses_the_plan(plr, helix, resume):
    exclude(plr, "evil\nG28")
    before = len(sent(helix))
    with pytest.raises(Exception, match="plr_scan_failed: unsafe_value"):
        run(plr.handle_plan(Req()))
    assert sent(helix)[before:] == [] and plr._plan is None


def test_commit_refuses_missing_confirmations_and_the_wrong_z(plr, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    with pytest.raises(Exception, match="plr_missing_confirmation: z_confirmed"):
        run(plr.handle_commit(Req(plan_id=plan_id, confirmations={"part_attached": True},
                                  z_reference="trusted")))
    with pytest.raises(Exception, match="plr_missing_confirmation"):
        run(plr.handle_commit(Req(plan_id=plan_id, z_reference="part_top",
                                  confirmations={"part_attached": True, "z_confirmed": True})))


def test_commit_before_prepare_is_refused(plr, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    with pytest.raises(Exception, match="plr_busy"):
        run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                  confirmations={"part_attached": True, "z_confirmed": True})))


def test_a_stale_plan_is_refused(plr, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    with pytest.raises(Exception, match="plr_stale_plan"):
        run(plr.handle_prepare(Req(plan_id="other")))
    plr.store.write(fit_record(plr.store.load())[0])          # a newer snapshot
    with pytest.raises(Exception, match="plr_stale_plan"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))


def test_a_printing_printer_is_busy(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]

    async def printing(objects):
        return {"print_stats": {"state": "printing"}}

    helix.klippy_apis.query_objects.side_effect = printing
    with pytest.raises(Exception, match="plr_busy"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))


def test_a_failed_start_unmarks_and_turns_heaters_off(plr, helix, resume, monkeypatch):
    unmarked = []
    monkeypatch.setattr(helix, "unmark_helix_initiated", lambda name: unmarked.append(name))
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))

    plr.printer.fail("M23", "SD busy")
    with pytest.raises(Exception, match="plr_klippy_error: SD busy"):
        run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                  confirmations={"part_attached": True, "z_confirmed": True})))
    assert unmarked == [RESUME_LINK]
    assert not (helix.gc_path / RESUME_LINK).exists()
    assert sent(helix)[-1] == HEATERS_OFF


def test_a_klipper_error_in_prepare_fails_safe(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]

    plr.printer.fail("G28", "Endstop x still triggered")
    with pytest.raises(Exception, match="plr_klippy_error: Endstop"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert sent(helix)[-1] == HEATERS_OFF
    assert plr.store.load() is not None


def test_abort_turns_heaters_off_and_keeps_the_snapshot(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    assert run(plr.handle_abort(Req(plan_id=plan_id))) == {"ok": True}
    assert sent(helix)[-1] == HEATERS_OFF
    assert plr.store.load() is not None
    with pytest.raises(Exception, match="plr_no_plan"):
        run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                  confirmations={"part_attached": True, "z_confirmed": True})))


def test_discard_deletes_the_snapshot(plr, helix, resume):
    run(plr.handle_plan(Req()))
    assert run(plr.handle_discard(Req())) == {"ok": True}
    assert plr.store.load() is None
    assert run(plr.handle_status(Req()))["snapshot"] is None
    with pytest.raises(Exception, match="plr_no_plan"):
        run(plr.handle_prepare(Req(plan_id="x")))


def test_a_long_scan_replies_pending_and_announces_the_plan(plr, resume, monkeypatch):
    import threading
    gate = threading.Event()

    class SlowExecutor:
        def __init__(self):
            from concurrent.futures import ThreadPoolExecutor
            self.pool = ThreadPoolExecutor(1)

        def submit(self, fn, *args):
            def slow():
                gate.wait(5)
                return fn(*args)
            return self.pool.submit(slow)

        def shutdown(self, wait=True):
            self.pool.shutdown(wait)

    plr._plan_executor = SlowExecutor()
    monkeypatch.setattr(helix_plr, "PLAN_SYNC_WAIT_S", 0.05)

    async def scenario():
        first = await plr.handle_plan(Req())
        plr.clock.t += 1.0
        again = await plr.handle_plan(Req())
        gate.set()
        import asyncio
        for _ in range(100):
            if any(e == "helix_print:plr_plan_ready" for e, _ in plr.events):
                break
            await asyncio.sleep(0.02)
        return first, again, await plr.handle_plan(Req())

    first, again, ready = run(scenario())
    assert first["pending"] is True and again == first
    assert ("helix_print:plr_plan_ready", {"plan_id": first["plan_id"]}) in plr.events
    assert ready["plan_id"] == first["plan_id"] and ready["method"] == "measured"


def test_a_root_level_file_starts_without_a_link(plr, helix, resume, monkeypatch):
    (helix.gc_path / "part.gcode").write_bytes(resume)
    added(plr, job_id="0000C3", filename="part.gcode")
    helix.history.add_job("0000C3", filename="part.gcode", status="interrupted")
    layer4 = resume.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    rec = build_record(printing_status(resume.index(b"G1 X70 Y10", layer4), layer=4), 1000.0)
    rec.update({"toolhead_pos": [69.0, 10.0, 0.8, 1000.0],
                "live_pos": [49.5, 10.0, 0.8, 1000.0 - 20 * 0.05 - 0.025],
                "gcode_offset": [0, 0, 0, 0], "extrude_factor": 1.0, "objects": [],
                "excluded": [], "macro_vars": {}})
    plr.store.write(fit_record(rec)[0])
    marks = []
    monkeypatch.setattr(helix, "mark_helix_initiated", lambda *a: marks.append(a))
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    full_resume(plr, plan_id)
    assert sent(helix)[-1].startswith("M23 part.gcode\n")
    assert marks == [("part.gcode", "plr_resume", None)]
    assert not (helix.gc_path / RESUME_LINK).exists()


def test_the_resumed_job_is_named_after_the_file_and_the_link_removed(plr, helix, resume):
    patched = []

    async def patch(info, state):
        patched.append((info.job_id, info.original_filename, state))

    helix._patch_history_entry = patch
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    full_resume(plr, plan_id)
    added(plr, job_id="0000D4", filename=RESUME_LINK)
    assert plr.job["history_name"] == "sub/part.gcode"
    finished(plr, "completed", job_id="0000D4")
    assert patched == [("0000D4", "sub/part.gcode", "completed")]
    assert not (helix.gc_path / RESUME_LINK).exists()


def test_the_plr_endpoints_are_registered(helix):
    paths = helix.server.endpoints
    for name in ("status", "plan", "prepare", "commit", "abort", "discard"):
        assert "/server/helix/plr/" + name in paths


# --------------------------------------------------------------------------
# QIDI print_state backend through the core
# --------------------------------------------------------------------------

from unittest.mock import AsyncMock  # noqa: E402

import helix_plr_qidi  # noqa: E402


class ConfigTable:
    """QIDI's config-table methods on its Moonraker database."""

    def __init__(self, db):
        self.rows = {}
        db.update_value_from_config = self.update
        db.delete_value_from_config = self.delete
        db.get_value_from_config = self.get

    async def update(self, section, key, value):
        assert section == "print_state"
        self.rows[key] = value

    async def delete(self, section, key):
        if key is None:
            self.rows = {}

    async def get(self, section, key):
        value = self.rows.get(key)

        class Cursor:
            async def fetchone(self_inner):
                return {"value": value} if value is not None else None

        return Cursor()


@pytest.fixture
def qidi_printer(plr, helix, tmp_path):
    run(helix.database.insert_item("helix-screen", "plr_profile",
                                   {"tier": 2, "backend": "qidi_print_state",
                                    "z_holds_unpowered": True}))
    table = ConfigTable(helix.database)
    apis = helix.klippy_apis
    apis.power_off_print = AsyncMock(return_value="ok")
    apis._send_klippy_request = AsyncMock(
        return_value={"SDCARD_POWER_OFF_CONTINUES_PINT_FILE": "resume"})
    apis.get_object_list.return_value = ["print_stats", "virtual_sdcard", "gcode_move",
                                         "toolhead", "extruder", "heater_bed"]
    apis.subscribe_objects.return_value = {
        "gcode_move": {"speed_factor_2": 1.5 / 60.0, "speed_2": 2.0, "base_position": [0, 0, 0, 0]},
        "virtual_sdcard": {"plate_index": 1}}
    run(plr.on_klippy_ready())
    cache = tmp_path / ".cache"
    cache.mkdir()
    plr.qidi._cache_dir = str(cache)
    plr.qidi._clock = plr.clock
    return table, cache


def test_qidi_is_selected_only_with_both_checks(plr, helix, qidi_printer):
    assert isinstance(plr.qidi, helix_plr_qidi.QidiPrintStateAdapter)
    del helix.klippy_apis.power_off_print
    run(plr._detect_qidi(helix.klippy_apis))
    assert plr.qidi is None


def test_qidi_on_mainline_klipper_is_not_selected(plr, helix):
    helix.klippy_apis.power_off_print = AsyncMock()
    ConfigTable(helix.database)
    helix.klippy_apis._send_klippy_request = AsyncMock(return_value={"G28": "home"})
    run(plr._detect_qidi(helix.klippy_apis))
    assert plr.qidi is None


def test_qidi_loop_writes_print_state_rows(plr, qidi_printer):
    table, cache = qidi_printer
    added(plr, job_id="0000E5")
    tick(plr, 0.5, **printing_status(100))
    assert table.rows == {}                      # QIDI deletes the section at start; wait
    tick(plr, 3.0, **printing_status(200))
    assert table.rows["file_position"] == "200"
    assert table.rows["speed_factor"].startswith("0.025")       # QIDI's own speed_factor_2


@pytest.fixture
def qidi_resume(plr, helix, resume, qidi_printer):
    table, cache = qidi_printer
    (cache / "part.gcode").write_bytes(resume)
    table.rows["file_position"] = "1"
    return table, cache


@pytest.fixture
def qidi_resume_in_object(plr, helix, resume_in_object, qidi_printer):
    table, cache = qidi_printer
    (cache / "part.gcode").write_bytes(resume_in_object)
    table.rows["file_position"] = "1"
    return table, cache


def test_qidi_resumes_inside_an_object_it_cannot_enter(plr, helix, qidi_resume_in_object):
    """Known gap: QIDI starts the file inside the block with no object
    entered; Klipper prints it and reports the END as not matching."""
    _, cache = qidi_resume_in_object
    data = (cache / "part.gcode").read_bytes()
    reply = run(plr.handle_plan(Req()))
    layer4 = data.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    assert data.index(b"EXCLUDE_OBJECT_START", layer4) < reply["resume_offset"] \
        < data.index(b"EXCLUDE_OBJECT_END", layer4)


def test_qidi_resumes_after_an_excluded_object_open_at_the_cut(plr, helix, qidi_resume_in_object):
    _, cache = qidi_resume_in_object
    exclude(plr, "BOX.STL_ID_0_COPY_0")
    reply = run(plr.handle_plan(Req()))
    assert reply["resume_offset"] == peg_in_layer4((cache / "part.gcode").read_bytes())


def test_qidi_status_and_offer(plr, helix, qidi_resume):
    table, _ = qidi_resume
    reply = run(plr.handle_status(Req()))
    assert (reply["backend"], reply["tier"]) == ("qidi_print_state", 2)
    assert reply["offer"] == {"available": True, "blocked_by": []}
    table.rows.clear()
    assert run(plr.handle_status(Req()))["offer"]["blocked_by"] == ["no_snapshot"]


class ReadyKlippy:
    """Moonraker's klippy_connection once Klippy reports ready."""

    def is_ready(self):
        return True


def reconnect_with_a_slow_qidi_check(plr, helix):
    """Klippy restarts; on the new connection gcode/help takes a while."""
    plr.on_klippy_gone()
    helix.server.components["klippy_connection"] = ReadyKlippy()

    async def slow_help(*args):
        for _ in range(20):
            await asyncio.sleep(0)
        return {"SDCARD_POWER_OFF_CONTINUES_PINT_FILE": "resume"}

    helix.klippy_apis._send_klippy_request = slow_help

    async def yielding_sleep(seconds):
        plr.clock.t += seconds
        await asyncio.sleep(0)

    plr._sleep = yielding_sleep


def test_status_asked_at_klippy_ready_waits_for_the_qidi_check(plr, helix, qidi_resume):
    reconnect_with_a_slow_qidi_check(plr, helix)

    async def scenario():
        status = asyncio.ensure_future(plr.handle_status(Req()))
        await asyncio.sleep(0)
        await plr.on_klippy_ready()
        return await status

    reply = run(scenario())
    assert (reply["backend"], reply["tier"]) == ("qidi_print_state", 2)
    assert reply["offer"] == {"available": True, "blocked_by": []}


def test_the_wait_for_klippy_ready_is_bounded(plr, helix, qidi_resume):
    reconnect_with_a_slow_qidi_check(plr, helix)
    started = plr.clock.t
    reply = run(plr.handle_status(Req()))           # on_klippy_ready never runs
    assert "klippy_not_ready" in reply["offer"]["blocked_by"]
    assert plr.clock.t - started == pytest.approx(helix_plr.READY_WAIT_S, abs=1.0)


class StartingKlippy:
    """Moonraker's klippy_connection while Klippy is still starting up."""

    class state:
        name = "STARTUP"

    def is_ready(self):
        return False


def restart_moonraker(plr, helix):
    """A fresh Moonraker: no backend detected yet for any connection."""
    reconnect_with_a_slow_qidi_check(plr, helix)
    plr.qidi = None


def test_a_qidi_check_slower_than_15s_still_answers_its_backend(plr, helix, qidi_resume):
    restart_moonraker(plr, helix)
    started = plr.clock.t

    async def slower_help(*args):
        for _ in range(20):
            plr.clock.t += 1.0
            await asyncio.sleep(0)
        return {"SDCARD_POWER_OFF_CONTINUES_PINT_FILE": "resume"}

    helix.klippy_apis._send_klippy_request = slower_help

    async def scenario():
        status = asyncio.ensure_future(plr.handle_status(Req()))
        await asyncio.sleep(0)
        await plr.on_klippy_ready()
        return await status

    reply = run(scenario())
    assert (reply["backend"], reply["tier"]) == ("qidi_print_state", 2)
    assert plr.clock.t - started >= 20.0


def test_status_asked_while_klippy_starts_waits_for_its_ready(plr, helix, qidi_resume):
    restart_moonraker(plr, helix)
    starting = StartingKlippy()
    helix.server.components["klippy_connection"] = starting

    async def scenario():
        status = asyncio.ensure_future(plr.handle_status(Req()))
        for _ in range(40):                 # 10 s of Klippy startup
            await asyncio.sleep(0)
        helix.server.components["klippy_connection"] = ReadyKlippy()
        await plr.on_klippy_ready()
        return await status

    reply = run(scenario())
    assert (reply["backend"], reply["tier"]) == ("qidi_print_state", 2)


def test_a_failed_backend_detection_answers_at_once(plr, helix, qidi_resume, monkeypatch):
    restart_moonraker(plr, helix)

    def broken(*args):
        raise RuntimeError("no gcode_move")

    monkeypatch.setattr(helix_plr_qidi, "qidi_print_state_present", broken)
    helix.klippy_apis._send_klippy_request = AsyncMock(return_value={})
    plr.events.clear()
    started = plr.clock.t

    async def scenario():
        status = asyncio.ensure_future(plr.handle_status(Req()))
        await asyncio.sleep(0)
        await plr.on_klippy_ready()
        return await status

    reply = run(scenario())
    assert reply["backend"] == "none"
    assert plr.clock.t - started < 1.0
    announced = [p for e, p in plr.events if e == "helix_print:plr_status"]
    assert len(announced) == 1 and announced[0]["backend"] == "none"


def test_klippy_ready_announces_the_status_it_decided(plr, helix, qidi_resume):
    reconnect_with_a_slow_qidi_check(plr, helix)
    plr.events.clear()
    run(plr.on_klippy_ready())
    announced = [p for e, p in plr.events if e == "helix_print:plr_status"]
    assert len(announced) == 1
    assert announced[0]["backend"] == "qidi_print_state"
    assert announced[0]["offer"] == {"available": True, "blocked_by": []}
    assert announced[0] == run(plr.handle_status(Req()))


def test_qidi_unlisted_printer_stays_off(plr, helix, qidi_resume):
    run(helix.database.insert_item("helix-screen", "plr_profile", {"tier": 0}))
    reply = run(plr.handle_status(Req()))
    assert (reply["backend"], reply["tier"]) == ("none", 0)


def test_qidi_plan_prepare_commit(plr, helix, qidi_resume, monkeypatch):
    table, cache = qidi_resume
    marks = []
    plr.qidi._mark = lambda *a: marks.append(a)
    reply = run(plr.handle_plan(Req()))
    assert reply["tier"] == 2 and reply["method"] == "measured"
    assert "mesh_unavailable" not in reply["warnings"] and "no_purge_location" not in reply["warnings"]
    assert plr._plan.purge_xy is None and plr._plan.mesh_loadable is None   # QIDI's sequence purges
    run(plr.handle_prepare(Req(plan_id=reply["plan_id"])))
    # The heat wait, then standby in place: no lift, QIDI lands on its own Z.
    assert sent(helix) == ["M140 S60", "M104 S220", "M83\nG1 E-1 F2100\nM104 S150"]
    done = run(plr.handle_commit(Req(plan_id=reply["plan_id"], z_reference="trusted",
                                     confirmations={"part_attached": True, "z_confirmed": True})))
    assert done == {"ok": True, "job_started": True}
    assert helix.klippy_apis.power_off_print.await_count == 1
    assert table.rows["file_position"] == str(reply["resume_offset"] + 4096)
    assert marks == [(str(cache / "part.gcode"), "plr_resume")]
    assert sent(helix)[-2:] == ["M140 S60", "M104 S220"]  # waited again inside commit
    assert not any(s.startswith("M23") for s in sent(helix))


def test_qidi_commit_failure_is_a_klippy_error_with_heaters_off(plr, helix, qidi_resume):
    helix.klippy_apis.power_off_print.side_effect = RuntimeError("no file")
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    with pytest.raises(Exception, match="plr_klippy_error: no file"):
        run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                  confirmations={"part_attached": True, "z_confirmed": True})))
    assert sent(helix)[-1] == HEATERS_OFF


def test_qidi_abort_keeps_the_rows_and_turns_heaters_off(plr, helix, qidi_resume):
    table, _ = qidi_resume
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_abort(Req(plan_id=plan_id)))
    assert table.rows == {"file_position": "1"}
    assert sent(helix)[-1] == HEATERS_OFF


def test_qidi_discard_deletes_rows_and_snapshot(plr, helix, qidi_resume):
    table, _ = qidi_resume
    run(plr.handle_discard(Req()))
    assert table.rows == {} and plr.store.load() is None


def test_scan_follows_the_file_klipper_reads(plr, helix, resume):
    """A firmware that prints from a copy (QIDI's .cache, a .3mf's plate file)
    counts file_position in that copy; the scan and index key on it."""
    copy = helix.gc_path.parent / "copy.gcode"
    copy.write_bytes(b";plate header\n" * 10 + resume)
    record = plr.store.load()
    s = printing_status(100, layer=4)
    s["virtual_sdcard"]["file_path"] = str(copy)
    tick(plr, 0.1, **s)
    assert plr.job["print_path"] == str(copy)
    assert plr.job["print_identity"]["file_size"] == copy.stat().st_size
    path, index, ident = plr.print_target()
    assert path == copy.resolve() and ".helix_index/.external/copy.gcode" in index
    shift = len(b";plate header\n" * 10)
    record["file_position"] += shift
    plr.store.write(fit_record(record)[0])
    # The generic start would select the job's file, not the copy the offset is in.
    with pytest.raises(Exception, match="plr_scan_failed: printed_copy"):
        run(plr.handle_plan(Req()))
    scan, point = plr._plan_submit(
        helix_plr._scan_and_locate, path, record["file_position"], index, ident, record,
        0.5).result()
    layer4 = resume.index(b"SET_PRINT_STATS_INFO CURRENT_LAYER=4")
    assert point.offset == resume.index(b"G1 X55 Y10", layer4) + shift


# --------------------------------------------------------------------------
# Sequence safety: single flight, abort, a machine that changed since prepare
# --------------------------------------------------------------------------

COMMIT_OK = {"part_attached": True, "z_confirmed": True}


def commit(plr, plan_id):
    return run(plr.handle_commit(Req(plan_id=plan_id, z_reference="trusted",
                                     confirmations=COMMIT_OK)))


def test_two_prepares_at_once_run_one(plr, helix, resume):
    """Two screens, or a retry, tapping Prepare together: the second is
    refused before either sends anything."""
    import asyncio
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    query = plr.printer.query

    async def yielding(objects):
        await asyncio.sleep(0)
        return await query(objects)

    helix.klippy_apis.query_objects.side_effect = yielding

    async def both():
        return await asyncio.gather(plr.handle_prepare(Req(plan_id=plan_id)),
                                    plr.handle_prepare(Req(plan_id=plan_id)),
                                    return_exceptions=True)

    results = run(both())
    assert sum(1 for r in results if r == {"ok": True}) == 1
    assert sum(1 for r in results if "plr_busy" in str(r)) == 1
    assert sum(1 for s in sent(helix) if s.startswith("SET_KINEMATIC_POSITION")) == 1


def test_abort_between_commit_steps_sends_nothing_more(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    real = plr.printer.run_gcode

    async def gcode(script):
        if script.startswith("SET_GCODE_OFFSET"):          # the restore step
            await plr.handle_abort(Req(plan_id=plan_id))
        return await real(script)

    helix.klippy_apis.run_gcode.side_effect = gcode
    with pytest.raises(Exception, match="aborted"):
        commit(plr, plan_id)
    assert not any("M109" in s for s in sent(helix))


def test_abort_during_the_last_prepare_step_stays_aborted(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    real = plr.printer.run_gcode

    async def gcode(script):
        if script.startswith("G28"):
            await plr.handle_abort(Req(plan_id=plan_id))
        return await real(script)

    helix.klippy_apis.run_gcode.side_effect = gcode
    with pytest.raises(Exception, match="aborted"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert plr._sequence == "aborted"
    with pytest.raises(Exception, match="plr_no_plan"):
        commit(plr, plan_id)


def test_abort_during_the_last_commit_step_never_starts_the_print(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    real = plr.printer.run_gcode

    async def gcode(script):
        if "G92 E" in script:                       # move_to_resume, the last step
            await plr.handle_abort(Req(plan_id=plan_id))
        return await real(script)

    helix.klippy_apis.run_gcode.side_effect = gcode
    with pytest.raises(Exception, match="aborted"):
        commit(plr, plan_id)
    assert not any(s.startswith("M23") for s in sent(helix))
    assert not (helix.gc_path / RESUME_LINK).exists()


def test_abort_during_a_heat_wait_stops_before_any_motion(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.heats = False

    async def abort_while_waiting(seconds):
        await plr.handle_abort(Req(plan_id=plan_id))

    plr._sleep = abort_while_waiting
    with pytest.raises(Exception, match="aborted"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert not any(s.startswith(("SET_KINEMATIC", "G28")) for s in sent(helix))


MOTION = ("SET_KINEMATIC_POSITION", "G91", "G1 ", "G0 ", "G28")


def moved_before_hot(printer, target=220.0, bed_target=60.0, since=0):
    """Scripts that moved the toolhead while the nozzle or the bed was under
    its target."""
    return [script for script, nozzle, bed in printer.log[since:]
            if any(line.startswith(MOTION) for line in script.splitlines())
            and (nozzle < target - 1.0 or bed < bed_target - 1.0)]


def test_prepare_heats_the_nozzle_in_place_before_anything_moves(plr, helix, resume):
    """After a cut the nozzle is frozen into the part: a cold lift or XY
    homing tears it off the bed."""
    plr.printer.nozzle_rate = 40.0
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert any(s.startswith("G28") for s in sent(helix))
    assert moved_before_hot(plr.printer) == []
    steps = [p["step"] for e, p in plr.events if e == "helix_print:plr_progress"
             and p["state"] == "done"]
    assert steps[:2] == ["bed_wait", "nozzle_in_place"]


def test_prepare_starts_bed_and_nozzle_together_and_moves_once_both_are_hot(
        plr, helix, resume):
    """The nozzle frees itself while the bed reheats and the part grips
    again; nothing lifts before both are at temperature."""
    plr.printer.bed = 25.0
    plr.printer.bed_rate = 5.0
    plr.printer.nozzle_rate = 40.0
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    nozzle_set = [bed for script, _, bed in plr.printer.log if script == "M104 S220"]
    assert nozzle_set == [25.0]                     # sent before the bed had warmed at all
    assert any(s.startswith("G28") for s in sent(helix))
    assert moved_before_hot(plr.printer) == []


def test_the_nozzle_drops_to_standby_once_lifted_and_retracts_while_hot(plr, helix, resume):
    """R30: lift, retract at print temperature, standby, then home XY."""
    plr.printer.nozzle_rate = 40.0
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    scripts = [script for script, _, _ in plr.printer.log]
    standby = scripts.index("M83\nG1 E-1 F2100\nM104 S150")
    assert scripts[standby - 1].startswith("SET_KINEMATIC_POSITION")     # the lift
    assert scripts[standby + 1] == "G28 X Y"
    assert plr.printer.log[standby][1] >= 219.0     # the retract runs hot
    assert plr.printer.nozzle_target == 150.0


def test_a_tier1_commit_reheats_the_nozzle_before_its_z_reference(plr, helix, resume):
    """The jog left a standby nozzle on the part; commit's Z reference lifts
    it, so it must be back at print temperature first."""
    tier1(plr, helix)
    plr.printer.nozzle_rate = 40.0
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.nozzle = 150.0                      # standby, as prepare left it
    before = len(plr.printer.log)
    run(plr.handle_commit(Req(plan_id=plan_id, z_reference="part_top", confirmations={
        "part_attached": True, "nozzle_clear": True, "z_jogged": True})))
    assert any(s.startswith("SET_KINEMATIC_POSITION")
               for s, _, _ in plr.printer.log[before:])
    assert moved_before_hot(plr.printer, since=before) == []


def test_qidi_heats_the_nozzle_in_place_in_prepare_and_again_before_qidis_resume(
        plr, helix, qidi_resume):
    """QIDI's sequence lifts and homes XY before its M109."""
    plr.printer.nozzle_rate = 40.0
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert plr.printer.nozzle >= 219.0
    plr.printer.nozzle = 25.0                       # cooled while the screen asked
    at_resume = []

    async def resume_call(*a):
        at_resume.append(plr.printer.nozzle)
        return "ok"

    helix.klippy_apis.power_off_print.side_effect = resume_call
    commit(plr, plan_id)
    assert at_resume and at_resume[0] >= 219.0


def test_abort_during_the_in_place_nozzle_wait_stops_before_any_motion(plr, helix, resume):
    plr.printer.nozzle_rate = 0.0                   # the bed heats, the nozzle never does
    plan_id = run(plr.handle_plan(Req()))["plan_id"]

    async def abort_while_waiting(seconds):
        await plr.handle_abort(Req(plan_id=plan_id))

    plr._sleep = abort_while_waiting
    with pytest.raises(Exception, match="aborted"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert "M104 S220" in sent(helix)
    assert not any(line.startswith(MOTION) for s in sent(helix) for line in s.splitlines())
    assert sent(helix)[-1] == HEATERS_OFF
    assert plr._sequence == "aborted"


def test_no_nozzle_temperature_is_refused_rather_than_moved_cold(plr, helix, resume):
    record = plr.store.load()
    record["heaters"]["extruder"]["target"] = 0.0   # and the file sets none
    plr.store.write(fit_record(record)[0])
    with pytest.raises(Exception, match="plr_scan_failed: no_nozzle_target"):
        run(plr.handle_plan(Req()))
    record["heaters"]["extruder"]["target"] = 220.0
    plr.store.write(fit_record(record)[0])
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr._plan = plr._plan._replace(nozzle_target=0.0)
    with pytest.raises(Exception, match="plr_scan_failed: no_nozzle_target"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert sent(helix) == []


def test_a_heat_wait_times_out(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.heats = False
    with pytest.raises(Exception, match="plr_klippy_error: bed_wait timed out"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert sent(helix)[-1] == HEATERS_OFF


def test_the_chamber_wait_is_polled_and_skippable(plr, helix, resume):
    record = plr.store.load()
    record["heaters"]["heater_generic chamber"] = {"target": 45.0, "temp": 25.0}
    plr.store.write(fit_record(record)[0])
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.heats = False
    plr.printer.bed = 60.0
    with pytest.raises(Exception, match="chamber_wait timed out"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.nozzle = 220.0
    run(plr.handle_prepare(Req(plan_id=plan_id, skip_chamber_wait=True)))
    assert "SET_HEATER_TEMPERATURE HEATER=chamber TARGET=45" in sent(helix)


def test_commit_after_a_klipper_restart_needs_a_new_prepare(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.on_klippy_gone()
    plr.klippy_ready = True
    with pytest.raises(Exception, match="plr_stale_plan"):
        commit(plr, plan_id)
    assert plr._sequence is None
    assert not any(s.startswith("M23") for s in sent(helix))


def test_commit_after_motors_off_needs_a_new_prepare(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.homed = ""                          # idle_timeout's M84
    with pytest.raises(Exception, match="plr_stale_plan"):
        commit(plr, plan_id)


def test_a_retried_prepare_never_redeclares_the_snapshot_z(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.fail("G28", "Endstop x still triggered")
    with pytest.raises(Exception):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.fail_on = None
    before = len(sent(helix))
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    retry = sent(helix)[before:]
    assert not any(s.startswith("SET_KINEMATIC_POSITION") for s in retry)   # Z still homed
    assert retry.index("G91\nG1 Z5 F300\nG90") < retry.index("G28 X Y")   # lifts again first
    # Klipper restarted after that lift: the nozzle is where it was lifted to, 10.8.
    plr.printer.homed = ""
    before = len(sent(helix))
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    retry = sent(helix)[before:]
    assert "SET_KINEMATIC_POSITION Z=10.8 SET_HOMED=Z" in retry
    assert not any("G1 Z5 F300" in s for s in retry)


def test_no_room_to_lift_means_no_offer(plr, helix, resume):
    plr.axis_max = [220.0, 220.0, 2.5]
    assert run(plr.handle_status(Req()))["offer"]["blocked_by"] == ["no_lift_room"]


@pytest.mark.parametrize("lift, expected", [(0, 5.0), (-3, 5.0), (0.5, 2.0), (8, 8.0)])
def test_profile_lift_is_never_below_the_minimum(lift, expected):
    assert parse_profile({"z_lift_mm": lift}).z_lift_mm == expected


def test_a_snapshot_without_a_live_position_is_not_planned(plr, resume):
    record = plr.store.load()
    record["live_pos"] = None
    plr.store.write(fit_record(record)[0])
    with pytest.raises(Exception, match="plr_scan_failed: no_live_position"):
        run(plr.handle_plan(Req()))


def test_a_tier_three_profile_acts_at_tier_two(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_profile",
                                   {"tier": 3, "z_holds_unpowered": True}))
    assert run(plr.handle_status(Req()))["tier"] == 2


def test_a_snapshot_of_another_job_is_not_offered(plr, resume):
    record = plr.store.load()
    record["job_id"] = "0000ZZ"
    plr.store.write(fit_record(record)[0])
    assert run(plr.handle_status(Req()))["offer"]["blocked_by"] == ["no_snapshot"]


def test_a_failed_background_plan_reports_its_error_once(plr, resume, monkeypatch):
    import threading
    gate = threading.Event()
    record = plr.store.load()
    record["active_extruder"] = "extruder1"
    plr.store.write(fit_record(record)[0])
    submits = []

    class SlowExecutor:
        def __init__(self):
            from concurrent.futures import ThreadPoolExecutor
            self.pool = ThreadPoolExecutor(1)

        def submit(self, fn, *args):
            submits.append(fn)

            def slow():
                gate.wait(5)
                return fn(*args)
            return self.pool.submit(slow)

        def shutdown(self, wait=True):
            self.pool.shutdown(wait)

    plr._plan_executor = SlowExecutor()
    monkeypatch.setattr(helix_plr, "PLAN_SYNC_WAIT_S", 0.05)

    async def scenario():
        import asyncio
        first = await plr.handle_plan(Req())
        gate.set()
        for _ in range(100):
            if any(e == "helix_print:plr_plan_ready" for e, _ in plr.events):
                break
            await asyncio.sleep(0.02)
        errors = []
        for _ in range(2):
            try:
                await plr.handle_plan(Req())
            except Exception as e:
                errors.append(str(e))
        return first, errors

    first, errors = run(scenario())
    ready = [p for e, p in plr.events if e == "helix_print:plr_plan_ready"]
    assert first["pending"] is True and "multi_material" in ready[0]["error"]
    assert len(errors) == 2 and all("multi_material" in e for e in errors)
    assert len(submits) == 1


def test_qidi_abort_before_quidis_resume_call_stops_it(plr, helix, qidi_resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    write = plr.qidi._write

    async def write_then_abort(rows):
        ok = await write(rows)
        await plr.handle_abort(Req(plan_id=plan_id))
        return ok

    plr.qidi._write = write_then_abort
    with pytest.raises(Exception, match="aborted"):
        commit(plr, plan_id)
    assert helix.klippy_apis.power_off_print.await_count == 0


def test_qidi_refuses_a_cache_file_that_is_not_the_snapshots(plr, helix, qidi_resume):
    _, cache = qidi_resume
    (cache / "part.gcode").write_bytes(b"G1 X1\n" * 10)
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    with pytest.raises(Exception, match="plr_scan_failed: qidi_cache_mismatch"):
        commit(plr, plan_id)
    assert helix.klippy_apis.power_off_print.await_count == 0


# --------------------------------------------------------------------------
# Part top, bed limits, homing overrides
# --------------------------------------------------------------------------

from helix_plr_scan import ResumePoint, state_before  # noqa: E402


def point_at(data, needle):
    scan = helix_gcode_reader.scan_to_offset(io.BytesIO(data), len(data))
    offset = data.index(needle)
    return scan, ResumePoint(offset, state_before(io.BytesIO(data), scan, offset),
                             "measured", 0.0, 0.0, [])


def test_part_top_skips_a_z_hop_at_the_cut():
    data = b"M83\nG1 Z.4\nG1 X10 Y10 E1\nG1 X20 Y10 E1\nG1 Z.8\nG1 X50 Y50\nG1 Z.4\n"
    scan, point = point_at(data, b"G1 X50 Y50")
    assert point.state.gcode_position()[2] == pytest.approx(0.8)
    assert helix_plr._part_top_z(scan, point) == pytest.approx(0.4)


def test_part_top_skips_the_next_layers_z():
    data = b"M83\nG1 Z.4\nG1 X10 Y10 E1\n;LAYER_CHANGE\nG1 Z.6\nG1 X20 Y20 E1\n"
    scan, point = point_at(data, b"G1 X20 Y20 E1")
    assert helix_plr._part_top_z(scan, point) == pytest.approx(0.4)


def test_tier1_without_any_extrusion_before_the_point_is_refused(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_profile", {"tier": 1}))
    import helix_plr as module
    real = module._part_top_z
    module._part_top_z = lambda scan, point: None
    try:
        with pytest.raises(Exception, match="plr_scan_failed: no_part_top"):
            run(plr.handle_plan(Req()))
    finally:
        module._part_top_z = real


def test_the_purge_line_runs_inside_the_bed_not_the_travel(plr, resume):
    run(plr.handle_plan(Req()))
    x0, y0, x1, y1 = plr._plan.purge_xy
    assert (x0, y0) == (15.0, 205.0)     # back edge: mesh_max 210 - margin, not the axis 220
    assert plr._plan.park_xy == (220.0, 220.0)


@pytest.mark.parametrize(
    "override, reason",
    [
        ({"axes": "xyz", "gcode": "G28 X\nG28 Y", "set_position_z": 0.0}, "override_sets_z"),
        ({"axes": "xyz", "gcode": "_HOME_X\n_HOME_Y\n_HOME_Z"}, "override_unknown"),
        ({"axes": "xyz", "gcode": "{% if 'x' not in printer.toolhead.homed_axes %}\n"
                                  "G28 X\n{% endif %}\nG90\nG1 X100 F6000\nG28 Y"}, None),
        ({"axes": "xy", "gcode": "SET_KINEMATIC_POSITION Z=0\nG28 X Y"}, "override_unknown"),
    ],
)
def test_homing_overrides_that_may_move_z(override, reason):
    check = check_xy_homing({"homing_override": override}, "corexy")
    assert check == HomingCheck(reason is None, reason, True)


def test_qidi_refuses_a_landing_below_the_plastic(plr, helix, qidi_resume):
    record = plr.store.load()
    record["gcode_offset"] = [0.0, 0.0, -0.1, 0.0]
    plr.store.write(fit_record(record)[0])
    with pytest.raises(Exception, match="plr_scan_failed: qidi_landing_below_part"):
        run(plr.handle_plan(Req()))
    record["gcode_offset"] = [0.0, 0.0, -0.04, 0.0]
    plr.store.write(fit_record(record)[0])
    assert run(plr.handle_plan(Req()))["method"] == "measured"


def test_qidi_landing_counts_the_mesh(plr, helix, qidi_resume):
    plr.job["mesh"] = {"points": [[-0.2, -0.2], [-0.2, -0.2]], "min": [0, 0], "max": [220, 220]}
    with pytest.raises(Exception, match="qidi_landing_below_part"):
        run(plr.handle_plan(Req()))


# --------------------------------------------------------------------------
# Resumed job's mesh, index pruning, one frame at a time
# --------------------------------------------------------------------------


def test_a_resumed_job_keeps_the_mesh_it_resumed_with(plr, helix, resume, tmp_path):
    cfg = tmp_path / "printer_data" / "config" / "helix-plr" / "mesh.cfg"
    cfg.parent.mkdir(parents=True, exist_ok=True)
    cfg.write_text("[bed_mesh helix_plr]\n")
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    full_resume(plr, plan_id)
    added(plr, job_id="0000F6", filename=RESUME_LINK)
    assert cfg.exists()
    assert plr.job["history_name"] == "sub/part.gcode"
    added(plr, job_id="0000F7", filename=RESUME_LINK)        # a later print, not a resume
    assert not cfg.exists()


def test_a_complete_follower_is_not_stepped_again(plr, helix, monkeypatch):
    monkeypatch.setattr(helix_plr, "FOLLOW_MIN_BYTES", 1000)
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(layered_gcode())
    steps = []
    executor = SyncExecutor()
    submit = executor.submit
    executor.submit = lambda fn, *a: (steps.append(fn) if getattr(fn, "__name__", "") == "step"
                                      else None, submit(fn, *a))[1]
    plr._follow_executor = executor
    added(plr)
    tick(plr, **printing_status(2000))
    assert len(steps) == 1
    plr.follower.complete = True
    tick(plr, 30.0, **printing_status(8000))
    assert len(steps) == 1


def test_the_prune_spares_the_indexes_recovery_is_using(plr, helix, monkeypatch):
    monkeypatch.setattr(helix_plr, "FOLLOW_MIN_BYTES", 1000)
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(layered_gcode())
    plr._follow_executor = SyncExecutor()
    added(plr)
    tick(plr, **printing_status(2000))
    index = helix_gcode_reader.layer_index_path(helix.gc_path, "sub/part.gcode")
    assert plr.protected_index_paths() == [index, index]
    # The file changed under the print: stale by the rule, kept while in use.
    path.write_bytes(path.read_bytes() + b"; edited\n")
    plr._prune_indexes()
    assert os.path.exists(index)
    plr.follower = None
    plr.job = None
    assert plr.protected_index_paths() == []
    plr._prune_indexes()
    assert not os.path.exists(index)


def test_the_job_start_prune_spares_a_temp_index_analysis_is_writing(plr, helix):
    """The one prune rule: a whole-file write's temp index stays while its
    worker could still be writing it, whoever runs the prune."""
    path = helix.gc_path / "sub" / "other.gcode"
    path.write_bytes(layered_gcode())
    tmp = helix_gcode_reader.layer_index_path(helix.gc_path, "sub/other.gcode") + ".tmp"
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    open(tmp, "w").write("{}\n")
    plr._follow_executor = SyncExecutor()
    plr._prune_indexes()
    assert os.path.exists(tmp)
    old = time.time() - helix_gcode_reader.worker_timeout(path.stat().st_size) - 60
    os.utime(tmp, (old, old))
    plr._prune_indexes()
    assert not os.path.exists(tmp)


def test_a_waiting_snapshots_index_is_protected(plr, helix):
    path = helix.gc_path / "sub" / "part.gcode"
    path.write_bytes(layered_gcode())
    added(plr)
    plr.follower = None
    assert plr.protected_index_paths() == [
        helix_gcode_reader.layer_index_path(helix.gc_path, "sub/part.gcode")]


def test_a_waiting_qidi_snapshots_external_index_is_protected(plr, helix, tmp_path):
    """QIDI prints from printer_data/.cache, outside gcodes: the index the
    snapshot resumes from lives under .external and the prune keeps it."""
    cache = tmp_path / "printer_data" / ".cache"
    cache.mkdir(parents=True)
    printed = cache / "part.gcode"
    printed.write_bytes(layered_gcode())
    (helix.gc_path / "sub" / "part.gcode").write_bytes(layered_gcode())
    added(plr)
    plr.job["print_path"] = str(printed)
    plr.job["print_identity"] = helix_plr.file_identity(printed)
    plr.follower = None
    index = helix_gcode_reader.layer_index_path(helix.gc_path, ".external/part.gcode")
    assert plr.protected_index_paths() == [index]
    other = helix_gcode_reader.layer_index_path(helix.gc_path, ".external/old.gcode")
    for path in (index, other):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w").write("{}\n")
    plr._follow_executor = SyncExecutor()
    plr._prune_indexes()
    assert os.path.exists(index) and not os.path.exists(other)


def test_a_follower_never_writes_into_a_complete_index(gfile):
    data = gfile.read_bytes()
    index = helix_gcode_reader.layer_index_path(gfile.parent, "part.gcode")
    helix_gcode_reader.write_layer_index(index, ident(gfile), [], [], len(data))
    before = open(index, "rb").read()
    f = LayerFollower(gfile, index, ident(gfile), limiter=NoThrottle())
    f.step(len(data) // 2)
    f.finish(None)
    assert open(index, "rb").read() == before


def test_two_status_frames_at_once_save_one_snapshot(plr):
    import asyncio
    added(plr)
    saving = plr._saving

    async def slow_saving():
        await asyncio.sleep(0)
        return await saving()

    plr._saving = slow_saving
    writes = []
    write = plr.store.write
    plr.store.write = lambda payload: (writes.append(payload), write(payload))[1]

    async def both():
        await asyncio.gather(plr.on_status_update(printing_status(100)),
                             plr.on_status_update(printing_status(200, layer=2)))

    run(both())
    assert len(writes) == 1           # two at once would also race on the slot seq


# --------------------------------------------------------------------------
# Backend selection, QIDI routing details, the offer outliving the switch
# --------------------------------------------------------------------------


@pytest.mark.parametrize(
    "profile, qidi, safe, expected",
    [
        ({"tier": 0}, False, True, ("none", 0)),
        ({"tier": 0, "backend": "qidi_print_state"}, True, True, ("none", 0)),
        ({"tier": 1}, False, True, ("generic", 1)),
        ({"tier": 2}, False, True, ("generic", 1)),                      # Z may sag: jog
        ({"tier": 2, "z_holds_unpowered": True}, False, True, ("generic", 2)),
        ({"tier": 3, "z_holds_unpowered": True}, False, True, ("generic", 2)),
        ({"tier": 2, "z_holds_unpowered": True}, False, False, ("none", 0)),  # unsafe G28 X Y
        ({"tier": 2, "z_holds_unpowered": True}, True, True, ("generic", 2)),  # QIDI fw, generic
        ({"tier": 2, "z_holds_unpowered": True, "backend": "qidi_print_state"}, True, False,
         ("qidi_print_state", 2)),
        ({"tier": 2, "z_holds_unpowered": True, "backend": "qidi_print_state"}, False, True,
         ("none", 0)),                                                   # no QIDI endpoint
        ({"tier": 1, "backend": "qidi_print_state"}, True, True, ("none", 0)),   # no jog on QIDI
        ({"tier": 2, "backend": "qidi_print_state"}, True, True, ("none", 0)),   # Z unproven
        ({"tier": 2, "z_holds_unpowered": True, "backend": "vendor"}, True, True, ("none", 0)),
    ],
)
def test_backend_selection(plr, profile, qidi, safe, expected):
    plr.qidi = object() if qidi else None
    plr.homing = HomingCheck(True, None) if safe else HomingCheck(False, "override_homes_z")
    assert plr.backend(parse_profile(profile)) == expected


def test_qidi_refuses_a_cache_file_other_than_the_one_printed(plr, helix, qidi_resume):
    _, cache = qidi_resume
    other = cache.parent / "elsewhere.gcode"
    other.write_bytes((cache / "part.gcode").read_bytes())
    plr.job["print_path"] = str(other)
    plr.job["print_identity"] = helix_plr.file_identity(other)
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    with pytest.raises(Exception, match="qidi_cache_mismatch"):
        commit(plr, plan_id)


def test_a_failing_qidi_row_write_does_not_stop_the_snapshot_loop(plr, helix, qidi_printer):
    async def boom(*a, **k):
        raise RuntimeError("bad row")

    plr.qidi.on_snapshot = boom
    added(plr, job_id="0000G1")
    tick(plr, 3.0, **printing_status(100))
    tick(plr, 1.0, **printing_status(200))
    assert plr.store.load()["file_position"] == 200


def test_a_skipped_chamber_wait_is_not_waited_for_in_commit(plr, helix, resume):
    record = plr.store.load()
    record["heaters"]["heater_generic chamber"] = {"target": 45.0, "temp": 25.0}
    plr.store.write(fit_record(record)[0])
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.heats = False
    plr.printer.bed = 60.0
    plr.printer.nozzle = 220.0
    run(plr.handle_prepare(Req(plan_id=plan_id, skip_chamber_wait=True)))
    assert commit(plr, plan_id) == {"ok": True, "job_started": True}


def test_turning_saving_off_keeps_an_existing_offer(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_user", {"enabled": False}))
    entry = run(helix._handle_status(None))["features"]["plr"]
    assert entry["enabled"] is True                  # a snapshot is waiting
    assert entry["saving"] is False                  # what the settings switch shows
    assert run(plr.handle_status(Req()))["offer"]["available"] is True
    run(plr.handle_discard(Req()))
    assert run(helix._handle_status(None))["features"]["plr"]["enabled"] is False


def test_a_next_layer_resume_needs_accept_next_layer(plr, helix, resume):
    record = plr.store.load()
    record["live_pos"] = [150.0, 150.0, 0.8, 0.0]          # nowhere near any line
    plr.store.write(fit_record(record)[0])
    reply = run(plr.handle_plan(Req()))
    assert reply["method"] == "layer_start" and "layer_start" in reply["warnings"]
    assert reply["requires"][-1] == "accept_next_layer"
    run(plr.handle_prepare(Req(plan_id=reply["plan_id"])))
    with pytest.raises(Exception, match="plr_missing_confirmation: accept_next_layer"):
        commit(plr, reply["plan_id"])
    assert run(plr.handle_commit(Req(plan_id=reply["plan_id"], z_reference="trusted",
                                     confirmations=dict(COMMIT_OK, accept_next_layer=True)))) \
        == {"ok": True, "job_started": True}


# --------------------------------------------------------------------------
# Round 2: the lift after the nozzle went down, abort then reopen, nozzle wait
# --------------------------------------------------------------------------


def tier1(plr, helix):
    run(helix.database.insert_item("helix-screen", "plr_profile", {"tier": 1}))


def test_reprepare_after_a_tier1_jog_lifts_before_homing(plr, helix, resume):
    tier1(plr, helix)
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.z = 0.8                                  # the user jogged down onto the part
    run(plr.handle_abort(Req(plan_id=plan_id)))
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    before = len(sent(helix))
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    again = sent(helix)[before:]
    assert not any(s.startswith("SET_KINEMATIC_POSITION") for s in again)
    assert again.index("G91\nG1 Z5 F300\nG90") < again.index("G28 X Y")


def test_reprepare_after_a_failed_commit_and_a_restart_is_refused(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.nozzle = 220.0
    plr.printer.fail("M83\nG1 X", "Heater extruder not heating at expected rate")   # the purge
    with pytest.raises(Exception, match="not heating"):
        commit(plr, plan_id)
    plr.printer.fail_on = None
    plr.printer.homed = ""                               # the heater fault shut Klipper down
    plr.on_klippy_gone()
    plr.klippy_ready = True
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    before = len(sent(helix))
    with pytest.raises(Exception, match="plr_stale_plan: position_unknown"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert sent(helix)[before:] == []


def test_a_homed_lift_fits_under_the_top_of_travel(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.printer.homed = "z"
    plr.printer.z = 246.0
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert "G91\nG1 Z3 F300\nG90" in sent(helix)


def test_abort_then_reopen_waits_for_the_old_run(plr, helix, resume):
    import asyncio
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.nozzle = 220.0
    real = plr.printer.run_gcode

    async def scenario():
        entered, release = asyncio.Event(), asyncio.Event()

        async def gcode(script):
            if script.startswith("SET_GCODE_OFFSET"):          # a long step
                entered.set()
                await release.wait()
            return await real(script)

        helix.klippy_apis.run_gcode.side_effect = gcode
        old = asyncio.ensure_future(plr.handle_commit(Req(
            plan_id=plan_id, z_reference="trusted", confirmations=COMMIT_OK)))
        await entered.wait()
        await plr.handle_abort(Req(plan_id=plan_id))
        aborted_at = len(sent(helix))
        refused = []
        for call in (plr.handle_plan(Req()), plr.handle_prepare(Req(plan_id=plan_id))):
            try:
                await call
            except Exception as e:
                refused.append(str(e))
        release.set()
        try:
            await old
        except Exception as e:
            refused.append(str(e))
        return refused, sent(helix)[aborted_at:]

    refused, after = run(scenario())
    # No new plan while the old run is inside its step, so no new prepare either.
    assert "plr_busy" in refused[0] and "aborted" in refused[2]
    assert "plr_busy" in refused[1] or "plr_no_plan" in refused[1]
    assert all(s == HEATERS_OFF for s in after)       # the old run sends nothing more
    assert run(plr.handle_plan(Req()))["method"] == "measured"   # free again once it exited


def test_abort_during_the_nozzle_wait_skips_the_purge(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.heats = False
    plr.printer.bed = 60.0
    plr.printer.nozzle = 25.0                       # cooled while the screen asked

    async def abort_while_waiting(seconds):
        await plr.handle_abort(Req(plan_id=plan_id))

    plr._sleep = abort_while_waiting
    with pytest.raises(Exception, match="aborted"):
        commit(plr, plan_id)
    assert not any(s.startswith("M83\nG1 X") for s in sent(helix))
    assert any(s.endswith("M104 S220") for s in sent(helix))


def test_the_nozzle_wait_times_out(plr, helix, resume):
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.heats = False
    plr.printer.bed = 60.0
    plr.printer.nozzle = 25.0                       # cooled while the screen asked
    with pytest.raises(Exception, match="nozzle_in_place timed out"):
        commit(plr, plan_id)


def test_old_klipper_without_set_homed_refuses_an_override(plr, helix, resume):
    plr.printer.old_klipper = True
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    plr.homing = HomingCheck(True, None, True)
    with pytest.raises(Exception, match="set_homed_unsupported"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert "G28 X Y" not in sent(helix)


def test_old_klipper_with_plain_g28_still_prepares(plr, helix, resume):
    plr.printer.old_klipper = True
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert "G28 X Y" in sent(helix)


def test_tier1_with_nowhere_to_heat_is_refused(plr, helix, resume):
    tier1(plr, helix)
    record = plr.store.load()
    record["objects"] = []
    plr.store.write(fit_record(record)[0])

    async def no_park(objects):
        out = await plr.printer.query(objects)
        if "configfile" in out:
            out["configfile"]["settings"].pop("stepper_x")
        return out

    helix.klippy_apis.query_objects.side_effect = no_park
    with pytest.raises(Exception, match="plr_scan_failed: no_purge_location"):
        run(plr.handle_plan(Req()))


@pytest.mark.parametrize("gcode", ["{% if 'x' not in params %}_HOME_Z{% endif %}\nG28 X Y",
                                   "{% if True %}G28 Z{% endif %}"])
def test_one_line_template_conditionals_are_read(gcode):
    check = check_xy_homing({"homing_override": {"axes": "xyz", "gcode": gcode}}, "corexy")
    assert check.xy_home_safe is False


def test_a_restart_after_a_tier1_jog_refuses_rather_than_declaring_the_lift(plr, helix, resume):
    tier1(plr, helix)
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    plr.printer.z = 0.8                                  # jogged down onto the part
    run(plr.handle_abort(Req(plan_id=plan_id)))
    plr.printer.homed = ""                               # then Klipper restarted
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    with pytest.raises(Exception, match="plr_stale_plan: position_unknown"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))


def test_old_klipper_refuses_a_custom_xy_homing_command(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_profile", {
        "tier": 2, "z_holds_unpowered": True, "xy_home_gcode": "_CG28 X Y"}))
    plr.printer.old_klipper = True
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    with pytest.raises(Exception, match="set_homed_unsupported"):
        run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert not any(s.startswith("_CG28") for s in sent(helix))


def test_a_custom_xy_homing_command_runs_where_set_homed_works(plr, helix, resume):
    run(helix.database.insert_item("helix-screen", "plr_profile", {
        "tier": 2, "z_holds_unpowered": True, "xy_home_gcode": "_CG28 X Y"}))
    plan_id = run(plr.handle_plan(Req()))["plan_id"]
    run(plr.handle_prepare(Req(plan_id=plan_id)))
    assert "_CG28 X Y" in sent(helix)


# --------------------------------------------------------------------------
# The mesh-include line in printer.cfg (server.helix.plr.mesh_include)
# --------------------------------------------------------------------------

def printer_cfg(plr, text=None):
    """The config dir's printer.cfg; written when `text` is given."""
    path = plr._config_dir() / "printer.cfg"
    if text is not None:
        path.write_text(text)
    return path


def test_mesh_include_status_reports_the_line(plr):
    printer_cfg(plr, "[printer]\nkinematics: corexy\n")
    assert run(plr.handle_mesh_include(Req())) == {"present": False}
    printer_cfg(plr, "[printer]\n[include helix-plr/*.cfg]\n")
    assert run(plr.handle_mesh_include(Req())) == {"present": True}


def test_mesh_include_add_writes_the_lines_and_a_rolling_backup(plr):
    cfg = printer_cfg(plr, "[printer]\nkinematics: corexy\n")
    reply = run(plr.handle_mesh_include(Req(action="add")))
    assert reply == {"present": True, "changed": True}
    assert cfg.read_text() == (
        "[printer]\n"
        "kinematics: corexy\n"
        "# Added by HelixScreen: lets a power-loss resume reload the print's bed mesh\n"
        "[include helix-plr/*.cfg]\n")
    backup = cfg.parent / "printer.cfg.helixscreen-bak"
    assert backup.read_text() == "[printer]\nkinematics: corexy\n"


def test_mesh_include_add_appends_after_a_file_with_no_final_newline(plr):
    printer_cfg(plr, "[printer]\nkinematics: corexy")
    run(plr.handle_mesh_include(Req(action="add")))
    assert printer_cfg(plr).read_text().startswith("[printer]\nkinematics: corexy\n#")


def test_mesh_include_add_is_idempotent(plr):
    printer_cfg(plr, "[printer]\n")
    assert run(plr.handle_mesh_include(Req(action="add"))) == {"present": True,
                                                               "changed": True}
    before = printer_cfg(plr).read_text()
    backup = plr._config_dir() / "printer.cfg.helixscreen-bak"
    backup.write_text("sentinel")
    assert run(plr.handle_mesh_include(Req(action="add"))) == {"present": True,
                                                               "changed": False}
    assert printer_cfg(plr).read_text() == before
    assert backup.read_text() == "sentinel"


def test_mesh_include_add_on_a_hand_added_line_changes_nothing(plr):
    printer_cfg(plr, "[printer]\n[include helix-plr/*.cfg]\n")
    assert run(plr.handle_mesh_include(Req(action="add"))) == {"present": True,
                                                               "changed": False}
    assert not (plr._config_dir() / "printer.cfg.helixscreen-bak").exists()


def test_mesh_include_add_replaces_a_lone_marker(plr):
    printer_cfg(plr, "[printer]\n"
                     "# Added by HelixScreen: lets a power-loss resume reload the print's bed mesh\n")
    run(plr.handle_mesh_include(Req(action="add")))
    text = printer_cfg(plr).read_text()
    assert text.count("# Added by HelixScreen") == 1


def test_mesh_include_remove_strips_only_our_lines(plr):
    cfg = printer_cfg(plr, "[printer]\n"
                           "  # Added by HelixScreen: lets a power-loss resume reload the print's bed mesh\n"
                           "[include helix-plr/*.cfg]  \n"
                           "[include helix-plr/mesh.cfg]\n"
                           "[include my_macros/*.cfg]\n")
    reply = run(plr.handle_mesh_include(Req(action="remove")))
    assert reply == {"present": False, "changed": True}
    assert cfg.read_text() == ("[printer]\n"
                               "[include helix-plr/mesh.cfg]\n"
                               "[include my_macros/*.cfg]\n")
    backup = cfg.parent / "printer.cfg.helixscreen-bak"
    assert "helix-plr/*.cfg" in backup.read_text()


def test_mesh_include_remove_is_idempotent(plr):
    printer_cfg(plr, "[printer]\n")
    assert run(plr.handle_mesh_include(Req(action="remove"))) == {"present": False,
                                                                  "changed": False}
    assert not (plr._config_dir() / "printer.cfg.helixscreen-bak").exists()


def test_mesh_include_refuses_when_printer_cfg_cannot_be_read(plr):
    with pytest.raises(Exception, match="plr_config_read"):
        run(plr.handle_mesh_include(Req()))


def test_mesh_include_refuses_when_the_write_fails(plr, monkeypatch):
    printer_cfg(plr, "[printer]\n")

    def disk_full(*args, **kwargs):
        raise OSError("disk full")

    monkeypatch.setattr(helix_plr, "write_mesh_include_change", disk_full)
    with pytest.raises(Exception, match="plr_config_write"):
        run(plr.handle_mesh_include(Req(action="add")))
    assert printer_cfg(plr).read_text() == "[printer]\n"


def test_mesh_include_refuses_an_unknown_action(plr):
    printer_cfg(plr, "[printer]\n")
    with pytest.raises(Exception, match="plr_bad_action"):
        run(plr.handle_mesh_include(Req(action="toggle")))


def test_a_failed_rename_leaves_printer_cfg_unchanged(plr):
    cfg = printer_cfg(plr, "[printer]\n")

    def replace_fails(src, dst):
        raise OSError("disk full")

    with pytest.raises(OSError):
        helix_plr.write_mesh_include_change(cfg, "[printer]\n", "[printer]\n# more\n",
                                            replace_fails)
    assert cfg.read_text() == "[printer]\n"
    assert (cfg.parent / "printer.cfg.helixscreen-bak").read_text() == "[printer]\n"


def test_uninstall_mesh_include_strips_the_lines_and_the_directory(plr, tmp_path):
    config = plr._config_dir()
    (config / "printer.cfg").write_text(
        "[printer]\n# Added by HelixScreen: lets a power-loss resume reload the print's bed mesh\n"
        "[include helix-plr/*.cfg]\n")
    (config / "helix-plr").mkdir()
    (config / "helix-plr" / "mesh.cfg").write_text("[bed_mesh helix_plr]\n")

    assert helix_plr.uninstall_mesh_include(config) is None

    assert (config / "printer.cfg").read_text() == "[printer]\n"
    assert (config / "printer.cfg.helixscreen-bak").read_text().count("helix-plr") == 1
    assert not (config / "helix-plr").exists()


def test_uninstall_mesh_include_with_nothing_there_touches_nothing(plr):
    config = plr._config_dir()
    (config / "printer.cfg").write_text("[printer]\n")

    assert helix_plr.uninstall_mesh_include(config) is None

    assert (config / "printer.cfg").read_text() == "[printer]\n"
    assert not (config / "printer.cfg.helixscreen-bak").exists()


def test_mesh_include_refuses_a_config_that_is_not_utf8(plr):
    (plr._config_dir() / "printer.cfg").write_bytes(b"\xff\xfe[printer]\n")
    with pytest.raises(Exception, match="plr_config_read"):
        run(plr.handle_mesh_include(Req()))
    with pytest.raises(Exception, match="plr_config_read"):
        run(plr.handle_mesh_include(Req(action="add")))


def test_uninstall_mesh_include_reports_a_config_that_is_not_utf8(plr):
    config = plr._config_dir()
    (config / "printer.cfg").write_bytes(b"\xff\xfe[printer]\n")
    problem = helix_plr.uninstall_mesh_include(config)
    assert problem is not None
    assert "printer.cfg" in problem
