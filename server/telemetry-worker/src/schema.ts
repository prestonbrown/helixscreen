// SPDX-License-Identifier: GPL-3.0-or-later
// One declaration per Analytics Engine point kind (the `index1` value). The
// writer in analytics.ts fills blob and double arrays from these tables, and
// queries ask for columns through col(kind, field) instead of literal blobN.
//
// Slots are positions in stored rows, and rows are read by position for 90
// days. A column never moves; new fields take the next free slot (blobs 13-20,
// doubles 9-20). A retired column leaves its slot empty.

export type FieldType = "blob" | "double";

export type EventRecord = Record<string, unknown>;

type ReadFn = (event: EventRecord, item?: EventRecord) => unknown;

/**
 * Pulls one column's raw value out of an event. `item` is the element a
 * fan-out kind iterates over (a panel, a widget, one settings change).
 * `paths` are the client fields it reads, in ColumnDef.paths notation, and
 * `fallback` says they are tried in order (see ColumnDef.fallback).
 */
export type Reader = ReadFn & { readonly paths: readonly string[]; readonly fallback: boolean };

export interface ColumnDef {
  /** Stable logical name used by col(); the client's JSON key where one exists. */
  field: string;
  type: FieldType;
  /** 1-based position in the blobs or doubles array. */
  slot: number;
  /** Accessor for columns that are not a plain `event[field]`. */
  read?: Reader;
  /**
   * The client JSON fields this column is written from, as dotted paths: `*`
   * stands for every key of a map (panel names, overlay ids) and `[]` for
   * every element of an array. A reader declares its own; a plain column
   * reads `[field]`. The contract test holds the client's event fixtures to
   * these and to R2_ONLY.
   */
  paths: readonly string[];
  /**
   * True when `paths` is a precedence list: a later path is read only while
   * every earlier one is absent, so an event carrying an earlier path does not
   * have the later ones written. False when every path feeds the value.
   */
  fallback: boolean;
}

export interface PointKind {
  /** The `index1` value. */
  kind: string;
  columns: ColumnDef[];
}

export const MAX_SLOT = 20;

// Mirrors src/system/install_provenance.cpp; "unknown" is what the client sends
// for a missing marker.
const INSTALL_SOURCES = ["helix-installer", "kiauh", "helper-script", "curl", "other", "unknown"];
const HOST_MODS = ["forge_x", "zmod", "klipper_mod", "cosmos", "paxx", "simple_af", "rinkhals", "none"];

// ---------- Readers ----------

const reads = (paths: readonly string[], fn: ReadFn, fallback = false): Reader =>
  Object.assign(fn, { paths, fallback });

const section = (v: unknown): EventRecord => (v ?? {}) as EventRecord;

/** `event[sec][key]`, then each flat key in order; null and undefined fall through. */
const nested = (sec: string, key: string, ...flatKeys: string[]): Reader =>
  reads(
    [`${sec}.${key}`, ...flatKeys],
    (e) => {
      const v = section(e[sec])[key];
      if (v !== undefined && v !== null) return v;
      for (const k of flatKeys) {
        if (e[k] !== undefined && e[k] !== null) return e[k];
      }
      return undefined;
    },
    true,
  );

/** A value from a fixed list, "unknown" for anything else a client sends, undefined when absent. */
const allowlisted = (read: Reader, allowed: readonly string[]): Reader =>
  reads(
    read.paths,
    (e, item) => {
      const v = read(e, item);
      if (v === undefined || v === null || v === "") return undefined;
      return allowed.includes(String(v)) ? v : "unknown";
    },
    read.fallback,
  );

const firstOf = (...readers: Reader[]): Reader =>
  reads(
    readers.flatMap((r) => r.paths),
    (e, item) => {
      for (const r of readers) {
        const v = r(e, item);
        if (v !== undefined && v !== null) return v;
      }
      return undefined;
    },
    readers.every((r) => r.fallback),
  );

const flat = (key: string): Reader => reads([key], (e) => e[key], true);

/** `item[key]`, where the fan-out built the item from the client fields `paths`. */
const fromItem = (key: string, ...paths: string[]): Reader =>
  reads(paths, (_e, item) => item?.[key]);

