// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, it, expect } from 'vitest'
import * as T from '../loop/thresholds'
import { findingsFromHealth, type UpdateFailureGroup } from '../loop/findings'
import type { HealthPayload, ReleaseWindow } from '../health'
import { updateFailureGroupsQuery, UPDATE_DEFERRED_REASON } from '../queries'

const WEEK = '2026-W42'

const win = (devices: number, crashFree: number | null): ReleaseWindow => ({
  devices,
  update_failures: 0,
  update_successes: 0,
  crash_free: crashFree,
  crash_rate: null,
  update_failure_rate: null,
  print_success: null,
})

function health(over: Partial<HealthPayload> = {}): HealthPayload {
  return {
    fleet: { actives_cur: 0, actives_prev: 0, cdn_sources: null, cdn_date: null },
    releases: [],
    errors: [],
    crash_clusters: [],
    data_health: [],
    not_applicable: { crashes: null, errors: null, prints: null, updates: null },
    ...over,
  }
}

const cluster = (devices: number, prev = 0, version = '1.0.4', board = 'rpi4') => ({
  signal: 'SIGSEGV',
  fault_code: 'SEGV_MAPERR',
  version,
  board,
  cur: devices * 2,
  prev,
  devices,
})

const run = (h: HealthPayload, groups: UpdateFailureGroup[] = []) => findingsFromHealth(h, groups, WEEK)

describe('crash clusters', () => {
  it('alerts and files at the device threshold, new this week', () => {
    const f = run(health({ crash_clusters: [cluster(T.CLUSTER_ALERT_DEVICES)] }))
    expect(f).toHaveLength(1)
    expect(f[0].kind).toBe('crash_cluster')
    expect(f[0].fingerprint).toBe('crash_cluster:sigsegv|segv_maperr|1.0.4')
    expect(f[0].actions).toEqual(expect.arrayContaining(['alert', 'issue', 'report']))
    expect(f[0].dashboardPath).toBe('/stability?version=1.0.4&board=rpi4')
  })

  it('does nothing one device below the thresholds, but still reports', () => {
    const f = run(health({ crash_clusters: [cluster(T.CLUSTER_ALERT_DEVICES - 1)] }))
    expect(f).toHaveLength(1)
    expect(f[0].actions).toEqual(['report'])
  })

  it('sums one crash across boards into one finding that crosses the thresholds', () => {
    const half = Math.ceil(T.CLUSTER_ISSUE_DEVICES / 2)
    const f = run(health({ crash_clusters: [cluster(half, 0, '1.0.4', 'rpi4'), cluster(half, 0, '1.0.4', 'rpi5')] }))
    expect(f).toHaveLength(1)
    expect(f[0].actions).toEqual(['alert', 'issue', 'report'])
    expect(f[0].summary).toMatchObject({ boards: 'rpi4, rpi5', devices: half * 2, crashes: half * 4, previous_crashes: 0 })
    expect(f[0].dashboardPath).toBe('/stability?version=1.0.4&board=rpi4%2Crpi5')
  })

  it('makes one finding per fingerprint, whatever the spelling of its parts', () => {
    const rows = [cluster(1, 0, '1.0.4', 'a'), { ...cluster(1, 0, '1.0.4', 'b'), signal: ' sigsegv ' }, cluster(1, 0, '1.0.5', 'a')]
    const f = run(health({ crash_clusters: rows }))
    expect(f.map((x) => x.fingerprint).sort()).toEqual([
      'crash_cluster:sigsegv|segv_maperr|1.0.4',
      'crash_cluster:sigsegv|segv_maperr|1.0.5',
    ])
  })

  it('does not alert an aggregate one of whose boards crashed last week', () => {
    const f = run(health({ crash_clusters: [cluster(T.CLUSTER_ALERT_DEVICES, 0, '1.0.4', 'rpi4'), cluster(1, 2, '1.0.4', 'rpi5')] }))
    expect(f).toHaveLength(1)
    expect(f[0].actions).toEqual(['issue', 'report'])
  })

  it('does not alert a cluster that crashed last week, but still files it', () => {
    const f = run(health({ crash_clusters: [cluster(T.CLUSTER_ALERT_DEVICES, 4)] }))
    expect(f[0].actions).toEqual(expect.arrayContaining(['issue', 'report']))
    expect(f[0].actions).not.toContain('alert')
  })
})

describe('error codes', () => {
  const err = (devices: number, status: 'new' | 'rising' | 'steady') => ({
    category: 'Network',
    code: 'E42',
    cur: 10,
    prev: 1,
    devices,
    status,
  })

  it.each(['new', 'rising'] as const)('files a %s error at the device threshold', (status) => {
    const f = run(health({ errors: [err(T.ERROR_ISSUE_DEVICES, status)] }))
    expect(f).toHaveLength(1)
    expect(f[0].kind).toBe('error_code')
    expect(f[0].fingerprint).toBe('error_code:network|e42')
    expect(f[0].actions).toContain('issue')
  })

  it('does not file one device below the threshold', () => {
    const f = run(health({ errors: [err(T.ERROR_ISSUE_DEVICES - 1, 'new')] }))
    expect(f.flatMap((x) => x.actions)).not.toContain('issue')
  })

  it('never files a steady error, however many devices', () => {
    const f = run(health({ errors: [err(T.ERROR_ISSUE_DEVICES * 10, 'steady')] }))
    expect(f.flatMap((x) => x.actions)).not.toContain('issue')
  })

  it('carries every cause of a hang split in the summary, next to the app-blocked count', () => {
    const hang: HealthPayload['errors'][number] = {
      category: 'ui',
      code: 'main_loop_hang',
      cur: 2,
      prev: 0,
      devices: 2,
      status: 'new',
      split: { suspended: 1, host_memory: 3, app_blocked: 2, unknown: 1 },
      total: 7,
    }
    const f = run(health({ errors: [hang] }))
    expect(f[0].summary).toMatchObject({
      count: 2,
      hangs_app_blocked: 2,
      hangs_host_out_of_memory: 3,
      hangs_app_suspended: 1,
      hangs_cause_unknown: 1,
    })
  })
})

