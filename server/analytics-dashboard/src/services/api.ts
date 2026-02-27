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
}

export interface CrashListData {
  crashes: {
    timestamp: string
    device_id: string
    version: string
    signal: string
    platform: string
    uptime_sec: number
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

export interface HardwareData {
  printer_models: { name: string; count: number }[]
  kinematics: { name: string; count: number }[]
  mcu_chips: { name: string; count: number }[]
  capabilities: { total: number; bits: number[] }
  avg_build_volume: { x: number; y: number; z: number }
  avg_counts: { fans: number; sensors: number; macros: number }
}

export interface EngagementData {
  panel_time: { panel: string; total_time_sec: number }[]
  panel_visits: { panel: string; total_visits: number }[]
  session_duration_trend: { date: string; avg_session_sec: number }[]
  themes: { name: string; count: number }[]
  locales: { name: string; count: number }[]
  brightness: { p25: number; p50: number; p75: number }
}

export interface ReliabilityData {
  uptime_trend: { date: string; avg_uptime_pct: number }[]
  disconnect_trend: { date: string; avg_disconnects: number }[]
  max_disconnect_sec: number
  error_categories: { category: string; count: number }[]
  error_codes: { category: string; code: string; count: number }[]
}

export interface PrintStartData {
  slicers: { name: string; count: number }[]
  file_size_buckets: { name: string; count: number }[]
  thumbnail_rate: number
  ams_rate: number
  sources: { name: string; count: number }[]
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

  getHardware(queryString: string): Promise<HardwareData> {
    return apiFetch(`/v1/dashboard/hardware?${queryString}`)
  },

  getEngagement(queryString: string): Promise<EngagementData> {
    return apiFetch(`/v1/dashboard/engagement?${queryString}`)
  },

  getReliability(queryString: string): Promise<ReliabilityData> {
    return apiFetch(`/v1/dashboard/reliability?${queryString}`)
  }
}