const appVersion = nested("app", "version", "app_version", "version");
const appPlatform = nested("app", "platform", "app_platform", "platform");
const appBoard = nested("app", "board", "app_board", "board");

// Update events carry two versions, so neither reads the envelope's app_version.
const targetVersion = flat("version");
const fromVersion = flat("from_version");

const themeNameSource = firstOf(flat("theme_name"), nested("app", "theme", "theme"));
const themeName: Reader = reads(themeNameSource.paths, (e) => themeNameSource(e) ?? "", true);

const darkLightTheme: Reader = reads(
  ["theme", ...themeName.paths],
  (e) => {
    if (e.theme !== undefined && e.theme !== null) return e.theme;
    const name = String(themeName(e));
    return name.includes("(Dark)") ? "dark" : name.includes("(Light)") ? "light" : "";
  },
  true,
);

/** "1" / "0" / "": a boolean that is "" for clients too old to report it. */
const triState = (read: Reader): Reader =>
  reads(
    read.paths,
    (e, item) => {
      const v = read(e, item);
      return v === undefined || v === null ? "" : v ? "1" : "0";
    },
    read.fallback,
  );

const SENSOR_COUNT_PATHS = [
  "sensors.filament",
  "sensors.temperature_extra",
  "sensors.color",
  "sensors.accel",
  "sensors.count",
  "sensor_count",
];

const hardwareSensorCount: Reader = reads(SENSOR_COUNT_PATHS, (e) => {
  const sensors = section(e.sensors);
  const sum =
    Number(sensors.filament ?? 0) +
    Number(sensors.temperature_extra ?? 0) +
    Number(sensors.color ?? 0) +
    Number(sensors.accel ?? 0);
  return sum || Number(sensors.count ?? e.sensor_count ?? 0);
});

// Bit order: 0=chamber, 1=accelerometer, 2=firmware_retraction,
// 3=exclude_object, 4=timelapse, 5=klippain_shaketune, 6=speaker, 7=probe,
// 8=led, 9=filament_sensor, 10=multi_extruder, 11=ams, 12=heater_bed.
const CAPABILITY_BITS = [
  "has_chamber",
  "has_accelerometer",
  "has_firmware_retraction",
  "has_exclude_object",
  "has_timelapse",
  "has_klippain_shaketune",
  "has_speaker",
];

const CAPABILITY_PATHS = [
  ...CAPABILITY_BITS.map((k) => `capabilities.${k}`),
  "capabilities_bitmask",
  "probe.has_probe",
  "leds.count",
  "sensors.filament",
  "tools.is_multi_tool",
  "extruders.count",
  "ams",
  "extruders.has_heater_bed",
];

const hardwareCapabilitiesBitmask: Reader = reads(CAPABILITY_PATHS, (e) => {
  const caps = section(e.capabilities);
  let mask = Number(e.capabilities_bitmask ?? 0);
  if (typeof caps === "object" && caps !== null && Object.keys(caps).length > 0) {
    const probe = section(e.probe);
    const leds = section(e.leds);
    const sensors = section(e.sensors);
    const tools = section(e.tools);
    const extruders = section(e.extruders);
    mask = 0;
    for (let i = 0; i < CAPABILITY_BITS.length; i++) {
      if (caps[CAPABILITY_BITS[i]]) mask |= 1 << i;
    }
    if (probe.has_probe) mask |= 1 << 7;
    if (Number(leds.count ?? 0) > 0) mask |= 1 << 8;
    if (Number(sensors.filament ?? 0) > 0) mask |= 1 << 9;
    if (tools.is_multi_tool || Number(extruders.count ?? 0) > 1) mask |= 1 << 10;
    if (e.ams) mask |= 1 << 11;
    if (extruders.has_heater_bed) mask |= 1 << 12;
  }
  return mask;
});

// The first seven features get their own double; the rest share a bitmask.
const FEATURE_DOUBLES = [
  "macros",
  "camera",
  "bed_mesh",
  "console_gcode",
  "input_shaper",
  "filament_management",
  "manual_probe",
];

