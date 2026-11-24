// SPDX-License-Identifier: GPL-3.0-or-later
// Turns the Health payload and the update-failure groups into findings: what is worth
// an alert, a GitHub issue, or only a line in the weekly report. Pure; the caller
// supplies the ISO week that scopes the findings that repeat weekly.

import type { HealthPayload } from '../health'
import { fingerprint, type FindingKind } from './fingerprint'
import {
  CLUSTER_ALERT_DEVICES,
  CLUSTER_ISSUE_DEVICES,
  ERROR_ISSUE_DEVICES,
  RELEASE_REGRESSION_CRASH_FREE_DROP,
  RELEASE_REGRESSION_MIN_DEVICES,
  UPDATE_FAILURE_ISSUE_DEVICES,
} from './thresholds'

export type Action = 'alert' | 'issue' | 'report'

export interface Finding {
  kind: FindingKind
  fingerprint: string
  /** Every finding carries 'report'. */
  actions: Action[]
  /** Issue title and alert line; carries no device ids. */
  title: string
  /** The counts the email and the issue body show. */
  summary: Record<string, string | number>
  /** Dashboard route with the query that narrows it to this finding. */
  dashboardPath: string
}

export interface UpdateFailureGroup {
  platform: string
  reason: string
  failures: number
  devices: number
}

/** A finding plus the device count it is ordered by. */
interface Ranked {
  finding: Finding
  devices: number
}

/** Crash-free rates are fractions; compare at a precision where 0.97 - 0.95 equals 0.02. */
const RATE_SCALE = 1e6

const query = (params: Record<string, string>): string => {
  const q = new URLSearchParams(params).toString()
  return q ? `?${q}` : ''
}

function actionsFor(alert: boolean, issue: boolean): Action[] {
  return [...(alert ? (['alert'] as const) : []), ...(issue ? (['issue'] as const) : []), 'report']
}

type CrashRow = HealthPayload['crash_clusters'][number]

/**
 * One finding per (signal, fault, version). The payload splits a cluster by board, and
 * a device reports one board, so the per-board device counts are disjoint and sum
 * exactly. The aggregate is new only when no board saw it last week.
 */
function crashFindings(health: HealthPayload): Ranked[] {
  const groups = new Map<string, CrashRow[]>()
  for (const c of health.crash_clusters) {
    const fp = fingerprint('crash_cluster', [c.signal, c.fault_code, c.version])
    groups.set(fp, [...(groups.get(fp) ?? []), c])
  }
  return [...groups].map(([fp, rows]) => {
    const { signal, fault_code, version } = rows[0]
    const sum = (pick: (c: CrashRow) => number): number => rows.reduce((n, c) => n + pick(c), 0)
    const devices = sum((c) => c.devices)
    const prev = sum((c) => c.prev)
    const boards = [...new Set(rows.map((c) => c.board))].sort()
    return {
      devices,
      finding: {
        kind: 'crash_cluster',
        fingerprint: fp,
        actions: actionsFor(devices >= CLUSTER_ALERT_DEVICES && prev === 0, devices >= CLUSTER_ISSUE_DEVICES),
        title: `Crash ${signal} ${fault_code} in ${version}`,
        summary: {
          signal,
          fault_code,
          version,
          boards: boards.join(', '),
          crashes: sum((c) => c.cur),
          previous_crashes: prev,
          devices,
        },
        dashboardPath: `/stability${query({ version, board: boards.join(',') })}`,
      },
    }
  })
}

function errorFindings(health: HealthPayload): Ranked[] {
  return health.errors.map((e) => ({
    devices: e.devices,
    finding: {
      kind: 'error_code',
      fingerprint: fingerprint('error_code', [e.category, e.code]),
      actions: actionsFor(false, e.status !== 'steady' && e.devices >= ERROR_ISSUE_DEVICES),
      title: `Error ${e.category} ${e.code} ${e.status}`,
      summary: {
        category: e.category,
        code: e.code,
        status: e.status,
        count: e.cur,
        previous_count: e.prev,
        devices: e.devices,
        // A hang's count above is the app-blocked share; these are this week's whole.
        ...(e.split
          ? { hangs_app_blocked: e.split.app_blocked, hangs_host_out_of_memory: e.split.host_memory, hangs_cause_unknown: e.split.unknown }
          : {}),
      },
      dashboardPath: '/stability',
    },
  }))
}

function updateFindings(groups: UpdateFailureGroup[]): Ranked[] {
  return groups.map((g) => ({
    devices: g.devices,
    finding: {
      kind: 'update_failure',
      fingerprint: fingerprint('update_failure', [g.platform, g.reason]),
      actions: actionsFor(false, g.devices >= UPDATE_FAILURE_ISSUE_DEVICES),
      title: `Update failures on ${g.platform}: ${g.reason}`,
      summary: { platform: g.platform, reason: g.reason, failures: g.failures, devices: g.devices },
      dashboardPath: `/releases${query({ platform: g.platform })}`,
    },
  }))
}

function releaseFindings(health: HealthPayload, week: string): Ranked[] {
  const out: Ranked[] = []
  for (const r of health.releases) {
    const cur = r.cur.crash_free
    const prev = r.prev.crash_free
    if (r.cur.devices < RELEASE_REGRESSION_MIN_DEVICES || cur === null || prev === null) continue
    const drop = Math.round((prev - cur) * RATE_SCALE)
    if (drop <= Math.round(RELEASE_REGRESSION_CRASH_FREE_DROP * RATE_SCALE)) continue
    out.push({
      devices: r.cur.devices,
      finding: {
        kind: 'release_regression',
        fingerprint: fingerprint('release_regression', [r.version, week]),
        actions: actionsFor(true, false),
        title: `Crash-free rate of ${r.version} dropped`,
        summary: {
          version: r.version,
          crash_free: cur,
          previous_crash_free: prev,
          devices: r.cur.devices,
        },
        dashboardPath: `/releases${query({ version: r.version })}`,
      },
    })
  }
  return out
}

/** The payload carries no date for a kind's latest day, so the week scopes the alert. */
function dataHealthFindings(health: HealthPayload, week: string): Ranked[] {
  return health.data_health
    .filter((d) => d.low)
    .map((d) => ({
      devices: 0,
      finding: {
        kind: 'data_health',
        fingerprint: fingerprint('data_health', [d.kind, week]),
        actions: actionsFor(true, false),
        title: `Ingest of ${d.kind} is low`,
        summary: { kind: d.kind, latest: d.latest, median: d.median },
        dashboardPath: '/',
      },
    }))
}

const rank = (f: Finding): number => (f.actions.includes('alert') ? 0 : f.actions.includes('issue') ? 1 : 2)

/** Alerts first, then issues, then the rest; within each, more devices first. */
export function findingsFromHealth(health: HealthPayload, updateGroups: UpdateFailureGroup[], week: string): Finding[] {
  return [
    ...crashFindings(health),
    ...errorFindings(health),
    ...updateFindings(updateGroups),
    ...releaseFindings(health, week),
    ...dataHealthFindings(health, week),
  ]
    .sort((a, b) => rank(a.finding) - rank(b.finding) || b.devices - a.devices)
    .map((r) => r.finding)
}
