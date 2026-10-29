# Measure pressure advance with the nozzle's force sensor.
#
# HELIX_PA_CALIBRATE [TEMP=] [MODE=full|quick|auto] [FILAMENT=<key>]
#     [CENTER=<k>] [PRINT_START=0|1]
#                     purge position, heat, feed, flow steps at a range of PA
#                     values while logging nozzle force, fit, apply, wipe.
#                     Console: "HELIX_PA: step <i>/<n>",
#                     "HELIX_PA: measured K=<k> (...)", "HELIX_PA: no result: ..."
# HELIX_PA_LOG_START / HELIX_PA_LOG_MARK LABEL= / HELIX_PA_LOG_STOP NAME=
#                     the raw capture, for diagnostics
#
# Force sources, first that answers wins:
#   prtouch    stock Creality K2: the nozzle MCU's own pressure commands on the
#              channel prtouch_v3 configured, through handles built here
#   load_cell  upstream Klipper / Kalico [load_cell]
#
# Installed and kept current by the HelixPrint Moonraker plugin. Method and
# wire protocol adapted from grant0013/K2-OpenKlipper (GPLv3).
#
# This file may be distributed under the terms of the GNU GPLv3 license.
import json
import logging
import os
import subprocess
import sys
import time

VERSION = 1
FILAMENT_AREA = 2.405  # mm^2, 1.75 mm filament
FULL_KS = [0.02, 0.04, 0.06, 0.08, 0.10, 0.12, 0.14]
FIT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "helix_pa_fit.py")

ACK_FMT = "ack_prtouch oid=%c err=%c expar0=%u expar1=%u"
START_FMT = ("start_prtouch_pres oid=%c cfg_regs=%c acq_tick=%u ned_tftr=%c "
             "ned_hftr=%c ned_lftr=%u min_hold=%i max_hold=%i add_hold=%i "
             "lmt_dead=%u")
READ_FMT = "read_prtouch_pres oid=%c is_src=%c ch=%c idx=%c len=%c"
READ_RESP_FMT = ("resault_prtouch_pres oid=%c tri_chxs=%c buf_len=%c ch=%c "
                 "idx=%c len=%c ticks=%.*s datas=%.*s")
STOP_FMT = "stop_prtouch_pres oid=%c sta_swap=%c"
BUF_LEN = 64
ACQ_S = 0.00078125
WINDOW_PERIOD = 0.056
# Contact thresholds out of reach: a flow step otherwise looks like a nozzle
# tap and the MCU freezes its buffer mid-step.
HOLD_MIN, HOLD_MAX = 50000, 100000


def _num(value):
    """Leading number of a config value (raw values can carry comments)."""
    if isinstance(value, (int, float)):
        return float(value)
    text = str(value).strip().split()[0] if str(value).strip() else ""
    try:
        return float(text)
    except ValueError:
        return None