const FEATURE_BITMASK_ORDER = [
  "spoolman",
  "led_control",
  "power_devices",
  "multi_printer",
  "theme_changed",
  "timelapse",
  "favorites",
  "pid_calibration",
  "firmware_retraction",
];

const featureFlag = (name: string): Reader =>
  reads([`features.${name}`], (e) => (section(e.features)[name] ? 1 : 0));

const featureBitmask: Reader = reads(
  FEATURE_BITMASK_ORDER.map((name) => `features.${name}`),
  (e) => {
    const features = section(e.features);
    let mask = 0;
    for (let i = 0; i < FEATURE_BITMASK_ORDER.length; i++) {
      if (features[FEATURE_BITMASK_ORDER[i]]) mask |= 1 << i;
    }
    return mask;
  },
);

// Stores how many frames the backtrace has; the addresses stay in R2.
const crashBacktraceDepth: Reader = reads(
  ["backtrace_depth", "backtrace[]"],
  (e) => e.backtrace_depth ?? (Array.isArray(e.backtrace) ? e.backtrace.length : 0),
  true,
);

// ---------- Column constructors ----------

const column = (type: FieldType, slot: number, field: string, read?: Reader): ColumnDef =>
  read
    ? { field, type, slot, read, paths: read.paths, fallback: read.fallback }
    : { field, type, slot, paths: [field], fallback: false };

const blob = (slot: number, field: string, read?: Reader): ColumnDef => column("blob", slot, field, read);

const dbl = (slot: number, field: string, read?: Reader): ColumnDef => column("double", slot, field, read);

const device = blob(1, "device_id");
const versionPlatform = [
  blob(2, "app_version", appVersion),
  blob(3, "app_platform", appPlatform),
];

/** Idempotency key parts for delta point kinds: (device_id, boot_id, snapshot_seq). */
const deltaColumns = [
  blob(13, "boot_id"),
  blob(14, "app_board", appBoard),
  dbl(9, "schema_version"),
  dbl(10, "snapshot_seq"),
];

const kind = (name: string, columns: ColumnDef[]): PointKind => ({ kind: name, columns });

/** screensaver_usage's per-saver maps, each keyed by the saver's stable name. */
export const SAVER_MAPS = [
  "saver_shown_sec",
  "saver_starts",
  "saver_woke_by_touch",
  "saver_stopped_other",
  "saver_too_heavy",
] as const;

// ---------- The table ----------

