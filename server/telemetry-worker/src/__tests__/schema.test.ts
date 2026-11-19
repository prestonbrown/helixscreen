// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, it, expect } from "vitest";
import { mapEventToDataPoints } from "../analytics";
import { snapshotToDataPoints } from "../cdn_fleet";
import {
  EVENT_FANOUT,
  MAX_SLOT,
  POINT_KINDS,
  R2_ONLY,
  col,
  hasCol,
} from "../schema";

describe("POINT_KINDS table", () => {
  it("never reuses a slot within a kind and stays inside the 20-slot limit", () => {
    for (const [name, pk] of Object.entries(POINT_KINDS)) {
      expect(pk.kind).toBe(name);
      const seen = new Set<string>();
      const fields = new Set<string>();
      for (const c of pk.columns) {
        const key = `${c.type}${c.slot}`;
        expect(seen.has(key), `${name}: ${key} used twice`).toBe(false);
        seen.add(key);
        expect(fields.has(c.field), `${name}: field ${c.field} declared twice`).toBe(false);
        fields.add(c.field);
        expect(c.slot).toBeGreaterThanOrEqual(1);
        expect(c.slot).toBeLessThanOrEqual(MAX_SLOT);
      }
    }
  });

  it("only fans out to kinds that exist", () => {
    for (const [eventType, kinds] of Object.entries(EVENT_FANOUT)) {
      expect(kinds.length, eventType).toBeGreaterThan(0);
      for (const k of kinds) expect(POINT_KINDS[k], `${eventType} -> ${k}`).toBeDefined();
    }
  });

  it("only lists R2-only fields for event types it knows", () => {
    for (const eventType of Object.keys(R2_ONLY)) {
      expect(EVENT_FANOUT[eventType], eventType).toBeDefined();
    }
  });

  // Rows already stored are read by position. A diff here means a column moved
  // or appeared: accept it only for a new field in a free slot (blobs 13-20,
  // doubles 9-20).
  it("keeps every column in its slot", () => {
    const slots: Record<string, Record<string, string>> = {};
    for (const [name, pk] of Object.entries(POINT_KINDS)) {
      slots[name] = {};
      for (const c of pk.columns) slots[name][c.field] = col(name, c.field);
    }
    expect(slots).toMatchInlineSnapshot(`
      {
        "async_lifetime_skips": {
          "app_platform": "blob3",
          "app_version": "blob2",
          "device_id": "blob1",
          "is_shutdown": "double4",
          "other_count": "double3",
          "total_skips": "double2",
          "uptime_sec": "double1",
        },
        "cdn_fleet_daily": {
          "channel": "blob2",
          "countries": "double3",
          "date": "blob1",
          "errors": "double4",
          "polls": "double1",
          "sources": "double2",
          "truncated": "blob3",
        },
        "connection_stability": {
          "app_board": "blob14",
          "app_platform": "blob3",
          "app_version": "blob2",
          "boot_id": "blob13",
          "connect_count": "double2",
          "device_id": "blob1",
          "disconnect_count": "double3",
          "klippy_error_count": "double7",
          "klippy_shutdown_count": "double8",
          "longest_disconnect_sec": "double6",
          "schema_version": "double9",
          "session_duration_sec": "double1",
          "snapshot_seq": "double10",
          "total_connected_sec": "double4",
          "total_disconnected_sec": "double5",
        },
        "crash": {
          "app_board": "blob8",
          "app_platform": "blob4",
          "app_version": "blob2",
          "backtrace_depth": "double3",
          "device_id": "blob1",
          "fault_addr": "blob7",
          "fault_code_name": "blob6",
          "load_base": "blob5",
          "signal": "double2",
          "signal_name": "blob3",
          "uptime_sec": "double1",
        },
        "error_encountered": {
          "app_board": "blob13",
          "app_platform": "blob3",
          "app_version": "blob2",
          "category": "blob4",
          "code": "blob5",
          "context": "blob6",
          "device_id": "blob1",
          "uptime_sec": "double1",
        },
        "feature_adoption": {
          "app_platform": "blob3",
          "app_version": "blob2",
          "bed_mesh": "double3",
          "camera": "double2",
          "console_gcode": "double4",
          "device_id": "blob1",
          "feature_bitmask": "double8",
          "filament_management": "double6",
          "input_shaper": "double5",
          "macros": "double1",
          "manual_probe": "double7",
        },
        "hardware_profile": {
          "ams_type": "blob8",
          "app_board": "blob13",
          "app_platform": "blob3",
          "app_version": "blob2",
          "build_vol_x": "double5",
          "build_vol_y": "double6",
          "build_vol_z": "double7",
          "capabilities_bitmask": "double8",
          "device_id": "blob1",
          "extruder_count": "double1",
          "fan_count": "double2",
          "has_helix_macros": "blob9",
          "kinematics": "blob5",
          "macro_count": "double4",
          "mcu_chip": "blob6",
          "moonraker_is_local": "blob10",
          "printer_model": "blob4",
          "probe_type": "blob7",
          "sensor_count": "double3",
        },
        "memory_snapshot": {
          "app_platform": "blob3",
          "app_version": "blob2",
          "device_id": "blob1",
          "rss_kb": "double2",
          "trigger": "blob4",
          "uptime_sec": "double1",
          "vm_hwm_kb": "double5",
          "vm_peak_kb": "double4",
          "vm_size_kb": "double3",
        },
        "memory_warning": {
          "app_platform": "blob3",
          "app_version": "blob2",
          "device_id": "blob1",
          "growth_5min_kb": "double5",
          "level": "blob4",
          "private_dirty_kb": "double6",
          "pss_kb": "double7",
          "reason": "blob5",
          "rss_kb": "double2",
          "system_available_mb": "double4",
          "system_total_mb": "double8",
          "uptime_sec": "double1",
          "vm_size_kb": "double3",
        },
        "overlay_visit": {
          "app_board": "blob14",
          "app_platform": "blob3",
          "app_version": "blob2",
          "boot_id": "blob13",
          "count": "double2",
          "device_id": "blob1",
          "overlay_name": "blob4",
          "schema_version": "double9",
          "session_duration_sec": "double1",
          "snapshot_seq": "double10",
        },
        "panel_usage": {
          "app_board": "blob14",
          "app_platform": "blob3",
          "app_version": "blob2",
          "boot_id": "blob13",
          "device_id": "blob1",
          "overlay_open_count": "double4",
          "panel_name": "blob4",
          "schema_version": "double9",
          "session_duration_sec": "double1",
          "snapshot_seq": "double10",
          "time_sec": "double2",
          "visits": "double3",
        },
        "performance_snapshot": {
          "app_platform": "blob3",
          "app_version": "blob2",
          "device_id": "blob1",
          "dropped_frame_count": "double5",
          "frame_time_p50_ms": "double2",
          "frame_time_p95_ms": "double3",
          "frame_time_p99_ms": "double4",
          "snapshot_seq": "double1",
          "task_handler_max_ms": "double8",
          "total_frame_count": "double6",
          "worst_panel": "blob4",
          "worst_panel_p95_ms": "double7",
        },
        "print_outcome": {
          "app_board": "blob14",
          "app_platform": "blob13",
          "app_version": "blob4",
          "bed_temp": "double3",
          "device_id": "blob1",
          "duration_sec": "double1",
          "filament_type": "blob3",
          "filament_used_mm": "double4",
          "nozzle_temp": "double2",
          "outcome": "blob2",
          "phases_completed": "double5",
        },
        "print_start_context": {
          "ams_active": "double3",
          "app_board": "blob13",
          "app_platform": "blob3",
          "app_version": "blob2",
          "device_id": "blob1",
          "estimated_duration_bucket": "blob6",
          "file_size_bucket": "blob5",
          "has_thumbnail": "double2",
          "slicer": "blob4",
          "source": "blob7",
          "tool_count_used": "double1",
        },
        "screensaver_level": {
          "app_board": "blob14",
          "app_platform": "blob3",
          "app_version": "blob2",
          "boot_id": "blob13",
          "device_id": "blob1",
          "level": "double1",
          "runs": "double2",
          "saver_name": "blob4",
          "schema_version": "double9",
          "snapshot_seq": "double10",
        },
        "screensaver_usage": {
          "app_board": "blob14",
          "app_platform": "blob3",
          "app_version": "blob2",
          "boot_id": "blob13",
          "device_id": "blob1",
          "saver_name": "blob4",
          "schema_version": "double9",
          "shown_sec": "double1",
          "snapshot_seq": "double10",
          "starts": "double2",
          "stopped_other": "double4",
          "too_heavy": "double5",
          "woke_by_touch": "double3",
        },
        "session": {
          "app_board": "blob11",
          "app_platform": "blob3",
          "app_version": "blob2",
          "arch": "blob9",
          "cpu_cores": "double2",
          "device_id": "blob1",
          "display": "blob6",
          "extruder_count": "double3",
          "host_mod": "blob14",
          "install_source": "blob13",
          "kinematics": "blob5",
          "locale": "blob7",
          "printer_model": "blob4",
          "ram_total_mb": "double1",
          "theme": "blob8",
          "zip_tool": "blob10",
        },
        "settings_change": {
          "app_platform": "blob3",
          "app_version": "blob2",
          "change_count": "double1",
          "device_id": "blob1",
          "new_value": "blob6",
          "old_value": "blob5",
          "setting": "blob4",
        },
        "settings_snapshot": {
          "animations_enabled": "double5",
          "app_platform": "blob3",
          "app_version": "blob2",
          "auto_update_channel": "blob6",
          "brightness_pct": "double1",
          "device_id": "blob1",
          "locale": "blob5",
          "multi_printer": "blob15",
          "screen_blank_timeout_sec": "double3",
          "screensaver_timeout_sec": "double2",
          "screensaver_type": "blob16",
          "sound_enabled": "double4",
          "spoolman_configured": "blob13",
          "theme": "blob8",
          "theme_changed": "blob14",
          "theme_name": "blob4",
          "time_format": "blob7",
        },
        "update_failed": {
          "app_platform": "blob3",
          "device_id": "blob1",
          "exit_code": "double3",
          "file_size": "double2",
          "from_version": "blob2",
          "host": "blob7",
          "http_code": "double1",
          "install_detail": "blob8",
          "reason": "blob4",
          "transfer_code": "double4",
          "transfer_error": "blob6",
          "version": "blob5",
        },
        "update_success": {
          "app_platform": "blob3",
          "device_id": "blob1",
          "from_version": "blob4",
          "version": "blob2",
        },
        "widget_interaction": {
          "app_board": "blob14",
          "app_platform": "blob3",
          "app_version": "blob2",
          "boot_id": "blob13",
          "count": "double2",
          "device_id": "blob1",
          "schema_version": "double9",
          "session_duration_sec": "double1",
          "snapshot_seq": "double10",
          "widget_id": "blob4",
        },
        "widget_placement": {
          "app_platform": "blob3",
          "app_version": "blob2",
          "count": "double1",
          "device_id": "blob1",
          "widget_id": "blob4",
        },
      }
    `);
  });
});

