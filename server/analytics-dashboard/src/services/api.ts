import { useAuthStore } from '@/stores/auth'
import { router } from '@/router'
import type { HealthPayload } from '../../../telemetry-worker/src/health'
import type { DeviceList, DevicePage } from '../../../telemetry-worker/src/devices'
import type { NotApplicable } from '../../../telemetry-worker/src/schema'
import type { WeeklyReport } from '../../../telemetry-worker/src/loop/report'
import type { RetentionCohort } from '../../../telemetry-worker/src/retention'
import { withNotApplicable, type WireNotApplicable } from '@/utils/notApplicable'

const API_BASE = import.meta.env.VITE_API_BASE || 'https://telemetry.helixscreen.org'

// Null-valued fields:
// - A crash rate is null when the filters exclude every crash or session row
//   (a model filter matches no crash), rendered "n/a".
// - A value aggregated from delta point kinds (panel usage, overlay and widget
//   use, connection stability) is null until a device on a delta-era client
//   has reported; `delta_era_devices` says how many have.

export type FilterKey = 'platform' | 'version' | 'model' | 'board'

// Returned by every endpoint that reads delta kinds. `filter_not_applicable`
// names an active filter none of those kinds carry (no panel or connection row
// has a printer model), so their values are "not reported per <filter>" rather
// than waiting on the rollout.
export interface DeltaEra {
  delta_era_devices: number
  filter_not_applicable: FilterKey | null
}

export interface OverviewData {
  active_devices: number
  total_events: number
  crash_rate: number | null
  print_success_rate: number | null
  events_over_time: { date: string; count: number }[]
  daily_active_devices: { date: string; devices: number }[]
  cumulative_devices: { date: string; total: number }[]
  // Fleet size derived from CDN update-manifest polls. Unlike active_devices,
  // this does not depend on the user having opted into telemetry, so it counts
  // installs the rest of this payload cannot see. `sources` is distinct client
  // IPs for the day: NAT hides devices behind one address, dynamic IPs split
  // one device across several, so treat it as an estimate rather than a count.
  cdn_fleet: { date: string; sources: number; polls: number; errors: number }[]
}

export interface AdoptionData {
  platforms: { name: string; count: number }[]
  versions: { name: string; count: number }[]
  printer_models: { name: string; count: number }[]
  kinematics: { name: string; count: number }[]
  boards: { name: string; count: number }[]
  install_sources: { name: string; count: number }[]
  host_mods: { name: string; count: number }[]
}

export interface PrintsData {
  success_rate_over_time: { date: string; rate: number; total: number }[]
  by_filament: { type: string; success_rate: number; count: number }[]
  avg_duration_sec: number
  by_model: { model: string; successes: number; total: number; rate: number | null }[]
  by_version: { version: string; successes: number; total: number; rate: number | null }[]
  start_context?: PrintStartData
  not_applicable: NotApplicable<'prints'>
}

export interface CrashesData {
  by_version: { version: string; crash_count: number; session_count: number; rate: number | null }[]
  by_signal: { signal: string; count: number }[]
  avg_uptime_sec: number
  by_platform?: {
    platform: string
    crash_count: number
    session_count: number
    crashing_devices: number
    session_devices: number
    rate: number | null
  }[]
  trailing_14d?: {
    crash_count: number
    session_count: number
    rate: number | null
    by_platform: {
      platform: string
      crash_count: number
      session_count: number
      rate: number | null
    }[]
  }
}

export interface ReleasesData {
  versions: {
    version: string
    active_devices: number
    crash_rate: number | null
    crash_free_devices: number | null
    p95_frame_ms: number | null
    p95_rss_kb: number | null
    print_success_rate: number | null
    total_sessions: number
    total_crashes: number
    update_failures: number
    update_successes: number
    update_failure_rate: number | null
  }[]
  adoption: { date: string; version: string; devices: number }[]
  not_applicable: NotApplicable<'crashes' | 'prints' | 'updates' | 'performance'>
}

