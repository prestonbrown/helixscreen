// SPDX-License-Identifier: GPL-3.0-or-later
// Maps telemetry events to Analytics Engine data points for real-time querying.

export interface AnalyticsEngineDataPoint {
  blobs?: string[];
  doubles?: number[];
  indexes?: string[];
}

/**
 * Maps a raw telemetry event to one or more Analytics Engine data points.
 * Most events produce a single data point; panel_usage is expanded into
 * one data point per panel (since C++ sends an aggregate per-session event).
 */
export function mapEventToDataPoints(
  event: Record<string, unknown>,
): AnalyticsEngineDataPoint[] {
  const result = mapEventToDataPointInternal(event);
  return Array.isArray(result) ? result : [result];
}

/**
 * Maps a raw telemetry event to an Analytics Engine data point (or array for panel_usage).
 * Each event type has a specific schema for blobs/doubles to enable
 * efficient SQL queries via the Analytics Engine SQL API.
 */
function mapEventToDataPointInternal(
  event: Record<string, unknown>,
): AnalyticsEngineDataPoint | AnalyticsEngineDataPoint[] {
  const eventType = String(event.event ?? "unknown");
  const deviceId = String(event.device_id ?? "");

  // Support both flat (schema v1) and nested (schema v2) field access
  const app = (event.app ?? {}) as Record<string, unknown>;
  const printer = (event.printer ?? {}) as Record<string, unknown>;
  const host = (event.host ?? {}) as Record<string, unknown>;

  if (eventType === "session") {
    return {
      indexes: ["session"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(printer.detected_model ?? event.printer_model ?? ""),
        String(printer.kinematics ?? event.kinematics ?? ""),
        String(app.display ?? event.display ?? ""),
        String(app.locale ?? event.locale ?? ""),
        String(app.theme ?? event.theme ?? ""),
        String(host.arch ?? event.arch ?? ""),
        // blob10: zip-extraction capability ("unzip" | "python" | "none").
        // Empty for clients older than the field. Gates the .tar.gz retirement
        // (#993) — see build_session_event() in src/system/telemetry_manager.cpp.
        String(app.zip_tool ?? event.zip_tool ?? ""),
        "",
        "",
      ],
      doubles: [
        Number(host.ram_total_mb ?? event.ram_total_mb ?? 0),
        Number(host.cpu_cores ?? event.cpu_cores ?? 0),
        Number(printer.extruder_count ?? event.extruder_count ?? 0),
        0,
        0,
        0,
        0,
        0,
      ],
    };
  }

  if (eventType === "print_outcome") {
    return {
      indexes: ["print_outcome"],
      blobs: [
        deviceId,
        String(event.outcome ?? ""),
        String(event.filament_type ?? ""),
        String(app.version ?? event.app_version ?? event.version ?? ""),
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
      ],
      doubles: [
        Number(event.duration_sec ?? 0),
        Number(event.nozzle_temp ?? 0),
        Number(event.bed_temp ?? 0),
        Number(event.filament_used_mm ?? 0),
        Number(event.phases_completed ?? 0),
        0,
        0,
        0,
      ],
    };
  }

  if (eventType === "crash") {
    return {
      indexes: ["crash"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(event.signal_name ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(event.load_base ?? ""),
        String(event.fault_code_name ?? ""),
        String(event.fault_addr ?? ""),
        "",
        "",
        "",
        "",
        "",
      ],
      doubles: [
        Number(event.uptime_sec ?? 0),
        Number(event.signal ?? event.signal_number ?? 0),
        Number(
          event.backtrace_depth ??
            (Array.isArray(event.backtrace) ? event.backtrace.length : 0),
        ),
        0,
        0,
        0,
        0,
        0,
      ],
    };
  }

  // Update outcome events. Both carry a version PAIR: `version` is the release
  // being moved to and `from_version` is the release it moved from.
  //
  // blob2 is "the version the device is RUNNING", matching every other event
  // type so the default filter blob map (version=blob2, platform=blob3) means
  // the same thing everywhere. That differs per event:
  //   update_failed  -> from_version (the install did not happen)
  //   update_success -> version      (the device is now on the new release)
  // Getting this wrong on update_failed makes the default version filter
  // useless: the target is always the newest release, so every failure row
  // collapses into one bucket. The question we need answered is which
  // DEPLOYED versions are failing (prestonbrown/helixscreen#993), which is
  // exactly from_version.
  if (eventType === "update_failed") {
    return {
      indexes: ["update_failed"],
      blobs: [
        deviceId,                                                           // blob1
        String(event.from_version ?? ""),                                   // blob2 RUNNING
        String(app.platform ?? event.app_platform ?? event.platform ?? ""), // blob3
        String(event.reason ?? ""),                                         // blob4
        String(app.version ?? event.app_version ?? event.version ?? ""),    // blob5 target
        "", "", "", "", "", "", "",
      ],
      doubles: [
        Number(event.http_code ?? 0),  // double1
        Number(event.file_size ?? 0),  // double2
        Number(event.exit_code ?? 0),  // double3
        0, 0, 0, 0, 0,
      ],
    };
  }

  if (eventType === "update_success") {
    return {
      indexes: ["update_success"],
      blobs: [
        deviceId,                                                           // blob1
        String(app.version ?? event.app_version ?? event.version ?? ""),    // blob2 new
        String(app.platform ?? event.app_platform ?? event.platform ?? ""), // blob3
        String(event.from_version ?? ""),                                   // blob4
        "", "", "", "", "", "", "", "",
      ],
      doubles: [0, 0, 0, 0, 0, 0, 0, 0],
    };
  }

  if (eventType === "memory_snapshot") {
    return {
      indexes: ["memory_snapshot"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(event.trigger ?? ""),
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
      ],
      doubles: [
        Number(event.uptime_sec ?? 0),
        Number(event.rss_kb ?? 0),
        Number(event.vm_size_kb ?? 0),
        Number(event.vm_peak_kb ?? 0),
        Number(event.vm_hwm_kb ?? 0),
        0,
        0,
        0,
      ],
    };
  }

  if (eventType === "memory_warning") {
    return {
      indexes: ["memory_warning"],
      blobs: [
        deviceId,                                                          // blob1
        String(app.version ?? event.app_version ?? event.version ?? ""),   // blob2
        String(app.platform ?? event.app_platform ?? event.platform ?? ""), // blob3
        String(event.level ?? ""),                                         // blob4
        String(event.reason ?? ""),                                        // blob5
        "", "", "", "", "", "", "",
      ],
      doubles: [
        Number(event.uptime_sec ?? 0),           // double1
        Number(event.rss_kb ?? 0),               // double2
        Number(event.vm_size_kb ?? 0),           // double3
        Number(event.system_available_mb ?? 0),  // double4
        Number(event.growth_5min_kb ?? 0),       // double5
        Number(event.private_dirty_kb ?? 0),     // double6
        Number(event.pss_kb ?? 0),               // double7
        Number(event.system_total_mb ?? 0),      // double8
      ],
    };
  }

  if (eventType === "hardware_profile") {
    // Extract MCU chip from nested mcus object (C++ sends mcus.primary)
    const mcus = (event.mcus ?? {}) as Record<string, unknown>;
    const mcuChip = String(mcus.primary ?? event.mcu_chip ?? "");

    // Extract probe type from nested probe object
    const probe = (event.probe ?? {}) as Record<string, unknown>;
    const probeType = String(probe.type ?? event.probe_type ?? "");

    // Extract build volume from nested object
    const buildVol = (event.build_volume ?? {}) as Record<string, unknown>;

    // Extract extruder count from nested object
    const extruders = (event.extruders ?? {}) as Record<string, unknown>;

    // Extract fan/sensor/macro counts from nested objects
    // C++ sends fans.total (not fans.count), sensors has sub-fields, macros.total_count
    const fans = (event.fans ?? {}) as Record<string, unknown>;
    const sensors = (event.sensors ?? {}) as Record<string, unknown>;
    const macros = (event.macros ?? {}) as Record<string, unknown>;
    const sensorCount =
      Number(sensors.filament ?? 0) +
      Number(sensors.temperature_extra ?? 0) +
      Number(sensors.color ?? 0) +
      Number(sensors.accel ?? 0);

    // Build capabilities bitmask from nested boolean object + derived flags
    // Bit order: 0=chamber, 1=accelerometer, 2=firmware_retraction,
    //   3=exclude_object, 4=timelapse, 5=klippain_shaketune, 6=speaker,
    //   7=probe, 8=led, 9=filament_sensor, 10=multi_extruder, 11=ams, 12=heater_bed
    const caps = (event.capabilities ?? {}) as Record<string, unknown>;
    const leds = (event.leds ?? {}) as Record<string, unknown>;
    const tools = (event.tools ?? {}) as Record<string, unknown>;
    let capsBitmask = Number(event.capabilities_bitmask ?? 0);
    if (typeof caps === "object" && caps !== null && Object.keys(caps).length > 0) {
      const capBits = [
        "has_chamber",
        "has_accelerometer",
        "has_firmware_retraction",
        "has_exclude_object",
        "has_timelapse",
        "has_klippain_shaketune",
        "has_speaker",
      ];
      capsBitmask = 0;
      for (let i = 0; i < capBits.length; i++) {
        if (caps[capBits[i]]) capsBitmask |= 1 << i;
      }
      // Derived flags from other event sections
      if (probe.has_probe) capsBitmask |= 1 << 7;
      if (Number(leds.count ?? 0) > 0) capsBitmask |= 1 << 8;
      if (Number(sensors.filament ?? 0) > 0) capsBitmask |= 1 << 9;
      if (tools.is_multi_tool || Number(extruders.count ?? 0) > 1) capsBitmask |= 1 << 10;
      if (event.ams) capsBitmask |= 1 << 11;
      if (extruders.has_heater_bed) capsBitmask |= 1 << 12;
    }

    // Extract AMS backend type from nested ams object
    const ams = (event.ams ?? {}) as Record<string, unknown>;
    const amsType = String(ams.type ?? "");

    // Tri-state as a string rather than a capsBitmask bit: a bit cannot tell
    // "false" apart from "this client is too old to report it", so every event
    // predating the field would read as a definite false.
    const triState = (v: unknown): string =>
      v === undefined || v === null ? "" : v ? "1" : "0";

    return {
      indexes: ["hardware_profile"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(printer.detected_model ?? event.printer_model ?? ""),
        String(printer.kinematics ?? event.kinematics ?? ""),
        mcuChip,
        probeType,
        amsType,
        // blob9: are HelixScreen's helper macros installed? Sent by the client
        // since schema v2 but never projected until now, so this starts
        // answering immediately without waiting on a client release.
        triState(macros.has_helix_macros),
        // blob10: is Moonraker on this machine or reached over the network?
        // Only ever the boolean - never the host or address.
        triState(event.moonraker_is_local),
        "",
        "",
      ],
      doubles: [
        Number(extruders.count ?? printer.extruder_count ?? event.extruder_count ?? 0),
        Number(fans.total ?? fans.count ?? event.fan_count ?? 0),
        sensorCount || Number(sensors.count ?? event.sensor_count ?? 0),
        Number(macros.total_count ?? macros.count ?? event.macro_count ?? 0),
        Number(buildVol.x_mm ?? event.build_vol_x ?? 0),
        Number(buildVol.y_mm ?? event.build_vol_y ?? 0),
        Number(buildVol.z_mm ?? event.build_vol_z ?? 0),
        capsBitmask,
      ],
    };
  }

  if (eventType === "settings_snapshot") {
    // Theme: prefer theme_name (full name like "Nord (Dark)") over dark/light mode string
    const themeName = String(event.theme_name ?? app.theme ?? event.theme ?? "");
    // Dark/light mode: "dark" or "light" for the dark vs light chart
    const darkLight = String(event.theme ?? (themeName.includes("(Dark)") ? "dark" : themeName.includes("(Light)") ? "light" : ""));
    const version = String(app.version ?? event.app_version ?? event.version ?? "");
    const platform = String(app.platform ?? event.app_platform ?? event.platform ?? "");
    const mainPoint: AnalyticsEngineDataPoint = {
      indexes: ["settings_snapshot"],
      blobs: [
        deviceId,
        version,
        platform,
        themeName,
        String(app.locale ?? event.locale ?? ""),
        String(event.update_channel ?? ""),
        String(event.time_format ?? ""),
        darkLight,
        "",
        "",
        "",
        "",
      ],
      doubles: [
        Number(event.brightness_pct ?? 0),
        Number(event.screensaver_timeout_sec ?? 0),
        Number(event.blank_timeout_sec ?? 0),
        Number(event.sound_enabled ?? 0),
        Number(event.animations_enabled ?? 0),
        0,
        0,
        0,
      ],
    };

    // Expand home widget placement into separate data points
    const homeWidgets = event.home_widgets;
    if (Array.isArray(homeWidgets) && homeWidgets.length > 0) {
      const points: AnalyticsEngineDataPoint[] = [mainPoint];

      // Count occurrences of each widget type
      const widgetCounts = new Map<string, number>();
      for (const w of homeWidgets) {
        const id = String(w);
        widgetCounts.set(id, (widgetCounts.get(id) ?? 0) + 1);
      }

      for (const [widgetId, count] of widgetCounts) {
        points.push({
          indexes: ["widget_placement"],
          blobs: [
            deviceId, version, platform, widgetId,
            "", "", "", "", "", "", "", "",
          ],
          doubles: [
            count,
            0, 0, 0, 0, 0, 0, 0,
          ],
        });
      }
      return points;
    }

    return mainPoint;
  }

  if (eventType === "panel_usage") {
    const version = String(app.version ?? event.app_version ?? event.version ?? "");
    const platform = String(app.platform ?? event.app_platform ?? event.platform ?? "");
    const sessionDuration = Number(event.session_duration_sec ?? 0);
    const overlayCount = Number(event.overlay_open_count ?? 0);

    // C++ sends aggregate maps: panel_time_sec={panel:sec}, panel_visits={panel:count}
    // Expand into one data point per panel for Analytics Engine querying
    const timeMap = (event.panel_time_sec ?? {}) as Record<string, unknown>;
    const visitMap = (event.panel_visits ?? {}) as Record<string, unknown>;
    const panelNames = new Set([...Object.keys(timeMap), ...Object.keys(visitMap)]);

    if (panelNames.size > 0) {
      const points: AnalyticsEngineDataPoint[] = [];
      for (const panel of panelNames) {
        points.push({
          indexes: ["panel_usage"],
          blobs: [
            deviceId, version, platform, panel,
            "", "", "", "", "", "", "", "",
          ],
          doubles: [
            sessionDuration,
            Number(timeMap[panel] ?? 0),
            Number(visitMap[panel] ?? 0),
            overlayCount,
            0, 0, 0, 0,
          ],
        });
      }
      // Expand widget interactions into separate data points
      const widgetMap = (event.widget_interactions ?? {}) as Record<string, unknown>;
      for (const [widgetId, count] of Object.entries(widgetMap)) {
        points.push({
          indexes: ["widget_interaction"],
          blobs: [
            deviceId, version, platform, widgetId,
            "", "", "", "", "", "", "", "",
          ],
          doubles: [
            sessionDuration,
            Number(count),
            0, 0, 0, 0, 0, 0,
          ],
        });
      }

      // Expand per-overlay visit counts into separate data points
      const overlayVisitMap = (event.overlay_visits ?? {}) as Record<string, unknown>;
      for (const [overlayName, count] of Object.entries(overlayVisitMap)) {
        points.push({
          indexes: ["overlay_visit"],
          blobs: [
            deviceId, version, platform, overlayName,
            "", "", "", "", "", "", "", "",
          ],
          doubles: [
            sessionDuration,
            Number(count),
            0, 0, 0, 0, 0, 0,
          ],
        });
      }

      return points;
    }

    // Fallback: flat format (pre-expansion schema or already per-panel)
    return {
      indexes: ["panel_usage"],
      blobs: [
        deviceId, version, platform,
        String(event.panel_name ?? ""),
        "", "", "", "", "", "", "", "",
      ],
      doubles: [
        sessionDuration,
        Number(event.time_sec ?? 0),
        Number(event.visits ?? 0),
        overlayCount,
        0, 0, 0, 0,
      ],
    };
  }

  if (eventType === "connection_stability") {
    return {
      indexes: ["connection_stability"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
      ],
      doubles: [
        Number(event.session_duration_sec ?? 0),
        Number(event.connect_count ?? 0),
        Number(event.disconnect_count ?? 0),
        Number(event.connected_sec ?? 0),
        Number(event.disconnected_sec ?? 0),
        Number(event.longest_disconnect_sec ?? 0),
        Number(event.klippy_error_count ?? 0),
        Number(event.klippy_shutdown_count ?? 0),
      ],
    };
  }

  if (eventType === "print_start_context") {
    return {
      indexes: ["print_start_context"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(event.slicer ?? ""),
        String(event.file_size_bucket ?? ""),
        String(event.duration_bucket ?? ""),
        String(event.source ?? ""),
        "",
        "",
        "",
        "",
        "",
      ],
      doubles: [
        Number(event.tool_count_used ?? 0),
        Number(event.has_thumbnail ?? 0),
        Number(event.ams_active ?? 0),
        0,
        0,
        0,
        0,
        0,
      ],
    };
  }

  if (eventType === "error_encountered") {
    return {
      indexes: ["error_encountered"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(event.category ?? ""),
        String(event.code ?? ""),
        String(event.context ?? ""),
        "",
        "",
        "",
        "",
        "",
        "",
      ],
      doubles: [
        Number(event.uptime_sec ?? 0),
        0,
        0,
        0,
        0,
        0,
        0,
        0,
      ],
    };
  }

  if (eventType === "performance_snapshot") {
    return {
      indexes: ["performance_snapshot"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(event.worst_panel ?? ""),
        "", "", "", "", "", "", "", "",
      ],
      doubles: [
        Number(event.snapshot_seq ?? 0),
        Number(event.frame_time_p50_ms ?? 0),
        Number(event.frame_time_p95_ms ?? 0),
        Number(event.frame_time_p99_ms ?? 0),
        Number(event.dropped_frame_count ?? 0),
        Number(event.total_frame_count ?? 0),
        Number(event.worst_panel_p95_ms ?? 0),
        Number(event.task_handler_max_ms ?? 0),
      ],
    };
  }

  if (eventType === "feature_adoption") {
    const features = (event.features ?? {}) as Record<string, boolean>;
    // First 7 features get individual double slots (indices 0-6).
    // Index 7 holds a bitmask for remaining features.
    const featureOrder = [
      "macros", "camera", "bed_mesh", "console_gcode",
      "input_shaper", "filament_management", "manual_probe",
    ];
    const doubles: number[] = featureOrder.map(f => features[f] ? 1 : 0);
    const bitmaskFeatures = [
      "spoolman", "led_control", "power_devices", "multi_printer",
      "theme_changed", "timelapse", "favorites", "pid_calibration",
      "firmware_retraction",
    ];
    let bitmask = 0;
    for (let i = 0; i < bitmaskFeatures.length; i++) {
      if (features[bitmaskFeatures[i]]) bitmask |= 1 << i;
    }
    doubles.push(bitmask);

    return {
      indexes: ["feature_adoption"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        "", "", "", "", "", "", "", "", "",
      ],
      doubles,
    };
  }

  if (eventType === "settings_changes") {
    const changes = (event.changes ?? []) as Array<{
      setting: string;
      old_value: string;
      new_value: string;
    }>;
    const version = String(app.version ?? event.app_version ?? event.version ?? "");
    const platform = String(app.platform ?? event.app_platform ?? event.platform ?? "");

    if (changes.length > 0) {
      return changes.map((c) => ({
        indexes: ["settings_change"],
        blobs: [
          deviceId, version, platform,
          String(c.setting ?? ""),
          String(c.old_value ?? ""),
          String(c.new_value ?? ""),
          "", "", "", "", "", "",
        ],
        doubles: [1, 0, 0, 0, 0, 0, 0, 0] as number[],
      }));
    }

    return {
      indexes: ["settings_change"],
      blobs: [deviceId, version, platform, "", "", "", "", "", "", "", "", ""],
      doubles: [0, 0, 0, 0, 0, 0, 0, 0],
    };
  }

  // Unknown event type — still write basic info for discoverability
  return {
    indexes: [eventType],
    blobs: [deviceId, eventType, "", "", "", "", "", "", "", "", "", ""],
    doubles: [0, 0, 0, 0, 0, 0, 0, 0],
  };
}