describe("cdn_fleet_daily", () => {
  it("is declared where the cron writes it", () => {
    const [dp] = snapshotToDataPoints({
      date: "2026-10-13",
      channels: [{ channel: "stable", polls: 11, sources: 7, countries: 3, errors: 2, truncated: true }],
    });
    expect(dp.indexes).toEqual(["cdn_fleet_daily"]);
    const at = (field: string): unknown => {
      const c = col("cdn_fleet_daily", field);
      const slot = Number(c.replace(/^\D+/, "")) - 1;
      return c.startsWith("blob") ? dp.blobs[slot] : dp.doubles[slot];
    };
    expect([at("date"), at("channel"), at("truncated")]).toEqual(["2026-10-13", "stable", "truncated"]);
    expect([at("polls"), at("sources"), at("countries"), at("errors")]).toEqual([11, 7, 3, 2]);
  });
});

describe("col / hasCol", () => {
  it("resolves a field to its Analytics Engine column", () => {
    expect(col("print_outcome", "outcome")).toBe("blob2");
    expect(col("crash", "uptime_sec")).toBe("double1");
    expect(hasCol("crash", "uptime_sec")).toBe(true);
  });

  it("throws on an unknown kind or field and reports false from hasCol", () => {
    expect(() => col("nope", "x")).toThrow(/unknown point kind/);
    expect(() => col("crash", "nope")).toThrow(/no column nope/);
    expect(() => col("constructor", "x")).toThrow(/unknown point kind/);
    expect(hasCol("nope", "x")).toBe(false);
    expect(hasCol("crash", "nope")).toBe(false);
    expect(hasCol("constructor", "x")).toBe(false);
  });
});

