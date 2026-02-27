// SPDX-License-Identifier: GPL-3.0-or-later
// Maps telemetry events to Analytics Engine data points for real-time querying.

export interface AnalyticsEngineDataPoint {
  blobs?: string[];
  doubles?: number[];
  indexes?: string[];
}

/**
 * Maps a raw telemetry event to an Analytics Engine data point.
 * Each event type has a specific schema for blobs/doubles to enable
 * efficient SQL queries via the Analytics Engine SQL API.
 */
export function mapEventToDataPoint(
  event: Record<string, unknown>,
): AnalyticsEngineDataPoint {
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

  if (eventType === "hardware_profile") {
    return {
      indexes: ["hardware_profile"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(printer.detected_model ?? event.printer_model ?? ""),
        String(printer.kinematics ?? event.kinematics ?? ""),
        String(event.mcu_chip ?? ""),
        String(event.probe_type ?? ""),
        "",
        "",
        "",
        "",
        "",
      ],
      doubles: [
        Number(printer.extruder_count ?? event.extruder_count ?? 0),
        Number(event.fan_count ?? 0),
        Number(event.sensor_count ?? 0),
        Number(event.macro_count ?? 0),
        Number(event.build_vol_x ?? 0),
        Number(event.build_vol_y ?? 0),
        Number(event.build_vol_z ?? 0),
        Number(event.capabilities_bitmask ?? 0),
      ],
    };
  }

  if (eventType === "settings_snapshot") {
    return {
      indexes: ["settings_snapshot"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(app.theme ?? event.theme ?? ""),
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
    return {
      indexes: ["panel_usage"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(event.panel_name ?? ""),
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
        Number(event.time_sec ?? 0),
        Number(event.visits ?? 0),
        Number(event.overlay_open_count ?? 0),
        0,
        0,
        0,
        0,
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