export type HealthData = HealthPayload
export type DevicePageData = DevicePage
export type DeviceListData = DeviceList
export interface RetentionData {
  cohorts: RetentionCohort[]
}

export interface MemoryData {
  rss_over_time: { date: string; avg_rss_kb: number; p95_rss_kb: number; max_rss_kb: number }[]
  rss_by_platform: { platform: string; avg_rss_kb: number }[]
  vm_peak_trend: { date: string; avg_vm_peak_kb: number }[]
}

export interface MemoryWarningsData {
  total_warnings: number
  affected_devices: number
  by_level: { level: string; count: number }[]
  over_time: { date: string; level: string; count: number }[]
  rss_at_warning: { date: string; avg_rss_kb: number; max_rss_kb: number }[]
  by_platform: { platform: string; count: number; avg_rss_kb: number }[]
  recent_warnings: {
    timestamp: string
    device_id: string
    version: string
    platform: string
    level: string
    reason: string
    uptime_sec: number
    rss_kb: number
    system_available_mb: number
    growth_5min_kb: number
    private_dirty_kb: number
    pss_kb: number
  }[]
}

export interface HardwareData {
  printer_models: { name: string; count: number }[]
  kinematics: { name: string; count: number }[]
  mcu_chips: { name: string; count: number }[]
  capabilities: { total: number; bits: number[] }
  avg_build_volume: { x: number; y: number; z: number }
  avg_counts: { fans: number; sensors: number; macros: number }
  ram_distribution: { name: string; count: number }[]
  ams_backends: { name: string; count: number }[]
  // `reported` is the denominator: devices that actually sent the field.
  // Clients too old to report it are excluded rather than counted as a "no".
  helix_macros: { installed: number; not_installed: number; reported: number }
  moonraker_locality: { local: number; remote: number; reported: number }
}

export interface EngagementData extends DeltaEra {
  avg_session_sec: number | null
  panel_time: { panel: string; total_time_sec: number }[]
  panel_visits: { panel: string; total_visits: number }[]
  session_duration_trend: { date: string; avg_session_sec: number }[]
  themes: { name: string; count: number }[]
  dark_vs_light: { name: string; count: number }[]
  locales: { name: string; count: number }[]
  brightness: { p25: number; p50: number; p75: number }
  widget_placement: { widget: string; devices: number }[]
  widget_interactions: { widget: string; interactions: number }[]
  overlay_visits: { overlay: string; total_visits: number }[]
}

export interface ReliabilityData extends DeltaEra {
  uptime_trend: { date: string; avg_uptime_pct: number }[]
  disconnect_trend: { date: string; avg_disconnects: number }[]
  max_disconnect_sec: number | null
  error_categories: { category: string; count: number }[]
  error_codes: { category: string; code: string; count: number }[]
}

export interface DisplayAnomalyData {
  affected_devices: number
  trend: { date: string; count: number }[]
  by_code: { code: string; count: number }[]
  by_version: { version: string; count: number }[]
  recent: { timestamp: string; device_id: string; version: string; platform: string; code: string; context: string; uptime_sec: number }[]
}

export interface StabilityData extends DeltaEra {
  crash_rate_trend: { date: string; crashes: number; sessions: number; rate: number | null }[]
  by_version: { version: string; crash_count: number; session_count: number; rate: number | null }[]
  by_signal: { signal: string; count: number }[]
  avg_uptime_sec: number
  klippy_trend: { date: string; errors: number; shutdowns: number }[]
  memory_warnings_trend: { date: string; count: number }[]
  error_categories: { category: string; count: number }[]
  error_codes: { category: string; code: string; count: number }[]
  recent_crashes: { timestamp: string; device_id: string; version: string; signal: string; platform: string; uptime_sec: number; occurrences: number }[]
  /** Crashes grouped by signal, fault code, version and board over the range; sample-weighted count, largest first. */
  crash_clusters: { signal: string; fault_code: string; version: string; board: string; count: number; devices: number }[]
  /** Error reports per category, code and version over the range; sample-weighted count, largest first. */
  errors_by_version: { category: string; code: string; version: string; count: number; devices: number }[]
  display_anomalies?: DisplayAnomalyData
  /** Crash and error blocks a filter leaves unreported; `filter_not_applicable` covers only the delta kinds. */
  not_applicable: NotApplicable<'crashes' | 'errors'>
}

