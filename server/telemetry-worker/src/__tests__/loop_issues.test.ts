// SPDX-License-Identifier: GPL-3.0-or-later
import { describe, it, expect } from 'vitest'
import { issueBody, issueLabels, syncIssues } from '../loop/issues'
import { findingsFromHealth, type Finding } from '../loop/findings'
import type { HealthPayload } from '../health'
import { fingerprintInBody, markerFor } from '../loop/fingerprint'
import { NEW_ISSUES_PER_RUN } from '../loop/thresholds'
import type { LoopState } from '../loop/state'
import { FakeTracker } from './loop_fakes'

const URL = 'https://dash.test'
const W1 = '2026-W42'
const W2 = '2026-W43'

const health: HealthPayload = {
  fleet: { actives_cur: 0, actives_prev: 0, cdn_sources: null, cdn_date: null },
  releases: [],
  errors: [],
  crash_clusters: [],
  data_health: [],
  not_applicable: { crashes: null, errors: null, prints: null, updates: null },
}

function finding(n: number, actions: Finding['actions'] = ['issue', 'report']): Finding {
  return {
    kind: 'crash_cluster',
    fingerprint: `crash_cluster:sigsegv|0x${n}|1.0.${n}`,
    actions,
    title: `Crash ${n}`,
    summary: { devices: 4, crashes: 9 },
    dashboardPath: '/stability?version=1.0.1',
  }
}
const fresh = (): LoopState => ({ alerts: {}, issueComments: {} })

describe('issueBody', () => {
  it('starts with the marker and carries no 64-hex string', () => {
    const f = finding(1)
    const body = issueBody(f, URL, W1, 'acme/widgets')
    expect(body.split('\n')[0]).toBe(markerFor(f.fingerprint))
    expect(fingerprintInBody(body)).toBe(f.fingerprint)
    expect(body).toContain(`${URL}/stability?version=1.0.1`)
    expect(body).toContain(W1)
    expect(body).not.toMatch(/[0-9a-f]{64}/i)
  })

  it('renders device-supplied values as escaped code spans, one table row each', () => {
    const f = { ...finding(1), summary: { board: 'pi|4\n| x | y |', version: '@octocat', devices: 4 } }
    const body = issueBody(f, URL, W1, 'acme/widgets')
    expect(body).toContain('| board | `pi\\|4 \\| x \\| y \\|` |')
    expect(body).toContain('| version | `@\u200boctocat` |')
    expect(body).toContain('| devices | 4 |')
    expect(body).not.toMatch(/@octocat/)
  })
})

describe('issueBody crash-report link', () => {
  const crash = { ...finding(1), summary: { signal: 'SIGSEGV', fault_code: 'SEGV_MAPERR', version: '1.1.0-beta.9', devices: 4 } }

  it('names the crash worker fingerprint prefix and links a search for it', () => {
    const body = issueBody(crash, URL, W1, 'acme/widgets')
    expect(body).toContain('`SIGSEGV/1.1.0-beta.9/`')
    const link = body.match(/<(https:\/\/github\.com\/acme\/widgets\/issues\?q=[^>]+)>/)
    expect(link).not.toBeNull()
    expect(new URLSearchParams(link![1].split('?')[1]).get('q')).toBe('is:issue label:crash "Fingerprint: SIGSEGV/1.1.0-beta.9/" in:body')
  })

  it('is absent from other kinds', () => {
    const body = issueBody({ ...crash, kind: 'error_code' }, URL, W1, 'acme/widgets')
    expect(body).not.toContain('Fingerprint:')
    expect(body).not.toContain('github.com')
  })

  it('is part of the body syncIssues files, with the tracker repo', async () => {
    const t = new FakeTracker()
    await syncIssues([crash], t, fresh(), W1, URL, false)
    expect(t.callsOf('create')[0].body).toContain(`https://github.com/${t.repo}/issues?q=`)
  })
})

