import type { DashFilterKey } from './urlState'
import type { HangSplit } from '../../../telemetry-worker/src/hang_cause'

/** The DeltaEra fields these formatters read; services/api's DeltaEra satisfies it. */
interface EraFields {
  delta_era_devices: number
  filter_not_applicable: DashFilterKey | null
}

export function formatDuration(seconds: number): string {
  if (!seconds) return '0s'
  if (seconds < 60) return `${Math.round(seconds)}s`
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  const s = Math.round(seconds % 60)
  if (h > 0) return m > 0 ? `${h}h ${m}m` : `${h}h`
  return s > 0 ? `${m}m ${s}s` : `${m}m`
}

export function formatTimestamp(ts: string): string {
  try {
    const d = new Date(ts)
    return d.toLocaleDateString('en-US', { month: 'short', day: 'numeric' }) +
      ' ' + d.toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit', hour12: false })
  } catch {
    return ts
  }
}

export function shortDeviceId(id: string): string {
  if (!id) return '\u2014'
  return id.slice(0, 8) + '...'
}

export function titleCase(snakeId: string): string {
  return snakeId.split('_').map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ')
}

const FILTER_LABEL: Record<DashFilterKey, string> = {
  platform: 'platform',
  version: 'version',
  model: 'printer model',
  board: 'board',
}

/** What a block shows when an active filter names a field its rows do not carry. */
export function notReportedPer(key: DashFilterKey): string {
  return `Not reported per ${FILTER_LABEL[key]}`
}

/**
 * One line naming the table columns an active filter leaves unreported, grouped
 * by filter: "Crash-free, Crash rate: Not reported per printer model". Empty
 * when every column is reported.
 */
export function unreportedNote(columns: [label: string, key: DashFilterKey | null][]): string {
  const byKey = new Map<DashFilterKey, string[]>()
  for (const [label, key] of columns) if (key) byKey.set(key, [...(byKey.get(key) ?? []), label])
  return [...byKey].map(([key, labels]) => `${labels.join(', ')}: ${notReportedPer(key)}`).join('; ')
}

/** What a view shows in place of a delta-era value it has no number for. */
export function deltaEraPlaceholder(era: EraFields): string {
  if (era.filter_not_applicable) return notReportedPer(era.filter_not_applicable)
  return `Waiting for devices on the new client (${era.delta_era_devices} so far)`
}

/**
 * A 0-1 rate as a percentage, or "n/a" when the filters make it inapplicable
 * or a payload from an older worker leaves it out.
 */
export function formatRate(rate: number | null, digits = 1): string {
  return rate == null || !Number.isFinite(rate) ? 'n/a' : `${(rate * 100).toFixed(digits)}%`
}

/**
 * A rate with the counts it rests on, e.g. "100.0% (1/1)"; "n/a" when the
 * rate is null, and the rate alone when a worker older than the dashboard
 * sent no counts.
 */
export function formatRateOf(rate: number | null, numerator: number, denominator: number, digits = 1): string {
  if (rate == null || !Number.isFinite(rate) || !Number.isFinite(numerator) || !Number.isFinite(denominator)) return formatRate(rate, digits)
  const n = (v: number) => Math.round(v).toLocaleString()
  return `${formatRate(rate, digits)} (${n(numerator)}/${n(denominator)})`
}

/** A change smaller than this many percentage points reads as no change. */
const PP_ZERO = 0.05

/**
 * The change from `prev` to `cur`, two 0-1 rates, in percentage points: its
 * label ("↑ +1.2pp", "↓ -0.4pp", "= 0.0pp") and its tone given which direction
 * is good. Null when either rate is null.
 */
export function ppChange(
  cur: number | null,
  prev: number | null,
  upIsGood: boolean
): { text: string; tone: 'good' | 'bad' | 'flat' } | null {
  if (cur === null || prev === null) return null
  const pp = (cur - prev) * 100
  if (Math.abs(pp) < PP_ZERO) return { text: '= 0.0pp', tone: 'flat' }
  return {
    text: `${pp > 0 ? '↑ +' : '↓ '}${pp.toFixed(1)}pp`,
    tone: pp > 0 === upIsGood ? 'good' : 'bad'
  }
}

/** The filter window as a caption phrase: '30d' is "last 30 days". */
export function rangeLabel(range: string): string {
  const days = Number(range.replace(/d$/, ''))
  if (!Number.isFinite(days) || days <= 0) return range
  return days === 1 ? 'last day' : `last ${days} days`
}

/**
 * What a section shows when its endpoint returned no rows: the filter the
 * endpoint says it cannot apply, else whether any filter is active.
 */
export function emptyPlaceholder(era: EraFields | null, activeFilters: DashFilterKey[]): string {
  if (era?.filter_not_applicable) return deltaEraPlaceholder(era)
  return activeFilters.length > 0 ? 'No data for this filter' : 'No data in this range'
}

export function formatMB(kb: number): string {
  return `${(kb / 1024).toFixed(1)} MB`
}

/** What each cause of a main_loop_hang reads as under the code, in the order the Health tab lists them. */
const HANG_CAUSE_LABEL: Record<keyof HangSplit, string> = {
  app_blocked: 'app blocked',
  host_memory: 'host out of memory',
  suspended: 'app suspended',
  unknown: 'unknown'
}

/**
 * One line counting every cause of a hang split under its label, e.g. "3 app blocked,
 * 7 host out of memory, 0 app suspended, 0 unknown". A stored weekly report may predate
 * a cause, so one it lacks reads as 0.
 */
export function hangCauseLine(split: HangSplit): string {
  return (Object.keys(HANG_CAUSE_LABEL) as (keyof HangSplit)[])
    .map((cause) => `${(split[cause] ?? 0).toLocaleString()} ${HANG_CAUSE_LABEL[cause]}`)
    .join(', ')
}