const KINDS: PointKind[] = [
  kind("session", [
    device,
    blob(2, "app_version", appVersion),
    blob(3, "app_platform", appPlatform),
    blob(4, "printer_model", nested("printer", "detected_model", "printer_model")),
    blob(5, "kinematics", nested("printer", "kinematics", "kinematics")),
    blob(6, "display", nested("app", "display", "display")),
    blob(7, "locale", nested("app", "locale", "locale")),
    blob(8, "theme", nested("app", "theme", "theme")),
    blob(9, "arch", nested("host", "arch", "arch")),
    blob(10, "zip_tool", nested("app", "zip_tool", "zip_tool")),
    // Resolved printer board; splits the unified "mips" platform key.
    blob(11, "app_board", appBoard),
    blob(13, "install_source", allowlisted(nested("app", "install_source"), INSTALL_SOURCES)),
    blob(14, "host_mod", allowlisted(nested("app", "host_mod"), HOST_MODS)),
    dbl(1, "ram_total_mb", nested("host", "ram_total_mb", "ram_total_mb")),
    dbl(2, "cpu_cores", nested("host", "cpu_cores", "cpu_cores")),
    dbl(3, "extruder_count", nested("printer", "extruder_count", "extruder_count")),
  ]),

  kind("print_outcome", [
    device,
    blob(2, "outcome"),
    blob(3, "filament_type"),
    blob(4, "app_version", appVersion),
    blob(13, "app_platform", appPlatform),
    blob(14, "app_board", appBoard),
    dbl(1, "duration_sec"),
    dbl(2, "nozzle_temp"),
    dbl(3, "bed_temp"),
    dbl(4, "filament_used_mm"),
    dbl(5, "phases_completed"),
  ]),

  kind("crash", [
    device,
    blob(2, "app_version", appVersion),
    blob(3, "signal_name"),
    blob(4, "app_platform", appPlatform),
    blob(5, "load_base"),
    blob(6, "fault_code_name"),
    blob(7, "fault_addr"),
    blob(8, "app_board", appBoard),
    dbl(1, "uptime_sec"),
    dbl(2, "signal", firstOf(flat("signal"), flat("signal_number"))),
    dbl(3, "backtrace_depth", crashBacktraceDepth),
  ]),

  // blob2 is the version the device was RUNNING, which the version filter and
  // per-version release stats read; the release it tried to move to is blob5.
  kind("update_failed", [
    device,
    blob(2, "from_version", fromVersion),
    blob(3, "app_platform", appPlatform),
    blob(4, "reason"),
    blob(5, "version", targetVersion),
    dbl(1, "http_code"),
    dbl(2, "file_size"),
    dbl(3, "exit_code"),
    // Why a download or install failed (#1813). Older clients send none of
    // these, so they read as empty / 0.
    blob(6, "transfer_error"),
    blob(7, "host"),
    blob(8, "install_detail"),
    dbl(4, "transfer_code"),
  ]),

  // blob2 is the release the device is running now; blob4 is where it came from.
  kind("update_success", [
    device,
    blob(2, "version", targetVersion),
    blob(3, "app_platform", appPlatform),
    blob(4, "from_version", fromVersion),
  ]),

  kind("memory_snapshot", [
    device,
    ...versionPlatform,
    blob(4, "trigger"),
    dbl(1, "uptime_sec"),
    dbl(2, "rss_kb"),
    dbl(3, "vm_size_kb"),
    dbl(4, "vm_peak_kb"),
    dbl(5, "vm_hwm_kb"),
  ]),

  kind("memory_warning", [
    device,
    ...versionPlatform,
    blob(4, "level"),
    blob(5, "reason"),
    dbl(1, "uptime_sec"),
    dbl(2, "rss_kb"),
    dbl(3, "vm_size_kb"),
    dbl(4, "system_available_mb"),
    dbl(5, "growth_5min_kb"),
    dbl(6, "private_dirty_kb"),
    dbl(7, "pss_kb"),
    dbl(8, "system_total_mb"),
  ]),

  kind("hardware_profile", [
    device,
    ...versionPlatform,
    blob(4, "printer_model", nested("printer", "detected_model", "printer_model")),
    blob(5, "kinematics", nested("printer", "kinematics", "kinematics")),
    blob(6, "mcu_chip", nested("mcus", "primary", "mcu_chip")),
    // No current client sends probe.type; kept so stored rows stay readable.
    blob(7, "probe_type", nested("probe", "type", "probe_type")),
    blob(8, "ams_type", nested("ams", "type")),
    // Tri-state strings: a bitmask bit cannot tell false from "client too old".
    blob(9, "has_helix_macros", triState(nested("macros", "has_helix_macros"))),
    blob(10, "moonraker_is_local", triState(flat("moonraker_is_local"))),
    blob(13, "app_board", appBoard),
    dbl(
      1,
      "extruder_count",
      firstOf(
        nested("extruders", "count"),
        nested("printer", "extruder_count", "extruder_count"),
      ),
    ),
    dbl(2, "fan_count", firstOf(nested("fans", "total"), nested("fans", "count", "fan_count"))),
    dbl(3, "sensor_count", hardwareSensorCount),
    dbl(4, "macro_count", firstOf(nested("macros", "total_count"), nested("macros", "count", "macro_count"))),
    dbl(5, "build_vol_x", nested("build_volume", "x_mm", "build_vol_x")),
    dbl(6, "build_vol_y", nested("build_volume", "y_mm", "build_vol_y")),
    dbl(7, "build_vol_z", nested("build_volume", "z_mm", "build_vol_z")),
    dbl(8, "capabilities_bitmask", hardwareCapabilitiesBitmask),
  ]),

  kind("settings_snapshot", [
    device,
    ...versionPlatform,
    blob(4, "theme_name", themeName),
    blob(5, "locale", nested("app", "locale", "locale")),
    blob(6, "auto_update_channel"),
    blob(7, "time_format"),
    blob(8, "theme", darkLightTheme),
    dbl(1, "brightness_pct"),
    dbl(2, "screensaver_timeout_sec"),
    dbl(3, "screen_blank_timeout_sec"),
    dbl(4, "sound_enabled"),
    dbl(5, "animations_enabled"),
    // Adoption flags no overlay or widget visit can show; "" is a client too old to send them.
    blob(13, "spoolman_configured", triState(flat("spoolman_configured"))),
    blob(14, "theme_changed", triState(flat("theme_changed"))),
    blob(15, "multi_printer", triState(flat("multi_printer"))),
    // The configured saver's stable registry name, "off" for none; "" is a client too old to send it.
    blob(16, "screensaver_type"),
  ]),

  // One point per distinct home widget id; `count` is how many times it is placed.
  kind("widget_placement", [
    device,
    ...versionPlatform,
    blob(4, "widget_id", fromItem("widget_id", "home_widgets[]")),
    dbl(1, "count", fromItem("count", "home_widgets[]")),
  ]),

  // One point per panel in the event's panel maps.
  kind("panel_usage", [
    device,
    ...versionPlatform,
    blob(4, "panel_name", fromItem("panel_name", "panel_time_sec.*", "panel_visits.*", "panel_name")),
    ...deltaColumns,
    dbl(1, "session_duration_sec"),
    dbl(2, "time_sec", fromItem("time_sec", "panel_time_sec.*", "time_sec")),
    dbl(3, "visits", fromItem("visits", "panel_visits.*", "visits")),
    dbl(4, "overlay_open_count"),
  ]),

  kind("widget_interaction", [
    device,
    ...versionPlatform,
    blob(4, "widget_id", fromItem("widget_id", "widget_interactions.*")),
    ...deltaColumns,
    dbl(1, "session_duration_sec"),
    dbl(2, "count", fromItem("count", "widget_interactions.*")),
  ]),

  kind("overlay_visit", [
    device,
    ...versionPlatform,
    blob(4, "overlay_name", fromItem("overlay_name", "overlay_visits.*")),
    ...deltaColumns,
    dbl(1, "session_duration_sec"),
    dbl(2, "count", fromItem("count", "overlay_visits.*")),
  ]),

  // One point per saver in the event's saver maps.
  kind("screensaver_usage", [
    device,
    ...versionPlatform,
    blob(4, "saver_name", fromItem("saver_name", ...SAVER_MAPS.map((m) => `${m}.*`))),
    ...deltaColumns,
    dbl(1, "shown_sec", fromItem("shown_sec", "saver_shown_sec.*")),
    dbl(2, "starts", fromItem("starts", "saver_starts.*")),
    dbl(3, "woke_by_touch", fromItem("woke_by_touch", "saver_woke_by_touch.*")),
    dbl(4, "stopped_other", fromItem("stopped_other", "saver_stopped_other.*")),
    dbl(5, "too_heavy", fromItem("too_heavy", "saver_too_heavy.*")),
  ]),

  // One point per (saver, gate level) a run ended at; `runs` is how many did.
  kind("screensaver_level", [
    device,
    ...versionPlatform,
    blob(4, "saver_name", fromItem("saver_name", "saver_settled_levels.*")),
    ...deltaColumns,
    dbl(1, "level", fromItem("level", "saver_settled_levels.*")),
    dbl(2, "runs", fromItem("runs", "saver_settled_levels.*")),
  ]),

  kind("connection_stability", [
    device,
    ...versionPlatform,
    blob(13, "boot_id"),
    blob(14, "app_board", appBoard),
    dbl(1, "session_duration_sec"),
    dbl(2, "connect_count"),
    dbl(3, "disconnect_count"),
    dbl(4, "total_connected_sec"),
    dbl(5, "total_disconnected_sec"),
    dbl(6, "longest_disconnect_sec"),
    dbl(7, "klippy_error_count"),
    dbl(8, "klippy_shutdown_count"),
    dbl(9, "schema_version"),
    dbl(10, "snapshot_seq"),
  ]),

  kind("print_start_context", [
    device,
    ...versionPlatform,
    blob(4, "slicer"),
    blob(5, "file_size_bucket"),
    blob(6, "estimated_duration_bucket"),
    blob(7, "source"),
    blob(13, "app_board", appBoard),
    dbl(1, "tool_count_used"),
    dbl(2, "has_thumbnail"),
    dbl(3, "ams_active"),
  ]),

  kind("error_encountered", [
    device,
    ...versionPlatform,
    blob(4, "category"),
    blob(5, "code"),
    blob(6, "context"),
    blob(13, "app_board", appBoard),
    dbl(1, "uptime_sec"),
  ]),

  kind("performance_snapshot", [
    device,
    ...versionPlatform,
    blob(4, "worst_panel"),
    dbl(1, "snapshot_seq"),
    dbl(2, "frame_time_p50_ms"),
    dbl(3, "frame_time_p95_ms"),
    dbl(4, "frame_time_p99_ms"),
    dbl(5, "dropped_frame_count"),
    dbl(6, "total_frame_count"),
    dbl(7, "worst_panel_p95_ms"),
    // No current client sends it; kept so stored rows stay readable.
    dbl(8, "task_handler_max_ms"),
  ]),

  kind("feature_adoption", [
    device,
    ...versionPlatform,
    ...FEATURE_DOUBLES.map((name, i) => dbl(i + 1, name, featureFlag(name))),
    dbl(8, "feature_bitmask", featureBitmask),
  ]),

  // One point per changed setting; `change_count` is 1 per change and 0 for an
  // event that lists none.
  kind("settings_change", [
    device,
    ...versionPlatform,
    blob(4, "setting", fromItem("setting", "changes[].setting")),
    blob(5, "old_value", fromItem("old_value", "changes[].old_value")),
    blob(6, "new_value", fromItem("new_value", "changes[].new_value")),
    dbl(
      1,
      "change_count",
      reads(["changes[].setting", "changes[].old_value", "changes[].new_value"], (_e, item) => (item ? 1 : 0)),
    ),
  ]),

  // Window totals; the per-tag breakdown is R2-only.
  kind("async_lifetime_skips", [
    device,
    ...versionPlatform,
    dbl(1, "uptime_sec"),
    dbl(2, "total_skips"),
    dbl(3, "other_count"),
    dbl(4, "is_shutdown"),
  ]),

  // Written by the daily cron (cdn_fleet.ts snapshotToDataPoints), never by a
  // device event, so no event fans out to it. Slot 1 is the UTC day described,
  // not a device_id.
  kind("cdn_fleet_daily", [
    blob(1, "date"),
    blob(2, "channel"),
    blob(3, "truncated"),
    dbl(1, "polls"),
    dbl(2, "sources"),
    dbl(3, "countries"),
    dbl(4, "errors"),
  ]),
];