export interface PrintStartData {
  slicers: { name: string; count: number }[]
  file_size_buckets: { name: string; count: number }[]
  thumbnail_rate: number
  ams_rate: number
  sources: { name: string; count: number }[]
}

export interface PerformanceData {
  fleet_p50_ms: number
  fleet_drop_rate: number
  high_drop_devices: number
  total_devices: number
  worst_panel: string
  frame_time_trend: { date: string; p50: number; p95: number; p99: number }[]
  drop_rate_by_platform: { platform: string; rate: number; dropped: number; total: number }[]
  drop_rate_by_version: { date: string; version: string; rate: number }[]
  jankiest_panels: { panel: string; times_worst: number; avg_p95_ms: number }[]
  by_platform_version: {
    platform: string
    version: string
    p95_frame_ms: number
    avg_rss_kb: number | null
    devices: number
  }[]
  jankiest_by_platform: { platform: string; panel: string; times_worst: number; avg_p95_ms: number }[]
}

// Adoption derived from overlay and widget use: `devices` used the overlay or
// widget `id` at least once in the range, out of `delta_era_devices`. Empty
// until a delta-era device has reported.
export interface FeaturesData extends DeltaEra {
  features: { id: string; devices: number; adoption_rate: number }[]
  /** Settings flags: devices with it on over devices reporting it; null rate before any report. */
  configured: { id: string; devices: number; reported: number; adoption_rate: number | null }[]
}

export interface UxInsightsData extends DeltaEra {
  avg_session_sec: number | null
  most_visited_panel: string
  least_visited_panel: string
  settings_change_rate_per_device_per_week: number
  panel_time: { panel: string; total_time_sec: number }[]
  panel_visits: { panel: string; total_visits: number; visits_per_device: number }[]
  settings_changes: { setting: string; change_count: number }[]
  settings_defaults: { setting: string; pct_changed: number; devices_changed: number }[]
}

export class ApiError extends Error {
  constructor(readonly status: number, statusText: string) {
    super(`API error: ${status} ${statusText}`)
  }
}

async function apiFetch<T>(path: string, params?: Record<string, string>): Promise<T> {
  const auth = useAuthStore()
  let url = `${API_BASE}${path}`
  if (params) {
    const sep = path.includes('?') ? '&' : '?'
    url += sep + new URLSearchParams(params).toString()
  }
  const res = await fetch(url, {
    headers: {
      'X-API-Key': auth.apiKey || ''
    }
  })

  if (res.status === 401) {
    auth.logout()
    router.push('/login')
    throw new Error('Unauthorized')
  }

  if (!res.ok) {
    throw new ApiError(res.status, res.statusText)
  }

  return res.json() as Promise<T>
}

// Each endpoint's not_applicable blocks; the normalized return types require every one.
const PRINTS_BLOCKS = ['prints'] as const
const RELEASES_BLOCKS = ['crashes', 'prints', 'updates', 'performance'] as const
const HEALTH_BLOCKS = ['crashes', 'errors', 'prints', 'updates'] as const
const STABILITY_BLOCKS = ['crashes', 'errors'] as const
const DEVICES_BLOCKS = ['crashes', 'prints'] as const

