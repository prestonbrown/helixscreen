import { useAuthStore } from '@/stores/auth'
import { router } from '@/router'

const API_BASE = import.meta.env.VITE_API_BASE || 'https://telemetry.helixscreen.org'

export interface OverviewData {
  active_devices: number
  total_events: number
  crash_rate: number
  print_success_rate: number
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
}

export interface PrintsData {
  success_rate_over_time: { date: string; rate: number; total: number }[]
  by_filament: { type: string; success_rate: number; count: number }[]
  avg_duration_sec: number
  start_context?: PrintStartData
}

export interface CrashesData {
  by_version: { version: string; crash_count: number; session_count: number; rate: number }[]
  by_signal: { signal: string; count: number }[]
  avg_uptime_sec: number
  by_platform?: {
    platform: string
    crash_count: number
    session_count: number
    crashing_devices: number
    session_devices: number
    rate: number
  }[]
  trailing_14d?: {
    crash_count: number
    session_count: number
    rate: number
    by_platform: {
      platform: string
      crash_count: number
      session_count: number
      rate: number
    }[]
  }
}

export interface CrashListData {
  crashes: {
    timestamp: string
    device_id: string
    version: string
    signal: string
    platform: string
    uptime_sec: number
    occurrences: number
  }[]
}

export interface ReleasesData {
  versions: {
    version: string
    active_devices: number
    crash_rate: number
    print_success_rate: number
    total_sessions: number
    total_crashes: number
  }[]
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

export interface EngagementData {
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

export interface ReliabilityData {
  uptime_trend: { date: string; avg_uptime_pct: number }[]
  disconnect_trend: { date: string; avg_disconnects: number }[]
  max_disconnect_sec: number
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

export interface StabilityData {
  crash_rate_trend: { date: string; crashes: number; sessions: number; rate: number }[]
  by_version: { version: string; crash_count: number; session_count: number; rate: number }[]
  by_signal: { signal: string; count: number }[]
  avg_uptime_sec: number
  klippy_trend: { date: string; errors: number; shutdowns: number }[]
  memory_warnings_trend: { date: string; count: number }[]
  error_categories: { category: string; count: number }[]
  error_codes: { category: string; code: string; count: number }[]
  recent_crashes: { timestamp: string; device_id: string; version: string; signal: string; platform: string; uptime_sec: number; occurrences: number }[]
  display_anomalies?: DisplayAnomalyData
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
}

export interface FeaturesData {
  total_devices: number
  features: { name: string; adoption_rate: number }[]
  by_version: { version: string; devices: number; macros: number; camera: number; bed_mesh: number }[]
}

export interface UxInsightsData {
  avg_session_sec: number
  most_visited_panel: string
  least_visited_panel: string
  settings_change_rate_per_device_per_week: number
  panel_time: { panel: string; total_time_sec: number }[]
  panel_visits: { panel: string; total_visits: number; visits_per_device: number }[]
  settings_changes: { setting: string; change_count: number }[]
  settings_defaults: { setting: string; pct_changed: number; devices_changed: number }[]
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
    throw new Error(`API error: ${res.status} ${res.statusText}`)
  }

  return res.json() as Promise<T>
}

export const api = {
  getOverview(queryString: string): Promise<OverviewData> {
    return apiFetch(`/v1/dashboard/overview?${queryString}`)
  },

  getAdoption(queryString: string): Promise<AdoptionData> {
    return apiFetch(`/v1/dashboard/adoption?${queryString}`)
  },

  getPrints(queryString: string): Promise<PrintsData> {
    return apiFetch(`/v1/dashboard/prints?${queryString}`)
  },

  getCrashes(queryString: string): Promise<CrashesData> {
    return apiFetch(`/v1/dashboard/crashes?${queryString}`)
  },

  getCrashList(queryString: string, limit = 50): Promise<CrashListData> {
    return apiFetch(`/v1/dashboard/crash-list?${queryString}&limit=${limit}`)
  },

  getReleases(versions: string[]): Promise<ReleasesData> {
    const params = `versions=${versions.map(v => encodeURIComponent(v)).join(',')}`
    return apiFetch(`/v1/dashboard/releases?${params}`)
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

  getStability(queryString: string): Promise<StabilityData> {
    return apiFetch(`/v1/dashboard/stability?${queryString}`)
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
