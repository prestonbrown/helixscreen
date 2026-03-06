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
        "",
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
    const fans = (event.fans ?? {}) as Record<string, unknown>;
    const sensors = (event.sensors ?? {}) as Record<string, unknown>;
    const macros = (event.macros ?? {}) as Record<string, unknown>;

    // Build capabilities bitmask from nested boolean object
    // Bit order: 0=chamber, 1=accelerometer, 2=firmware_retraction,
    //            3=exclude_object, 4=timelapse, 5=klippain_shaketune, 6=speaker
    const caps = (event.capabilities ?? {}) as Record<string, unknown>;
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
    }

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
        "",
        "",
        "",
        "",
        "",
      ],
      doubles: [
        Number(extruders.count ?? printer.extruder_count ?? event.extruder_count ?? 0),
        Number(fans.count ?? event.fan_count ?? 0),
        Number(sensors.count ?? event.sensor_count ?? 0),
        Number(macros.count ?? event.macro_count ?? 0),
        Number(buildVol.x_mm ?? event.build_vol_x ?? 0),
        Number(buildVol.y_mm ?? event.build_vol_y ?? 0),
        Number(buildVol.z_mm ?? event.build_vol_z ?? 0),
        capsBitmask,
      ],
    };
  }

  if (eventType === "settings_snapshot") {
    // Theme: prefer theme_name (full name like "Nord") over dark/light mode string
    const themeName = String(event.theme_name ?? app.theme ?? event.theme ?? "");
    return {
      indexes: ["settings_snapshot"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        themeName,
        String(app.locale ?? event.locale ?? ""),
        String(event.update_channel ?? ""),
        String(event.time_format ?? ""),
        "",
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

  // Unknown event type — still write basic info for discoverability
  return {
    indexes: [eventType],
    blobs: [deviceId, eventType, "", "", "", "", "", "", "", "", "", ""],
    doubles: [0, 0, 0, 0, 0, 0, 0, 0],
  };
}