def unzip_data(blob, signed=True):
    """Decode the nozzle MCU's delta-packed sample array."""
    if not blob:
        return []
    count = blob[0]
    sel_count = (count + 3) // 4
    pos = 1 + sel_count
    out = []
    acc = 0
    for i in range(count):
        sel = blob[sel_count - i // 4]
        width = ((sel >> ((i % 4) * 2)) & 3) + 1
        acc += int.from_bytes(blob[pos:pos + width], "little", signed=True)
        pos += width
        if not signed:
            acc &= 0xffffffff
        out.append(acc)
    return out


# --- routine profiles -------------------------------------------------------

def profile_from_settings(settings, own):
    """Where and how this printer runs the routine: (profile, None) or
    (None, reason). Creality CFS printers name their purge chute in [box];
    anything else names a purge position in [helix_pa_gauge]."""
    box = settings.get("box") or {}
    epx, epy = _num(box.get("extrude_pos_x", "")), _num(box.get("extrude_pos_y", ""))
    if epx is not None and epy is not None:
        # Stock swings the nozzle from extrude_pos_x toward the wiper after a
        # purge; stay inside that span, clear of the wiper itself.
        wiper = _num(box.get("clean_pos_left_x", "")) or (epx + 27.)
        return {
            "name": "creality_cfs", "purge": (epx, epy),
            "wiggle": (epx, min(epx + 14., wiper - 5.)), "speed": 40.,
            "goto": ["BOX_GO_TO_EXTRUDE_POS"],
            "feed_start": ["BOX_START_PRINT"], "feed_stop": ["BOX_END_PRINT"],
            "wipe": ["BOX_NOZZLE_CLEAN"], "park": ["BOX_MOVE_TO_SAFE_POS"],
            "preflight": "creality_cfs",
        }, None
    px, py = _num(own.get("purge_x", "")), _num(own.get("purge_y", ""))
    if px is None or py is None:
        return None, "no purge position: set purge_x and purge_y in [helix_pa_gauge]"
    span = _num(own.get("wiggle", "")) or 14.
    goto = ["G90", "G1 X%.1f Y%.1f F6000" % (px, py)]
    split = lambda key: [l for l in str(own.get(key, "") or "").split("\n") if l.strip()]
    return {
        "name": "generic", "purge": (px, py), "wiggle": (px, px + span),
        "speed": _num(own.get("speed", "")) or 40., "goto": goto,
        "feed_start": split("feed_start_gcode"), "feed_stop": split("feed_stop_gcode"),
        "wipe": split("wipe_gcode"), "park": split("park_gcode"),
    }, None


def preflight(profile, status_of):
    """Why the routine cannot start now, or None. `status_of(name)` returns a
    printer object's status dict, or None."""
    if profile.get("preflight") != "creality_cfs":
        return None
    # The K2 keeps initialising its closed-loop motors after Klipper reports
    # ready; a homing move in that window breaks the motor bus.
    if not (status_of("motor_control") or {}).get("motor_ready"):
        return "the motors are still initialising, try again in a moment"
    # With no lane on record BOX_START_PRINT feeds nothing, and the extruder
    # grinds the filament against the box's take-up within seconds.
    box = status_of("box") or {}
    known = [unit.get("filament") for name, unit in box.items()
             if name[:1] == "T" and isinstance(unit, dict)]
    if not any(f not in (None, "", "None") for f in known):
        return "the CFS has no lane on record as loaded: load the filament through the CFS first"
    return None


def warmup_gcode(profile, slow=2.0, fast=12.0):
    """Prime and settle the melt before anything is logged."""
    xa, xb = profile["wiggle"]
    lines = ["G90", "M83", "G1 X%.1f Y%.1f F6000" % (xa, profile["purge"][1]), "G1 E15 F300"]
    for _ in range(2):
        lines += _wiggle(profile, fast, 4) + _wiggle(profile, slow, 4)
    return lines + ["G4 P2000"]


def _wiggle(profile, flow, count):
    xa, xb = profile["wiggle"]
    seconds = abs(xb - xa) / profile["speed"]
    e = flow / FILAMENT_AREA * seconds
    return ["G1 X%.1f E%.5f F%d" % (xb if i % 2 == 0 else xa, e, profile["speed"] * 60)
            for i in range(count)]


def sweep_gcode(profile, ks, cycles=8, slow=2.0, fast=12.0, name="helix_pa"):
    """The logged part: a dry wiggle, then per PA value `cycles` slow/fast
    flow steps. Every logged extrusion moves X, because Klipper applies PA
    only to moves with X/Y."""
    xa, xb = profile["wiggle"]
    lines = ["SET_PRESSURE_ADVANCE ADVANCE=0", "HELIX_PA_LOG_START", "G4 P1000",
             "HELIX_PA_LOG_MARK LABEL=dry"]
    lines += ["G1 X%.1f F%d" % (xb if i % 2 == 0 else xa, profile["speed"] * 60)
              for i in range(6)]
    lines.append("G4 P1500")
    for k in ks:
        lines += ["SET_PRESSURE_ADVANCE ADVANCE=%g" % k, "G4 P1500",
                  "HELIX_PA_LOG_MARK LABEL=k%g" % k]
        for _ in range(cycles):
            lines += _wiggle(profile, slow, 4) + _wiggle(profile, fast, 4)
        lines += _wiggle(profile, slow, 4)
        lines += ["HELIX_PA_LOG_MARK LABEL=end%g" % k, "G4 P1500"]
    lines.append("HELIX_PA_LOG_STOP NAME=%s" % name)
    return lines


def quick_ks(center, spacing=0.02):
    """Four values straddling `center`, none below 0.005."""
    low = max(0.005, center - 1.5 * spacing)
    return [round(low + i * spacing, 4) for i in range(4)]


# --- measured cache ---------------------------------------------------------

class PaCache:
    """Measured K per filament and nozzle temperature, for MODE=auto and the
    apply at a filament change. The user-facing record is the screen's."""

    def __init__(self, path):
        self.path = path
        try:
            with open(path) as f:
                data = json.load(f)
            self.entries = dict(data.get("entries", {}))
        except (OSError, ValueError, AttributeError):
            self.entries = {}

    @staticmethod
    def _key(filament, temp):
        return "%s@%d" % (filament, int(round(temp)))

    def get(self, filament, temp):
        return self.entries.get(self._key(filament, temp))

    def put(self, filament, temp, k, stamp=None):
        self.entries[self._key(filament, temp)] = {
            "k": k, "stamp": stamp if stamp is not None else time.time()}
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"version": 1, "entries": self.entries}, f, indent=1)
        os.replace(tmp, self.path)