describe('issueBody hang causes', () => {
  it('names every cause the Health split counts of a main_loop_hang', () => {
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
    const [f] = findingsFromHealth({ ...health, errors: [hang] }, [], W1)
    const body = issueBody(f, URL, W1, 'acme/widgets')
    for (const row of ['| hangs_app_blocked | 2 |', '| hangs_host_out_of_memory | 3 |', '| hangs_app_suspended | 1 |', '| hangs_cause_unknown | 1 |']) {
      expect(body).toContain(row)
    }
  })
})

describe('issueLabels', () => {
  it('maps each kind to its label', () => {
    expect(issueLabels('crash_cluster')).toEqual(['telemetry', 'crash'])
    expect(issueLabels('error_code')).toEqual(['telemetry', 'error-code'])
    expect(issueLabels('update_failure')).toEqual(['telemetry', 'update-failure'])
    expect(issueLabels('printer')).toEqual(['telemetry', 'printer-db'])
  })
})

describe('syncIssues', () => {
  it('handles a fingerprint once per run', async () => {
    const t = new FakeTracker()
    const out = await syncIssues([finding(1), { ...finding(1), title: 'twin' }], t, fresh(), W1, URL, false)
    expect(out.map((o) => o.action)).toEqual(['created'])
    expect(t.callsOf('create')).toHaveLength(1)
  })

  it('files the title through the title escaper', async () => {
    const t = new FakeTracker()
    await syncIssues([{ ...finding(1), title: 'Crash @octocat\nin 1.0' }], t, fresh(), W1, URL, false)
    expect(t.callsOf('create')[0].title).toBe('Crash @\u200boctocat in 1.0')
  })

  it('creates an issue for a new finding and records the week', async () => {
    const t = new FakeTracker()
    const state = fresh()
    const f = finding(1)
    const out = await syncIssues([f], t, state, W1, URL, false)
    expect(out).toEqual([{ fingerprint: f.fingerprint, action: 'created', issue: { number: 1, url: expect.any(String) } }])
    expect(t.callsOf('create')[0].labels).toEqual(['telemetry', 'crash'])
    expect(state.issueComments[f.fingerprint]).toBe(W1)
  })

  it('ignores findings without the issue action', async () => {
    const t = new FakeTracker()
    expect(await syncIssues([finding(1, ['report'])], t, fresh(), W1, URL, false)).toEqual([])
    expect(t.calls).toEqual([])
  })

  it('comments on an open issue not yet commented this week', async () => {
    const t = new FakeTracker()
    const f = finding(1)
    t.seed(markerFor(f.fingerprint))
    const state: LoopState = { alerts: {}, issueComments: { [f.fingerprint]: W1 } }
    const out = await syncIssues([f], t, state, W2, URL, false)
    expect(out[0].action).toBe('commented')
    expect(t.callsOf('comment')).toHaveLength(1)
    expect(state.issueComments[f.fingerprint]).toBe(W2)
  })

  it('a second run in the same week is unchanged with no second comment', async () => {
    const t = new FakeTracker()
    const state = fresh()
    const f = finding(1)
    await syncIssues([f], t, state, W1, URL, false)
    const out = await syncIssues([f], t, state, W1, URL, false)
    expect(out[0].action).toBe('unchanged')
    expect(t.callsOf('create')).toHaveLength(1)
    expect(t.callsOf('comment')).toHaveLength(0)
  })

  it('the same finding two weeks running updates one issue', async () => {
    const t = new FakeTracker()
    const state = fresh()
    const f = finding(1)
    await syncIssues([f], t, state, W1, URL, false)
    await syncIssues([f], t, state, W2, URL, false)
    expect(t.callsOf('create')).toHaveLength(1)
    expect(t.callsOf('comment')).toHaveLength(1)
  })

  it('reopens a closed issue with a returned comment and never duplicates it', async () => {
    const t = new FakeTracker()
    const f = finding(1)
    const seeded = t.seed(markerFor(f.fingerprint), 'closed')
    const state = fresh()
    const out = await syncIssues([f], t, state, W1, URL, false)
    expect(out[0]).toMatchObject({ action: 'reopened', issue: { number: seeded.number } })
    expect(t.callsOf('reopen')).toEqual([{ op: 'reopen', issue: seeded.number }])
    expect(t.callsOf('comment')[0].body).toContain('returned')
    expect(t.callsOf('create')).toHaveLength(0)
    expect(state.issueComments[f.fingerprint]).toBe(W1)
  })

  it('files at most NEW_ISSUES_PER_RUN new issues and holds the rest', async () => {
    const t = new FakeTracker()
    const fs = Array.from({ length: NEW_ISSUES_PER_RUN + 1 }, (_, i) => finding(i))
    const out = await syncIssues(fs, t, fresh(), W1, URL, false)
    expect(out.filter((o) => o.action === 'created')).toHaveLength(NEW_ISSUES_PER_RUN)
    expect(out[NEW_ISSUES_PER_RUN]).toEqual({ fingerprint: fs[NEW_ISSUES_PER_RUN].fingerprint, action: 'held' })
    expect(t.callsOf('create')).toHaveLength(NEW_ISSUES_PER_RUN)
  })

  it('updates to existing issues are not counted against the cap', async () => {
    const t = new FakeTracker()
    const fs = Array.from({ length: NEW_ISSUES_PER_RUN + 1 }, (_, i) => finding(i))
    t.seed(markerFor(fs[0].fingerprint))
    const out = await syncIssues(fs, t, fresh(), W1, URL, false)
    expect(out.filter((o) => o.action === 'held')).toHaveLength(0)
  })

  it('a dry run makes no write calls and leaves state alone but reports the same actions', async () => {
    const mk = () => {
      const t = new FakeTracker()
      const fs = Array.from({ length: NEW_ISSUES_PER_RUN + 3 }, (_, i) => finding(i))
      t.seed(markerFor(fs[0].fingerprint))
      t.seed(markerFor(fs[1].fingerprint), 'closed')
      return { t, fs }
    }
    const live = mk()
    const dry = mk()
    const state = fresh()
    const liveOut = await syncIssues(live.fs, live.t, fresh(), W1, URL, false)
    const dryOut = await syncIssues(dry.fs, dry.t, state, W1, URL, true)
    expect(dryOut.map((o) => [o.fingerprint, o.action])).toEqual(liveOut.map((o) => [o.fingerprint, o.action]))
    for (const op of ['create', 'comment', 'reopen'] as const) expect(dry.t.callsOf(op)).toHaveLength(0)
    expect(state.issueComments).toEqual({})
  })

  it('holds a finding with the error when GitHub fails and continues', async () => {
    const t = new FakeTracker()
    t.failWith = new Error('boom')
    const fs = [finding(1), finding(2)]
    const out = await syncIssues(fs, t, fresh(), W1, URL, false)
    expect(out).toEqual(fs.map((f) => ({ fingerprint: f.fingerprint, action: 'held', error: 'boom' })))
  })

  it('a failure on one finding does not stop the next', async () => {
    const t = new FakeTracker()
    const fs = [finding(1), finding(2)]
    const orig = t.create.bind(t)
    let n = 0
    t.create = async (...a) => {
      if (n++ === 0) throw new Error('first fails')
      return orig(...a)
    }
    const state = fresh()
    const out = await syncIssues(fs, t, state, W1, URL, false)
    expect(out.map((o) => o.action)).toEqual(['held', 'created'])
    expect(state.issueComments[fs[0].fingerprint]).toBeUndefined()
  })

  it('reports a created issue that came back without the telemetry label', async () => {
    const t = new FakeTracker()
    const orig = t.create.bind(t)
    t.create = async (title, body) => ({ ...(await orig(title, body, ['crash'])), labels: ['crash'] })
    const f = finding(1)
    const out = await syncIssues([f], t, fresh(), W1, URL, false)
    expect(out).toEqual([
      { fingerprint: f.fingerprint, action: 'created', issue: { number: 1, url: expect.any(String) }, error: expect.stringMatching(/telemetry label/) },
    ])
  })
})