export const POINT_KINDS: Record<string, PointKind> = Object.fromEntries(
  KINDS.map((k) => [k.kind, k]),
);

/** Event type -> the point kinds it writes, in write order. */
export const EVENT_FANOUT: Record<string, string[]> = {
  session: ["session"],
  print_outcome: ["print_outcome"],
  crash: ["crash"],
  update_failed: ["update_failed"],
  update_success: ["update_success"],
  memory_snapshot: ["memory_snapshot"],
  memory_warning: ["memory_warning"],
  hardware_profile: ["hardware_profile"],
  settings_snapshot: ["settings_snapshot", "widget_placement"],
  panel_usage: ["panel_usage", "widget_interaction", "overlay_visit"],
  connection_stability: ["connection_stability"],
  screensaver_usage: ["screensaver_usage", "screensaver_level"],
  print_start_context: ["print_start_context"],
  error_encountered: ["error_encountered"],
  performance_snapshot: ["performance_snapshot"],
  feature_adoption: ["feature_adoption"],
  settings_changes: ["settings_change"],
  async_lifetime_skips: ["async_lifetime_skips"],
};

function mergeLists(...groups: Array<Record<string, string[]>>): Record<string, string[]> {
  const out: Record<string, string[]> = {};
  for (const group of groups) {
    for (const [event, paths] of Object.entries(group)) (out[event] ??= []).push(...paths);
  }
  return out;
}