# --- force sources ----------------------------------------------------------

class PrtouchSource:
    """Creality's nozzle MCU, read in 64-sample windows that carry MCU ticks.
    Only HELIX_PA_CALIBRATE and the log commands sample, and G-code runs one
    command at a time, so a probe never shares the channel with us."""
    name = "prtouch"

    def __init__(self, printer):
        pres = getattr(printer.lookup_object("prtouch_v3", None), "pres", None)
        self.oid = getattr(pres, "oid", None)
        self.mcu = getattr(pres, "mcu", None)
        if self.oid is None or self.mcu is None:
            raise LookupError("no nozzle force sensor")
        cq = self.mcu.alloc_command_queue()
        q = self.mcu.lookup_query_command
        self.start_cmd = q(START_FMT, ACK_FMT, oid=self.oid, cq=cq)
        self.read_cmd = q(READ_FMT, READ_RESP_FMT, oid=self.oid, cq=cq)
        self.stop_cmd = q(STOP_FMT, ACK_FMT, oid=self.oid, cq=cq)
        self.reactor = printer.get_reactor()
        self.timer = None
        self.sink = None
        self.streaming = False

    def print_time(self):
        return self.mcu.estimated_print_time(self.reactor.monotonic())

    def begin(self, sink):
        self.sink = sink
        self.timer = self.reactor.register_timer(self._poll, self.reactor.NOW)

    def end(self):
        if self.timer is not None:
            self.reactor.unregister_timer(self.timer)
            self.timer = None
        if self.streaming:
            self._stop()

    def _start(self):
        self.start_cmd.send([self.oid, 60, int(self.mcu.seconds_to_clock(ACQ_S)),
                             0, 0, 0, HOLD_MIN, HOLD_MAX, 0, 0])
        self.streaming = True

    def _stop(self):
        self.stop_cmd.send([self.oid, 0])
        self.streaming = False

    def _read_window(self):
        c64 = self.mcu.clock32_to_clock64
        out = []
        idx = 0
        while idx < BUF_LEN:
            resp = self.read_cmd.send([self.oid, 0, 0, idx, BUF_LEN - idx])
            n = resp.get("len", 0) if isinstance(resp, dict) else 0
            if not n:
                break
            vals = unzip_data(resp.get("datas", b""), signed=True)
            ticks = unzip_data(resp.get("ticks", b""), signed=False)
            for i, (tk, v) in enumerate(zip(ticks, vals)):
                # Unfilled slots carry zero ticks; the first sample after a
                # restart reads the offset.
                if tk and idx + i:
                    out.append((self.mcu.clock_to_print_time(c64(tk & 0xffffffff)), v))
            idx += n
        return out

    def _poll(self, eventtime):
        try:
            if self.streaming:
                # Read before stopping: a stop empties the buffer.
                self.sink(self._read_window())
                self._stop()
            self._start()
        except Exception:
            logging.exception("helix_pa_gauge: window read failed")
        return self.reactor.monotonic() + WINDOW_PERIOD


