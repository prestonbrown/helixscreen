#!/usr/bin/env python3
"""Extract a mock-replay script from the K1C capture (klippy + helix app log).

One print window -> a timed event list the MoonrakerClientMock can replay
through its real dispatch paths (notify_gcode_response + notify_status_update),
so the full app - manager wiring, MoonrakerAPI callbacks, collector, subjects -
exercises against captured data with no printer attached.

Sources (already pulled to /tmp/k1c-capture):
  klippy.log  - respond_info lines (forwarded console), PRTOUCH_MOVE positions
  helix.1.log - the app's record of the REAL bed_mesh WS pushes (keys + timing)
Events use klippy-clock relative ms; the app log runs +1h ahead, corrected here.
"""
import json
import re

KLIPPY = "/tmp/k1c-capture/extracted/usr/data/printer_data/logs/klippy.log"
APPLOG = "/tmp/k1c-capture/extracted/usr/data/helixscreen/logs/helix.1.log"

# klippy-clock window of the flowrate print's prep (pre-start block + print)
START, END = "22:18:00", "22:24:45"

TS_RE = re.compile(r"^\[INFO\] 2026-08-19 (\d\d:\d\d:\d\d),(\d+)")
RESP_RE = re.compile(r"\[gcode:respond_info:301\] (.*)$")
MOVE_RE = re.compile(r"\[PRTOUCH_MOVE\] Start Move, gcmd=G1 F[\d.]+ X([\d.]+) Y([\d.]+) Z([\d.]+)")

def tms(h, m, s, ms):
    return ((int(h) * 60 + int(m)) * 60 + int(s)) * 1000 + int(ms)

def parse_t(t, frac):
    return tms(t[0:2], t[3:5], t[6:8], frac.ljust(3, "0")[:3])

base = None
events = []

with open(KLIPPY, errors="replace") as f:
    for line in f:
        m = TS_RE.match(line)
        if not m:
            continue
        t = m.group(1)
        if not (START <= t <= END):
            continue
        rel = parse_t(t, m.group(2))
        if base is None:
            base = rel
        rel -= base
        rm = RESP_RE.search(line)
        if rm:
            events.append({"t": rel, "type": "gcode_response", "line": "// " + rm.group(1)})
            continue
        mm = MOVE_RE.search(line)
        if mm:
            events.append({"t": rel, "type": "status", "object": "toolhead",
                           "payload": {"position": [float(mm.group(1)), float(mm.group(2)),
                                                    float(mm.group(3)), 0.0]}})

# Bed-mesh WS pushes from the app log (clock +1h => klippy = app - 1h).
# Shapes from the real payloads: present pushes carry the full 5x5 matrix
# (captured verbatim from klippy's _handle_query dump), the clear carries
# probed_matrix: null.
MATRIX = [[-0.083781, -0.106906, -0.087213, 0.001906, 0.065813],
          [-0.074028, -0.11167, -0.104185, -0.06775, 0.025625],
          [-0.042991, -0.062344, -0.094125, -0.133065, -0.042063],
          [-0.186094, -0.26725, -0.228781, -0.248063, -0.159906],
          [-0.29625, -0.333449, -0.386869, -0.372781, -0.232437]]
PRESENT = {"profile_name": "default", "mesh_min": [5.0, 5.0], "mesh_max": [215.0, 215.0],
           "probed_matrix": MATRIX}
CLEAR = {"profile_name": None, "mesh_min": [5.0, 5.0], "mesh_max": [215.0, 215.0],
         "probed_matrix": None}

pushes = []  # (klippy_rel_ms, present)
with open(APPLOG, errors="replace") as f:
    for line in f:
        m = re.match(r"^\[2026-08-19 (2\d:\d\d:\d\d)\.(\d+)\]", line)
        if not m:
            continue
        app_t = m.group(1)
        # app window 23:18:00-23:24:45 == klippy window - 1h
        if not ("23:18:00" <= app_t <= "23:24:45"):
            continue
        rel = parse_t(app_t, m.group(2)) - 3600 * 1000 - tms(22, 18, 0, "0")  # app->klippy -1h, window base
        if "Bed mesh data cleared" in line:
            pushes.append((rel, False))
        elif "Bed mesh updated: profile='default'" in line:
            pushes.append((rel, True))
for rel, present in pushes:
    if rel < 0:
        continue
    events.append({"t": rel, "type": "status", "object": "bed_mesh",
                   "payload": PRESENT if present else CLEAR})

# Heater targets (from the app log's recompute lines): staged K1C heating.
events += [
    {"t": 900, "type": "status", "object": "extruder",
     "payload": {"temperature": 27.0, "target": 130.0}},
    {"t": 950, "type": "status", "object": "heater_bed",
     "payload": {"temperature": 26.0, "target": 55.0}},
    {"t": 330000, "type": "status", "object": "extruder",
     "payload": {"temperature": 129.0, "target": 220.0}},
]

# Arming edge first, completion marker last.
events.insert(0, {"t": 0, "type": "status", "object": "print_stats",
                  "payload": {"state": "printing", "filename": "flowrate_0_PLA_36m5s.gcode",
                              "print_duration": 0.0, "progress": 0.0}})
events.sort(key=lambda e: e["t"])

out = {
    "description": "K1C flowrate print 2026-08-19: pre-start gcode block (CX_ROUGH_G28 "
                   "EXTRUDER_TEMP=220 BED_TEMP=55, CX_NOZZLE_CLEAR, ACCURATE_G28, "
                   "BED_MESH_CALIBRATE, PRINT_PREPARED). Extracted from the klippy.log + "
                   "helix app log capture in /tmp/k1c-capture (clocks: app = klippy + 1h).",
    "config_probe_count": [5, 5],
    "events": events,
}
dest = "tests/fixtures/k1c_flowrate_replay.json"
with open(dest, "w") as f:
    json.dump(out, f, indent=1)
kinds = {}
for e in events:
    key = e["type"] + (":" + e["object"] if e["type"] == "status" else "")
    kinds[key] = kinds.get(key, 0) + 1
print(dest, "events:", len(events), kinds, "span_ms:", events[-1]["t"])