describe("table-driven writer", () => {
  it("writes an update_failed target from version, not app_version", () => {
    const [dp] = mapEventToDataPoints({
      event: "update_failed",
      device_id: "d",
      version: "1.0.5",
      app_version: "1.0.4",
      from_version: "1.0.3",
      reason: "corrupt_download",
    });
    expect(dp.blobs![Number(col("update_failed", "version").slice(4)) - 1]).toBe("1.0.5");
    expect(dp.blobs![Number(col("update_failed", "from_version").slice(4)) - 1]).toBe("1.0.3");
    expect(dp.blobs).not.toContain("1.0.4");
  });

  it("leaves an update_failed target empty rather than borrowing app_version", () => {
    const [dp] = mapEventToDataPoints({
      event: "update_failed",
      device_id: "d",
      app_version: "1.0.4",
      from_version: "1.0.3",
    });
    expect(dp.blobs![4]).toBe("");
    expect(dp.blobs).not.toContain("1.0.4");
  });

  it("writes an update_success version from version, not app_version", () => {
    const [dp] = mapEventToDataPoints({
      event: "update_success",
      device_id: "d",
      version: "1.0.5",
      app_version: "1.0.4",
      from_version: "1.0.3",
    });
    expect(dp.blobs![1]).toBe("1.0.5");
    expect(dp.blobs![3]).toBe("1.0.3");
    expect(dp.blobs).not.toContain("1.0.4");
  });

  it("writes a print_outcome's app_version, platform and board to the table's columns", () => {
    const [dp] = mapEventToDataPoints({
      event: "print_outcome",
      device_id: "d",
      outcome: "success",
      app_version: "1.1.0",
      app_platform: "mips",
      app_board: "creator5-pro",
    });
    const at = (f: string) => dp.blobs![Number(col("print_outcome", f).slice(4)) - 1];
    expect(at("app_version")).toBe("1.1.0");
    expect(at("app_platform")).toBe("mips");
    expect(at("app_board")).toBe("creator5-pro");
    expect(col("print_outcome", "app_version")).toBe("blob4");
    expect(col("print_outcome", "app_platform")).toBe("blob13");
    expect(col("print_outcome", "app_board")).toBe("blob14");
  });

  it.each(["print_start_context", "error_encountered", "hardware_profile", "connection_stability"])(
    "writes %s's app_board to a column past the established twelve",
    (eventType) => {
      const [dp] = mapEventToDataPoints({
        event: eventType,
        device_id: "d",
        app_version: "1.1.0",
        app_platform: "mips",
        app_board: "k1",
      });
      const slot = (f: string) => Number(col(eventType, f).slice(4));
      expect(slot("app_board")).toBeGreaterThan(12);
      expect(dp.blobs![slot("app_board") - 1]).toBe("k1");
      expect(dp.blobs![slot("app_version") - 1]).toBe("1.1.0");
      expect(dp.blobs![slot("app_platform") - 1]).toBe("mips");
    },
  );

  it("stamps the delta envelope on every point a panel_usage event fans out to", () => {
    const points = mapEventToDataPoints({
      event: "panel_usage",
      device_id: "dev",
      schema_version: 3,
      boot_id: "boot-1",
      snapshot_seq: 7,
      app_version: "1.1.0",
      session_duration_sec: 600,
      panel_time_sec: { home: 30 },
      panel_visits: { home: 2 },
      widget_interactions: { temp: 4 },
      overlay_visits: { settings: 3 },
    });
    expect(points.map((p) => p.indexes![0])).toEqual([
      "panel_usage",
      "widget_interaction",
      "overlay_visit",
    ]);
    for (const p of points) {
      const kind = p.indexes![0];
      expect(p.blobs![Number(col(kind, "boot_id").slice(4)) - 1]).toBe("boot-1");
      expect(p.doubles![Number(col(kind, "schema_version").slice(6)) - 1]).toBe(3);
      expect(p.doubles![Number(col(kind, "snapshot_seq").slice(6)) - 1]).toBe(7);
    }
    expect(points[0].blobs![3]).toBe("home");
    expect(points[0].doubles!.slice(0, 4)).toEqual([600, 30, 2, 0]);
    expect(points[1].blobs![3]).toBe("temp");
    expect(points[1].doubles!.slice(0, 2)).toEqual([600, 4]);
    expect(points[2].blobs![3]).toBe("settings");
    expect(points[2].doubles!.slice(0, 2)).toEqual([600, 3]);
  });

  it("fans a screensaver_usage event out to one point per saver and one per settled level", () => {
    const points = mapEventToDataPoints({
      event: "screensaver_usage",
      device_id: "dev",
      schema_version: 3,
      boot_id: "boot-3",
      snapshot_seq: 2,
      app_version: "1.1.0",
      saver_shown_sec: { maze: 40, pipes: 90 },
      saver_starts: { maze: 1, pipes: 2 },
      saver_woke_by_touch: { pipes: 1 },
      saver_stopped_other: { pipes: 1 },
      saver_too_heavy: { maze: 1 },
      saver_settled_levels: { "maze:3": 1, "pipes:0": 1, "pipes:1": 1 },
    });
    expect(points.map((p) => p.indexes![0])).toEqual([
      "screensaver_usage",
      "screensaver_usage",
      "screensaver_level",
      "screensaver_level",
      "screensaver_level",
    ]);
    for (const p of points) {
      const kind = p.indexes![0];
      expect(p.blobs![Number(col(kind, "boot_id").slice(4)) - 1]).toBe("boot-3");
      expect(p.doubles![Number(col(kind, "schema_version").slice(6)) - 1]).toBe(3);
      expect(p.doubles![Number(col(kind, "snapshot_seq").slice(6)) - 1]).toBe(2);
    }
    const usage = (kind: string, p: (typeof points)[number], f: string) =>
      p.doubles![Number(col(kind, f).slice(6)) - 1];
    const saver = (p: (typeof points)[number]) => p.blobs![Number(col("screensaver_usage", "saver_name").slice(4)) - 1];

    const [maze, pipes] = points;
    expect(saver(maze)).toBe("maze");
    expect(["shown_sec", "starts", "woke_by_touch", "stopped_other", "too_heavy"].map((f) => usage("screensaver_usage", maze, f))).toEqual([40, 1, 0, 0, 1]);
    expect(saver(pipes)).toBe("pipes");
    expect(["shown_sec", "starts", "woke_by_touch", "stopped_other", "too_heavy"].map((f) => usage("screensaver_usage", pipes, f))).toEqual([90, 2, 1, 1, 0]);

    const levels = points.slice(2).map((p) => [
      p.blobs![Number(col("screensaver_level", "saver_name").slice(4)) - 1],
      usage("screensaver_level", p, "level"),
      usage("screensaver_level", p, "runs"),
    ]);
    expect(levels).toEqual([
      ["maze", 3, 1],
      ["pipes", 0, 1],
      ["pipes", 1, 1],
    ]);
  });

  it("writes no screensaver points for an event with empty saver maps", () => {
    expect(mapEventToDataPoints({ event: "screensaver_usage", device_id: "dev" })).toEqual([]);
  });

  it("keeps install provenance to its allowlists and leaves older clients empty", () => {
    const blobFor = (field: string, app: Record<string, unknown>) => {
      const [dp] = mapEventToDataPoints({ event: "session", device_id: "d", app });
      return dp.blobs![Number(col("session", field).slice(4)) - 1];
    };
    expect(blobFor("install_source", { install_source: "kiauh" })).toBe("kiauh");
    expect(blobFor("install_source", { install_source: "unknown" })).toBe("unknown");
    expect(blobFor("install_source", { install_source: "/home/alice" })).toBe("unknown");
    expect(blobFor("install_source", {})).toBe("");
    expect(blobFor("host_mod", { host_mod: "forge_x" })).toBe("forge_x");
    expect(blobFor("host_mod", { host_mod: "my printer" })).toBe("unknown");
    expect(blobFor("host_mod", {})).toBe("");
  });

  it("reads settings_snapshot's screensaver_type into its own blob", () => {
    const [dp] = mapEventToDataPoints({ event: "settings_snapshot", device_id: "d", screensaver_type: "maze" });
    expect(dp.blobs![Number(col("settings_snapshot", "screensaver_type").slice(4)) - 1]).toBe("maze");
  });

  it("stamps the delta envelope on connection_stability", () => {
    const [dp] = mapEventToDataPoints({
      event: "connection_stability",
      device_id: "dev",
      schema_version: 3,
      boot_id: "boot-2",
      snapshot_seq: 4,
      total_connected_sec: 100,
    });
    expect(dp.blobs![Number(col("connection_stability", "boot_id").slice(4)) - 1]).toBe("boot-2");
    expect(dp.doubles![Number(col("connection_stability", "snapshot_seq").slice(6)) - 1]).toBe(4);
    expect(dp.doubles![Number(col("connection_stability", "schema_version").slice(6)) - 1]).toBe(3);
    expect(dp.doubles![3]).toBe(100);
  });

  it("writes the settings adoption flags as 1, 0, or empty for a client that sends none", () => {
    const flags = ["spoolman_configured", "theme_changed", "multi_printer"];
    const at = (dp: { blobs?: string[] }, f: string) =>
      dp.blobs![Number(col("settings_snapshot", f).slice(4)) - 1];
    const [sent] = mapEventToDataPoints({
      event: "settings_snapshot",
      device_id: "d",
      spoolman_configured: true,
      theme_changed: false,
      multi_printer: true,
    });
    expect(flags.map((f) => at(sent, f))).toEqual(["1", "0", "1"]);
    const [old] = mapEventToDataPoints({ event: "settings_snapshot", device_id: "d" });
    expect(flags.map((f) => at(old, f))).toEqual(["", "", ""]);
  });

  it("writes widget placement and settings changes one point per element", () => {
    const placements = mapEventToDataPoints({
      event: "settings_snapshot",
      device_id: "d",
      home_widgets: ["temp", "fan", "temp"],
    });
    expect(placements.map((p) => p.indexes![0])).toEqual([
      "settings_snapshot",
      "widget_placement",
      "widget_placement",
    ]);
    expect(placements[1].blobs![3]).toBe("temp");
    expect(placements[1].doubles![0]).toBe(2);

    const changes = mapEventToDataPoints({
      event: "settings_changes",
      device_id: "d",
      changes: [{ setting: "brightness", old_value: "50", new_value: "80" }],
    });
    expect(changes).toHaveLength(1);
    expect(changes[0].blobs!.slice(3, 6)).toEqual(["brightness", "50", "80"]);
    expect(changes[0].doubles![0]).toBe(1);

    const none = mapEventToDataPoints({ event: "settings_changes", device_id: "d" });
    expect(none).toHaveLength(1);
    expect(none[0].doubles![0]).toBe(0);
  });

  it("writes an async_lifetime_skips window as its own kind and keeps the per-tag list out", () => {
    const [dp] = mapEventToDataPoints({
      event: "async_lifetime_skips",
      device_id: "d",
      app_version: "1.1.0",
      app_platform: "pi",
      uptime_sec: 90,
      total_skips: 5,
      other_count: 1,
      is_shutdown: true,
      tags: [{ tag: "panel", count: 4 }],
    });
    expect(dp.indexes).toEqual(["async_lifetime_skips"]);
    expect(dp.blobs![1]).toBe("1.1.0");
    expect(dp.doubles!.slice(0, 4)).toEqual([90, 5, 1, 1]);
    expect(R2_ONLY.async_lifetime_skips).toContain("tags");
  });

  it("falls back to the discoverability point for unknown and inherited event names", () => {
    for (const name of ["brand_new_event", "constructor", "toString"]) {
      const [dp] = mapEventToDataPoints({ event: name, device_id: "d" });
      expect(dp.indexes).toEqual([name]);
      expect(dp.blobs![1]).toBe(name);
    }
  });

  it("pads missing values to empty strings and zeros", () => {
    const [dp] = mapEventToDataPoints({ event: "crash" });
    expect(dp.blobs!.every((b) => b === "")).toBe(true);
    expect(dp.doubles!.every((d) => d === 0)).toBe(true);
  });
});