class LoadCellSource:
    """Upstream / Kalico load cell: a continuous stream of rows starting
    (time, grams, counts, ...). A bare [load_cell] is its own object; one
    inside [load_cell_probe] is reached through get_load_cell()."""
    name = "load_cell"

    def __init__(self, printer):
        self.cell = printer.lookup_object("load_cell", None)
        probe = printer.lookup_object("load_cell_probe", None)
        if self.cell is None and hasattr(probe, "get_load_cell"):
            self.cell = probe.get_load_cell()
        if self.cell is None or not hasattr(self.cell, "add_client"):
            raise LookupError("no nozzle force sensor")
        self.printer = printer
        self.active = False

    def print_time(self):
        mcu = self.printer.lookup_object("mcu")
        return mcu.estimated_print_time(self.printer.get_reactor().monotonic())

    def begin(self, sink):
        self.active = True

        def on_samples(msg):
            if not self.active:
                return False
            sink([(row[0], row[2]) for row in msg.get("data", [])])
            return True
        self.cell.add_client(on_samples)

    def end(self):
        self.active = False


SOURCES = (PrtouchSource, LoadCellSource)


# --- the companion ----------------------------------------------------------

class HelixPaGauge:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.reactor = self.printer.get_reactor()
        self.gcode = self.printer.lookup_object("gcode")
        self.own = {key: config.get(key, None) for key in (
            "purge_x", "purge_y", "wiggle", "speed", "feed_start_gcode",
            "feed_stop_gcode", "wipe_gcode", "park_gcode")}
        self.out_dir = config.get("output_dir", "/tmp/helix_pa_gauge")
        self.cache_path = config.get("cache_path", None)
        self.source = self.dtrapq = self.profile = self.cache = None
        self.available, self.reason = False, "not ready"
        self.running = False
        self.last = {}
        self.ks = []
        self._reset_log()
        self.printer.register_event_handler("klippy:ready", self._ready)
        for name, desc in (
                ("HELIX_PA_CALIBRATE", "Measure pressure advance with the nozzle force sensor"),
                ("HELIX_PA_LOG_START", "Start logging nozzle force"),
                ("HELIX_PA_LOG_MARK", "Tag the next move in the force log"),
                ("HELIX_PA_LOG_STOP", "Stop logging and write the CSV files")):
            self.gcode.register_command(name, getattr(self, "cmd_" + name), desc=desc)

    # A missing or differently shaped object leaves the commands refusing
    # with a reason; it must never fail Klipper's ready callback.
    def _ready(self):
        try:
            self.available, self.reason = self._setup()
        except Exception as e:
            logging.exception("helix_pa_gauge: setup failed")
            self.available, self.reason = False, "setup failed: %s" % (e,)

    def _setup(self):
        reasons = []
        for cls in SOURCES:
            try:
                self.source = cls(self.printer)
                break
            except LookupError as e:
                reasons.append(str(e))
        if self.source is None:
            return False, reasons[0] if reasons else "no nozzle force sensor"
        mr = self.printer.lookup_object("motion_report", None)
        # Upstream names the map dtrapqs; Creality's older fork, trapqs.
        trapqs = getattr(mr, "dtrapqs", None) or getattr(mr, "trapqs", None) or {}
        self.dtrapq = trapqs.get("extruder")
        if self.dtrapq is None:
            return False, "no extruder motion report"
        configfile = self.printer.lookup_object("configfile", None)
        settings = configfile.get_status(self.reactor.monotonic()).get("settings", {}) \
            if configfile is not None else {}
        self.profile, reason = profile_from_settings(settings, self.own)
        if self.profile is None:
            return False, reason
        self.cache = PaCache(self.cache_path or os.path.join(
            self._config_dir(), "helix_pa_gauge.json"))
        return True, None

    def _config_dir(self):
        """Beside printer.cfg, so the cache survives a reboot and a backup
        of the config directory carries it."""
        start_args = getattr(self.printer, "get_start_args", lambda: {})()
        config_file = start_args.get("config_file")
        return os.path.dirname(config_file) if config_file else self.out_dir

    def get_status(self, eventtime):
        return {"version": VERSION, "available": self.available, "reason": self.reason,
                "source": getattr(self.source, "name", None),
                "profile": (self.profile or {}).get("name"),
                "running": self.running, "last": dict(self.last)}

    # --- capture ----------------------------------------------------------

    def _reset_log(self):
        self.samples, self.moves, self.marks = [], {}, []
        self.logging = False
        self.pulled_to = 0.

    def _sink(self, samples):
        self.samples.extend(samples)
        self._pull_moves()

    def _pull_moves(self):
        end = self.source.print_time()
        moves, _ = self.dtrapq.extract_trapq(self.pulled_to, end)
        for m in moves:
            self.moves[round(m.print_time, 6)] = (m.move_t, m.start_v, m.accel, m.start_x)
        self.pulled_to = max(self.pulled_to, end - 2.)

    def _require(self, gcmd):
        if not self.available:
            raise gcmd.error("HELIX_PA: no result: %s" % self.reason)

    def cmd_HELIX_PA_LOG_START(self, gcmd):
        self._require(gcmd)
        if self.logging:
            raise gcmd.error("HELIX_PA: the force log is already running")
        self._reset_log()
        self.logging = True
        self.pulled_to = self.source.print_time() - 1.
        self.source.begin(self._sink)

    def cmd_HELIX_PA_LOG_MARK(self, gcmd):
        label = gcmd.get("LABEL", "")
        toolhead = self.printer.lookup_object("toolhead")
        self.marks.append((toolhead.get_last_move_time(), label))
        if label.startswith("k") and label[1:] in ["%g" % k for k in self.ks]:
            index = ["%g" % k for k in self.ks].index(label[1:]) + 1
            gcmd.respond_info("HELIX_PA: step %d/%d" % (index, len(self.ks)))

    def cmd_HELIX_PA_LOG_STOP(self, gcmd):
        if not self.logging:
            raise gcmd.error("HELIX_PA: the force log is not running")
        self.printer.lookup_object("toolhead").wait_moves()
        # Let the last window land before closing the stream.
        self.reactor.pause(self.reactor.monotonic() + 2 * WINDOW_PERIOD)
        self.source.end()
        self.logging = False
        self._pull_moves()
        base = self._write_csv(gcmd.get("NAME", "helix_pa"))
        gcmd.respond_info("HELIX_PA: logged %d samples, %d moves -> %s_*"
                          % (len(self.samples), len(self.moves), base))

    def _write_csv(self, name):
        os.makedirs(self.out_dir, exist_ok=True)
        base = os.path.join(self.out_dir, name)
        with open(base + "_pressure.csv", "w") as f:
            f.write("print_time,raw\n")
            f.writelines("%.6f,%d\n" % (t, v) for t, v in self.samples)
        with open(base + "_extruder.csv", "w") as f:
            f.write("print_time,move_t,start_v,accel,start_e\n")
            for t in sorted(self.moves):
                f.write("%.6f,%.6f,%.6f,%.6f,%.6f\n" % ((t,) + self.moves[t]))
        with open(base + "_marks.csv", "w") as f:
            f.write("print_time,label\n")
            f.writelines("%.6f,%s\n" % (t, label) for t, label in self.marks)
        return base

    # --- the routine ------------------------------------------------------

    def _run(self, lines):
        if lines:
            self.gcode.run_script_from_command("\n".join(lines))

    def _fit(self, name):
        proc = subprocess.Popen([sys.executable, FIT, self.out_dir, name],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        while proc.poll() is None:
            self.reactor.pause(self.reactor.monotonic() + 0.2)
        out, err = proc.communicate()
        if proc.returncode != 0:
            return {"fitted": None, "reliable": False,
                    "problems": ["fit failed: %s" % err.decode(errors="replace")[-200:]]}
        return json.loads(out.decode())

    def cmd_HELIX_PA_CALIBRATE(self, gcmd):
        self._require(gcmd)
        if self.running:
            raise gcmd.error("HELIX_PA: a calibration is already running")
        now = self.reactor.monotonic()

        def status_of(name):
            obj = self.printer.lookup_object(name, None)
            return obj.get_status(now) if obj is not None and hasattr(obj, "get_status") else None
        blocked = preflight(self.profile, status_of)
        if blocked:
            raise gcmd.error("HELIX_PA: no result: %s" % blocked)
        heaters = self.printer.lookup_object("heaters")
        extruder = self.printer.lookup_object("toolhead").get_extruder()
        target = extruder.get_heater().get_status(self.reactor.monotonic())["target"]
        temp = gcmd.get_float("TEMP", target if target >= 170. else 220.,
                              minval=170., maxval=320.)
        mode = gcmd.get("MODE", "full").lower()
        filament = gcmd.get("FILAMENT", "")
        print_start = gcmd.get_int("PRINT_START", 0)
        previous = extruder.get_status(self.reactor.monotonic()).get("pressure_advance", 0.)

        if mode == "auto" and filament:
            cached = self.cache.get(filament, temp)
            if cached is not None:
                self._run(["SET_PRESSURE_ADVANCE ADVANCE=%.4f" % cached["k"]])
                gcmd.respond_info("HELIX_PA: measured K=%.4f (saved for %s)"
                                  % (cached["k"], filament))
                return
        center = gcmd.get_float("CENTER", previous or 0.04, minval=0.)
        self.ks = FULL_KS if mode == "full" else quick_ks(center)
        self.running = True
        result = {"fitted": None, "reliable": False, "problems": ["did not run"]}
        try:
            toolhead = self.printer.lookup_object("toolhead")
            homed = toolhead.get_status(self.reactor.monotonic())["homed_axes"]
            prepare = [] if "xyz" in homed else ["G28"]
            prepare += self.profile["goto"] + ["M109 S%d" % temp]
            prepare += self.profile["feed_start"] + warmup_gcode(self.profile)
            self._run(prepare)
            self._run(sweep_gcode(self.profile, self.ks))
            result = self._fit("helix_pa")
        finally:
            self.running = False
            self._run(self.profile["feed_stop"])
            self._run(["SET_PRESSURE_ADVANCE ADVANCE=%.4f" % previous])
        if result.get("reliable"):
            k = result["fitted"]
            self._run(["SET_PRESSURE_ADVANCE ADVANCE=%.4f" % k])
            if filament:
                self.cache.put(filament, temp, k)
            gcmd.respond_info("HELIX_PA: measured K=%.4f (signal/noise %.0f, %d steps)"
                              % (k, result.get("snr", 0), result.get("steps", 0)))
        else:
            gcmd.respond_info("HELIX_PA: no result: %s"
                              % "; ".join(result.get("problems") or ["unknown"]))
        self.last = {"k": result.get("fitted"), "reliable": bool(result.get("reliable")),
                     "problems": result.get("problems", []), "snr": result.get("snr"),
                     "filament": filament, "temp": temp, "stamp": time.time()}
        finish = list(self.profile["wipe"])
        if not print_start:
            finish += ["M104 S0"] + self.profile["park"]
        self._run(finish)


def load_config(config):
    return HelixPaGauge(config)