describe('update failures', () => {
  const grp = (devices: number): UpdateFailureGroup => ({
    platform: 'Pi',
    reason: 'Download_Failed',
    failures: devices * 3,
    devices,
  })

  it('files at the device threshold', () => {
    const f = run(health(), [grp(T.UPDATE_FAILURE_ISSUE_DEVICES)])
    expect(f).toHaveLength(1)
    expect(f[0].kind).toBe('update_failure')
    expect(f[0].fingerprint).toBe('update_failure:pi|download_failed')
    expect(f[0].actions).toEqual(expect.arrayContaining(['issue', 'report']))
  })

  it('does not file one device below it', () => {
    const f = run(health(), [grp(T.UPDATE_FAILURE_ISSUE_DEVICES - 1)])
    expect(f.flatMap((x) => x.actions)).not.toContain('issue')
  })
})

describe('release regression', () => {
  const rel = (devices: number, cur: number | null, prev: number | null) => ({
    version: '1.0.4',
    cur: win(devices, cur),
    prev: win(100, prev),
  })

  it('alerts when the drop exceeds the threshold', () => {
    const f = run(health({ releases: [rel(T.RELEASE_REGRESSION_MIN_DEVICES, 0.949, 0.97)] }))
    expect(f).toHaveLength(1)
    expect(f[0].kind).toBe('release_regression')
    expect(f[0].fingerprint).toBe(`release_regression:1.0.4|${WEEK.toLowerCase()}`)
    expect(f[0].actions).toEqual(expect.arrayContaining(['alert', 'report']))
    expect(f[0].dashboardPath).toBe('/releases?version=1.0.4')
  })

  it('does not alert on a drop of exactly the threshold', () => {
    expect(run(health({ releases: [rel(T.RELEASE_REGRESSION_MIN_DEVICES, 0.95, 0.97)] }))).toEqual([])
  })

  it('alerts a drop just over the threshold', () => {
    expect(run(health({ releases: [rel(T.RELEASE_REGRESSION_MIN_DEVICES, 0.9499, 0.97)] }))).toHaveLength(1)
  })

  it('needs the minimum device count', () => {
    expect(run(health({ releases: [rel(T.RELEASE_REGRESSION_MIN_DEVICES - 1, 0.5, 0.99)] }))).toEqual([])
  })

  it.each([
    [null, 0.97],
    [0.5, null],
  ])('ignores a null crash_free (%s, %s)', (cur, prev) => {
    expect(run(health({ releases: [rel(T.RELEASE_REGRESSION_MIN_DEVICES, cur, prev)] }))).toEqual([])
  })

  it('ignores an improvement', () => {
    expect(run(health({ releases: [rel(T.RELEASE_REGRESSION_MIN_DEVICES, 0.99, 0.9)] }))).toEqual([])
  })
})

describe('data health', () => {
  it('alerts for every low kind and none for a healthy one', () => {
    const f = run(
      health({
        data_health: [
          { kind: 'session', latest: 1, median: 10, low: true },
          { kind: 'crash', latest: 9, median: 10, low: false },
        ],
      }),
    )
    expect(f).toHaveLength(1)
    expect(f[0].kind).toBe('data_health')
    expect(f[0].fingerprint).toBe(`data_health:session|${WEEK.toLowerCase()}`)
    expect(f[0].actions).toEqual(expect.arrayContaining(['alert', 'report']))
  })
})

describe('ordering and safety', () => {
  it('puts alerts first, then issues, then more devices first', () => {
    const f = run(
      health({
        crash_clusters: [cluster(9, 5, '1.0.1'), cluster(4, 0, '1.0.2'), cluster(7, 5, '1.0.3'), cluster(2, 0, '1.0.5')],
      }),
    )
    expect(f.map((x) => x.fingerprint.split('|')[2])).toEqual(['1.0.2', '1.0.1', '1.0.3', '1.0.5'])
  })

  it('every finding carries report and none contains a 64-hex string', () => {
    const f = run(
      health({
        crash_clusters: [cluster(5)],
        errors: [{ category: 'c', code: 'x', cur: 9, prev: 0, devices: 9, status: 'new' }],
        data_health: [{ kind: 'k', latest: 0, median: 9, low: true }],
      }),
      [{ platform: 'p', reason: 'r', failures: 9, devices: 9 }],
    )
    expect(f.length).toBeGreaterThan(3)
    for (const x of f) {
      expect(x.actions).toContain('report')
      expect(JSON.stringify(x)).not.toMatch(/[0-9a-f]{64}/i)
    }
  })
})

describe('updateFailureGroupsQuery', () => {
  const sql = updateFailureGroupsQuery([0, 7])

  it('excludes the deferred-for-print reason by the shared constant', () => {
    expect(sql).toContain(`!= '${UPDATE_DEFERRED_REASON}'`)
    expect(UPDATE_DEFERRED_REASON).toBe('print_in_progress')
  })

  it('groups update_failed rows by platform and reason over the window', () => {
    expect(sql).toContain("index1 = 'update_failed'")
    expect(sql).toContain('GROUP BY platform, reason')
    expect(sql).toContain('count(DISTINCT')
    expect(sql).toContain("INTERVAL '7' DAY")
  })
})