/**
 * Event type -> client fields deliberately kept out of Analytics Engine, in
 * ColumnDef.paths notation. An entry keeps its whole subtree: R2 still holds
 * the raw event. Every field the client sends is either read by a column of
 * the event's kinds or listed here (src/__tests__/contract.test.ts).
 */
export const R2_ONLY: Record<string, string[]> = mergeLists(
  // AE write time is the time axis, so the client's clock is never a column.
  Object.fromEntries(Object.keys(EVENT_FANOUT).map((event) => [event, ["timestamp"]])),
  // Envelope fields a kind has no column for. boot_id and schema_version are
  // columns on the delta kinds only, where they key and gate the deltas.
  {
    session: ["boot_id", "schema_version"],
    print_outcome: ["boot_id", "schema_version"],
    crash: ["boot_id", "schema_version"],
    update_failed: ["boot_id", "schema_version", "app_board"],
    update_success: ["boot_id", "schema_version", "app_board"],
    memory_snapshot: ["boot_id", "schema_version", "app_board"],
    memory_warning: ["boot_id", "schema_version", "app_board"],
    hardware_profile: ["boot_id", "schema_version"],
    settings_snapshot: ["boot_id", "schema_version", "app_board"],
    print_start_context: ["boot_id", "schema_version"],
    error_encountered: ["boot_id", "schema_version"],
    performance_snapshot: ["boot_id", "schema_version", "app_board"],
    settings_changes: ["boot_id", "schema_version", "app_board"],
    async_lifetime_skips: ["boot_id", "schema_version", "app_board"],
  },
  // Update events store the two versions they move between, and app_platform;
  // app_version repeats one version and `platform` names the release's target.
  { update_failed: ["app_version", "platform"], update_success: ["app_version", "platform"] },
  // The session kind reads its app section, which repeats these envelope fields.
  { session: ["app_version", "app_platform", "app_board"] },
  // Marks a snapshot taken at shutdown, which describes the sender rather than the window.
  {
    panel_usage: ["is_shutdown"],
    connection_stability: ["is_shutdown"],
    screensaver_usage: ["is_shutdown"],
    performance_snapshot: ["is_shutdown"],
  },
  // Session detail the hardware_profile kind stores in queryable form (model, MCU,
  // volume, capability bits), or that no dashboard view reads.
  {
    session: [
      "features",
      "host.cpu_model",
      "host.os",
      "host.screen_locality",
      "printer.build_volume",
      "printer.has_chamber",
      "printer.has_heated_bed",
      "printer.klipper_version",
      "printer.moonraker_version",
      "printer.mcu",
      "printer.mcu_count",
    ],
  },
  // Display hardware no view charts; the session kind stores the resolution.
  {
    session: ["app.display_backend", "app.input_type", "app.has_backlight", "app.has_hw_blank"],
    hardware_profile: ["display_backend"],
  },
  // Name-level hardware lists, read from R2 for printer-detection analysis.
  {
    hardware_profile: [
      "printer_objects",
      "mcus.chips",
      "fans.names",
      "leds.names",
      "macros.names",
      "sensors.filament_names",
      "sensors.temperature_names",
      "steppers.names",
    ],
  },
  // Hardware detail beyond the counts and capability bits the Hardware view charts.
  {
    hardware_profile: [
      "mcus.count",
      "extruders.has_chamber_heater",
      "fans.part_cooling",
      "fans.heater_fan",
      "fans.controller_fan",
      "fans.generic",
      "steppers.count",
      "leds.has_led_effects",
      "sensors.load_cell",
      "probe.has_bed_mesh",
      "probe.has_qgl",
      "probe.has_z_tilt",
      "probe.has_screws_tilt",
      "ams.unit_count",
      "ams.total_slots",
      "tools.count",
      "macros.has_print_start",
      "macros.has_nozzle_clean",
      "macros.has_heat_soak",
      "macros.has_purge_line",
      "macros.led_macro_count",
      "plugins.helix_plugin_installed",
    ],
  },
  // Process memory breakdown for one-off investigations; the kinds store the RSS and VM series.
  {
    memory_snapshot: [
      "vm_data_kb",
      "vm_swap_kb",
      "private_dirty_kb",
      "private_clean_kb",
      "shared_clean_kb",
      "pss_kb",
      "system_total_mb",
      "system_available_mb",
      "malloc_arena_kb",
      "malloc_uordblks_kb",
      "malloc_fordblks_kb",
      "malloc_hblks",
      "malloc_hblkhd_kb",
    ],
    memory_warning: [
      "vm_hwm_kb",
      "vm_swap_kb",
      "private_clean_kb",
      "shared_clean_kb",
      "shared_dirty_kb",
      "swap_pss_kb",
      "print_state",
      "active_panel",
      "gcode_renderer_loaded",
    ],
  },
  // Crash forensics for symbolizing one crash; AE keeps the fields crashes are grouped and counted by.
  { crash: ["backtrace", "memory_map", "fault_code", "reg_pc", "reg_sp", "reg_lr", "reg_bp"] },
  // Per-panel frame breakdown; the kind stores the worst panel and its p95.
  { performance_snapshot: ["panels"] },
  // Per-tag skip counts; the kind stores the window totals.
  { async_lifetime_skips: ["tags"] },
);