export const api = {
  getOverview(queryString: string): Promise<OverviewData> {
    return apiFetch(`/v1/dashboard/overview?${queryString}`)
  },

  getAdoption(queryString: string): Promise<AdoptionData> {
    return apiFetch(`/v1/dashboard/adoption?${queryString}`)
  },

  async getPrints(queryString: string): Promise<PrintsData> {
    const wire = await apiFetch<WireNotApplicable<PrintsData>>(`/v1/dashboard/prints?${queryString}`)
    return withNotApplicable(wire, PRINTS_BLOCKS)
  },

  getCrashes(queryString: string): Promise<CrashesData> {
    return apiFetch(`/v1/dashboard/crashes?${queryString}`)
  },

  // Without `versions` the worker picks the most active releases itself.
  async getReleases(queryString: string, versions?: string[]): Promise<ReleasesData> {
    const list = versions ? `&versions=${versions.map(v => encodeURIComponent(v)).join(',')}` : ''
    const wire = await apiFetch<WireNotApplicable<ReleasesData>>(`/v1/dashboard/releases?${queryString}${list}`)
    return withNotApplicable(wire, RELEASES_BLOCKS)
  },

  async getHealth(queryString: string): Promise<HealthData> {
    const wire = await apiFetch<WireNotApplicable<HealthData>>(`/v1/dashboard/health?${queryString}`)
    return withNotApplicable(wire, HEALTH_BLOCKS)
  },

  async getReports(): Promise<string[]> {
    return (await apiFetch<{ weeks: string[] }>('/v1/dashboard/reports')).weeks
  },

  // A stored report may predate a not_applicable block, so its health part is normalized like the live one.
  async getReport(week: string): Promise<WeeklyReport> {
    const wire = await apiFetch<Omit<WeeklyReport, 'health'> & { health: WireNotApplicable<HealthData> }>(
      `/v1/dashboard/reports/${encodeURIComponent(week)}`
    )
    return { ...wire, health: withNotApplicable(wire.health, HEALTH_BLOCKS) }
  },

  async getDevices(queryString: string): Promise<DeviceListData> {
    const wire = await apiFetch<WireNotApplicable<DeviceListData>>(`/v1/dashboard/devices?${queryString}`)
    return withNotApplicable(wire, DEVICES_BLOCKS)
  },

  // One device over a fixed 90 days: no filters apply.
  getDevice(id: string): Promise<DevicePageData> {
    return apiFetch(`/v1/dashboard/device/${encodeURIComponent(id)}`)
  },

  getRetention(queryString: string): Promise<RetentionData> {
    return apiFetch(`/v1/dashboard/fleet/retention?${queryString}`)
  },

  getMemory(queryString: string): Promise<MemoryData> {
    return apiFetch(`/v1/dashboard/memory?${queryString}`)
  },

  getMemoryWarnings(queryString: string): Promise<MemoryWarningsData> {
    return apiFetch(`/v1/dashboard/memory-warnings?${queryString}`)
  },

  getHardware(queryString: string): Promise<HardwareData> {
    return apiFetch(`/v1/dashboard/hardware?${queryString}`)
  },

  getEngagement(queryString: string): Promise<EngagementData> {
    return apiFetch(`/v1/dashboard/engagement?${queryString}`)
  },

  getReliability(queryString: string): Promise<ReliabilityData> {
    return apiFetch(`/v1/dashboard/reliability?${queryString}`)
  },

  async getStability(queryString: string): Promise<StabilityData> {
    const wire = await apiFetch<WireNotApplicable<StabilityData>>(`/v1/dashboard/stability?${queryString}`)
    return withNotApplicable(wire, STABILITY_BLOCKS)
  },

  getPerformance(queryString: string): Promise<PerformanceData> {
    return apiFetch(`/v1/dashboard/performance?${queryString}`)
  },

  getFeatures(queryString: string): Promise<FeaturesData> {
    return apiFetch(`/v1/dashboard/features?${queryString}`)
  },

  getUxInsights(queryString: string): Promise<UxInsightsData> {
    return apiFetch(`/v1/dashboard/ux?${queryString}`)
  },
}