// ---------- Lookup ----------

export function hasKind(kindName: string): boolean {
  return Object.prototype.hasOwnProperty.call(POINT_KINDS, kindName);
}

function findColumn(kindName: string, field: string): ColumnDef | undefined {
  return hasKind(kindName) ? POINT_KINDS[kindName].columns.find((c) => c.field === field) : undefined;
}

/** The Analytics Engine column name for a field: "blob7" / "double3". */
export function col(kindName: string, field: string): string {
  if (!hasKind(kindName)) throw new Error(`unknown point kind: ${kindName}`);
  const c = findColumn(kindName, field);
  if (!c) throw new Error(`point kind ${kindName} has no column ${field}`);
  return `${c.type}${c.slot}`;
}

export function hasCol(kindName: string, field: string): boolean {
  return findColumn(kindName, field) !== undefined;
}

/** One Analytics Engine row as the SQL API returns it: every slot plus timestamp and index1. */
export type RawRow = Record<string, unknown>;

/** The rows of one SQL API result; a missing or failed result has none. */
export function rowsOf<T = RawRow>(res: unknown): T[] {
  return (res as { data?: T[] } | null)?.data ?? [];
}

/** A cell read as a number; missing or non-numeric reads 0. */
export const num = (v: unknown): number => Number(v ?? 0) || 0;

/** A cell read as a string; missing reads "". */
export const str = (v: unknown): string => String(v ?? "");

/** The dashboard filters. Each one matches a schema field that differs per point kind (queries.ts). */
export const FILTER_KEYS = ["platform", "version", "model", "board"] as const;
export type FilterKey = (typeof FILTER_KEYS)[number];

/**
 * Per payload block, the active filter none of the block's point kinds carry,
 * or null. A block with one reads "not reported per <filter>": its zeros are
 * not measurements.
 */
export type NotApplicable<K extends string> = Record<K, FilterKey | null>;

/**
 * Every column of a kind read from its slot in a raw row, keyed by field name.
 * Empty strings and zero doubles are kept; a missing slot reads as "" or 0.
 */
export function decodeRow(kindName: string, row: RawRow): Record<string, string | number> {
  if (!hasKind(kindName)) throw new Error(`unknown point kind: ${kindName}`);
  const out: Record<string, string | number> = {};
  for (const c of POINT_KINDS[kindName].columns) {
    const raw = row[`${c.type}${c.slot}`];
    out[c.field] = c.type === "blob" ? String(raw ?? "") : Number(raw ?? 0) || 0;
  }
  return out;
}
