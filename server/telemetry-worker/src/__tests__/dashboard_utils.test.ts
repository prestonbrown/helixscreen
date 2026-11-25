// SPDX-License-Identifier: GPL-3.0-or-later
import { afterEach, describe, expect, it, vi } from 'vitest'
import { groupVersionRates, groupVersions, olderLines, ungrouped, versionChips } from '../../../analytics-dashboard/src/utils/versions'
import { planActivation, rowAction, rowHint, rowLabel, single } from '../../../analytics-dashboard/src/utils/drill'
import { mergeOptions, rateAxis } from '../../../analytics-dashboard/src/utils/chart'
import { emptyPlaceholder, formatRate, formatRateOf, hangCauseLine, notReportedPer, ppChange, rangeLabel, unreportedNote } from '../../../analytics-dashboard/src/utils/format'
import { canonicalModelKey, canonicalModels, isFilterable } from '../../../analytics-dashboard/src/utils/models'
import { LOW_COUNT_PRINTS, groupMaterialSets, isLowCount, materialSet } from '../../../analytics-dashboard/src/utils/materials'
import { filterValues } from '../queries'
import type { HangSplit } from '../hang_cause'
import { withNotApplicable } from '../../../analytics-dashboard/src/utils/notApplicable'
import { isRenderError } from '../../../analytics-dashboard/src/utils/renderError'
import { barChartHeight, dimmed, seriesColor, todayUtc, trimPartialDays } from '../../../analytics-dashboard/src/utils/series'

const rows = (names: string[]) => names.map((name, i) => ({ name, count: i + 1 }))
const LIVE = ['1.1.0-beta.9', '1.1.0-beta.8', '1.0.5', '1.0.4', '1.0.0', '0.99.118', '0.99.30']

describe('groupVersions', () => {
  it('collapses betas of an absent release and old lines', () => {
    const out = groupVersions(rows(LIVE), new Set())
    expect(out.map((g) => g.label)).toEqual([
      '1.1.0 betas (2)',
      '1.0.5',
      '1.0.4',
      '1.0.0',
      '0.99.x (2 versions)'
    ])
    expect(out[0].key).toBe('betas:1.1.0')
    expect(out[0].members).toEqual(['1.1.0-beta.9', '1.1.0-beta.8'])
    expect(out[0].expandable).toBe(true)
    expect(out[4]).toMatchObject({ key: 'line:0.99', expandable: true, count: 6 + 7, members: ['0.99.118', '0.99.30'] })
  })

  it('shows betas individually once the release is present', () => {
    const out = groupVersions(rows([...LIVE, '1.1.0']), new Set())
    expect(out.map((g) => g.label)).toEqual([
      '1.1.0',
      '1.1.0-beta.9',
      '1.1.0-beta.8',
      '1.0.5',
      '1.0.4',
      '1.0.0',
      '0.99.x (2 versions)'
    ])
  })

  it('expands a collapsed line to a header and its members, newest first', () => {
    const out = groupVersions(rows(LIVE), new Set(['line:0.99']))
    expect(out.slice(-3).map((g) => [g.label, g.expandable, g.expanded ?? false])).toEqual([
      ['0.99.x ▾', true, true],
      ['0.99.118', false, false],
      ['0.99.30', false, false]
    ])
    expect(out[out.length - 3]).toMatchObject({ key: 'line:0.99', count: 6 + 7, members: ['0.99.118', '0.99.30'] })
  })

  it('expands a beta group under a header that collapses it', () => {
    const out = groupVersions(rows(LIVE), new Set(['betas:1.1.0']))
    expect(out.slice(0, 3).map((g) => g.label)).toEqual(['1.1.0 betas ▾', '1.1.0-beta.9', '1.1.0-beta.8'])
    expect(out[0]).toMatchObject({ key: 'betas:1.1.0', expandable: true, expanded: true })
    expect(rowAction(out[0])).toBe('expand')
    expect(planActivation([['version', out[0]]])).toEqual({ kind: 'expand', key: 'betas:1.1.0' })
    expect(rowHint(out[0])).toBe('click to collapse')
    expect(rowLabel(out[0])).toBe('1.1.0 betas ▾')
  })

  it('returns nothing for no rows', () => {
    expect(groupVersions([], new Set())).toEqual([])
  })
})

describe('canonicalModels', () => {
  it('folds case, separators and a trailing parenthetical', () => {
    const keys = ['creality_ender_3_v3_ke', 'Creality Ender 3 V3 KE', 'Creality Ender-3 V3 KE'].map(canonicalModelKey)
    expect(new Set(keys).size).toBe(1)
    expect(canonicalModelKey('FlashForge Adventurer 5M Pro (ForgeX)')).toBe(
      canonicalModelKey('FlashForge Adventurer 5M Pro')
    )
    expect(canonicalModelKey('K1')).toBe(canonicalModelKey('Creality K1'))
  })

  it('labels with the biggest spelling and keeps every member', () => {
    const out = canonicalModels([
      { name: 'K1', count: 3 },
      { name: 'Creality K1', count: 10 },
      { name: 'Unknown', count: 50 },
      { name: '', count: 1 }
    ])
    expect(out.map((g) => g.label)).toEqual(['Unknown', 'Creality K1', ''])
    expect(out[1]).toMatchObject({ key: 'model:creality k1', count: 13, members: ['K1', 'Creality K1'] })
  })
})

describe('isFilterable', () => {
  it('rejects values the worker would alter or split', () => {
    expect(isFilterable("CdO - Cubo dell'Odio")).toBe(false)
    expect(isFilterable('Voron 2.4')).toBe(true)
    expect(isFilterable('a,b')).toBe(false)
    expect(isFilterable(' Voron 2.4')).toBe(false)
    expect(isFilterable('Voron 2.4 ')).toBe(false)
    expect(isFilterable('')).toBe(false)
  })

  // Filterable exactly when the worker's parse of the value gives back the value itself.
  const workerKeeps = (raw: string) => {
    const parsed = filterValues(raw)
    return parsed.length === 1 && parsed[0] === raw
  }
  const printable = Array.from({ length: 95 }, (_, i) => String.fromCharCode(32 + i))
  const cases = [
    ...printable.map((c) => `a${c}b`),
    ...printable,
    'Voron 2.4', ' Voron', 'Voron ', '\tVoron', '', ',', 'a,b', "CdO - Cubo dell'Odio", 'Prusa MK4\u00e9', 'K1 (Max)/SE+',
  ]
  it.each(cases.map((c) => [JSON.stringify(c), c]))('agrees with the worker on %s', (_label, raw) => {
    expect(isFilterable(raw)).toBe(workerKeeps(raw))
  })
})

describe('materialSet', () => {
  it('normalizes lists', () => {
    expect(materialSet('PLA;PLA;PETG;PLA')).toEqual({ set: 'PETG + PLA', materials: ['PETG', 'PLA'], multi: true })
    expect(materialSet('pla ')).toEqual({ set: 'PLA', materials: ['PLA'], multi: false })
    expect(materialSet('')).toEqual({ set: '', materials: [], multi: false })
    expect(materialSet('PLA;;PLA').set).toBe('PLA')
  })
})

describe('groupMaterialSets', () => {
  it('merges spellings with a count-weighted rate', () => {
    const out = groupMaterialSets([
      { type: 'PLA;PLA', count: 72, success_rate: 0.9 },
      { type: 'PLA', count: 28, success_rate: 0.5 }
    ])
    expect(out).toHaveLength(1)
    expect(out[0]).toMatchObject({ key: 'set:PLA', label: 'PLA', count: 100, members: ['PLA;PLA', 'PLA'] })
    expect(out[0].success_rate).toBeCloseTo(0.788, 3)
  })
  it('gives a null rate, not 0, to a set with no prints', () => {
    expect(groupMaterialSets([{ type: 'PLA', count: 0, success_rate: 0 }])[0].success_rate).toBeNull()
  })
})

describe('trimPartialDays', () => {
  const day = (date: string) => ({ date })
  it('drops the first day and today', () => {
    const s = ['2026-10-10', '2026-10-11', '2026-10-12', '2026-10-13'].map(day)
    expect(trimPartialDays(s, '2026-10-13').map((p) => p.date)).toEqual(['2026-10-11', '2026-10-12'])
  })
  it('drops only the first day when today is absent', () => {
    const s = ['2026-10-10', '2026-10-11', '2026-10-12'].map(day)
    expect(trimPartialDays(s, '2026-10-20').map((p) => p.date)).toEqual(['2026-10-11', '2026-10-12'])
  })
  it('leaves nothing for one or two days', () => {
    expect(trimPartialDays(['2026-10-10', '2026-10-11'].map(day), '2026-10-11')).toEqual([])
    expect(trimPartialDays(['2026-10-10', '2026-10-11'].map(day), '2026-10-20')).toEqual([])
  })
})

describe('seriesColor and barChartHeight', () => {
  it('cycles a 12 color palette', () => {
    expect(seriesColor(0)).not.toBe(seriesColor(1))
    expect(seriesColor(12)).toBe(seriesColor(0))
  })
  it('sizes bar charts per row with a floor', () => {
    expect(barChartHeight(5)).toBe(300)
    expect(barChartHeight(56)).toBe(1292)
  })
})

describe('todayUtc', () => {
  afterEach(() => vi.useRealTimers())
  it('formats the UTC date', () => {
    vi.useFakeTimers()
    vi.setSystemTime(new Date('2026-10-15T23:59:59Z'))
    expect(todayUtc()).toBe('2026-10-15')
  })
})

describe('ungrouped', () => {
  it('gives each raw value its own filterable row', () => {
    expect(ungrouped([{ name: 'pi', count: 4 }])).toEqual([
      { key: 'pi', label: 'pi', count: 4, members: ['pi'], expandable: false }
    ])
  })
})

describe('rangeLabel', () => {
  it('names the window', () => {
    expect(rangeLabel('30d')).toBe('last 30 days')
    expect(rangeLabel('1d')).toBe('last day')
    expect(rangeLabel('bogus')).toBe('bogus')
  })
})

describe('emptyPlaceholder', () => {
  const era = { delta_era_devices: 3, filter_not_applicable: null }
  it('names a filter the endpoint cannot apply', () => {
    expect(emptyPlaceholder({ ...era, filter_not_applicable: 'model' }, ['model'])).toBe(
      'Not reported per printer model'
    )
  })
  it('blames an active filter when the endpoint does not say', () => {
    expect(emptyPlaceholder(null, ['board'])).toBe('No data for this filter')
    expect(emptyPlaceholder(era, ['model'])).toBe('No data for this filter')
  })
  it('blames the range when no filter is active', () => {
    expect(emptyPlaceholder(null, [])).toBe('No data in this range')
  })
})

describe('withNotApplicable', () => {
  it('fills every block with null when the payload has no not_applicable', () => {
    const out = withNotApplicable({ total: 3, devices: [] }, ['crashes', 'prints'] as const)
    expect(out).toEqual({ total: 3, devices: [], not_applicable: { crashes: null, prints: null } })
  })
  it('keeps what the worker sent and fills only the missing blocks', () => {
    const out = withNotApplicable({ not_applicable: { crashes: 'model' as const } }, ['crashes', 'errors'] as const)
    expect(out.not_applicable).toEqual({ crashes: 'model', errors: null })
  })
})

describe('ppChange', () => {
  it('is null when either rate is missing', () => {
    expect(ppChange(null, 0.5, true)).toBeNull()
    expect(ppChange(0.5, null, true)).toBeNull()
  })
  it('signs the change in percentage points and judges it by the good direction', () => {
    expect(ppChange(0.92, 0.9, true)).toEqual({ text: '↑ +2.0pp', tone: 'good' })
    expect(ppChange(0.92, 0.9, false)).toEqual({ text: '↑ +2.0pp', tone: 'bad' })
    expect(ppChange(0.05, 0.08, false)).toEqual({ text: '↓ -3.0pp', tone: 'good' })
  })
  it('reads a change under 0.05pp as flat on both sides of zero', () => {
    expect(ppChange(0.5004, 0.5, true)).toEqual({ text: '= 0.0pp', tone: 'flat' })
    expect(ppChange(0.4996, 0.5, true)).toEqual({ text: '= 0.0pp', tone: 'flat' })
    expect(ppChange(0.5006, 0.5, true)?.tone).toBe('good')
  })
})

describe('unreportedNote', () => {
  it('is empty when every column is reported', () => {
    expect(unreportedNote([['Crash rate', null], ['Print success', null]])).toBe('')
  })
  it('groups the unreported columns by filter', () => {
    expect(notReportedPer('model')).toBe('Not reported per printer model')
    expect(
      unreportedNote([
        ['Crash-free', 'model'],
        ['Crash rate', 'model'],
        ['Devices', null],
        ['p95 frame', 'board'],
      ])
    ).toBe('Crash-free, Crash rate: Not reported per printer model; p95 frame: Not reported per board')
  })
})

describe('hangCauseLine', () => {
  it('lists every cause of a hang split under its label', () => {
    expect(hangCauseLine({ app_blocked: 3, host_memory: 7, suspended: 2, unknown: 1 })).toBe(
      '3 app blocked, 7 host out of memory, 2 app suspended, 1 unknown'
    )
  })
  it('reads a cause a stored weekly report predates as 0', () => {
    const older = { app_blocked: 3, host_memory: 7, unknown: 0 } as HangSplit
    expect(hangCauseLine(older)).toBe('3 app blocked, 7 host out of memory, 0 app suspended, 0 unknown')
  })
  it('lists a cause a newer worker sends and this dashboard has no label for, under its key', () => {
    const newer = { app_blocked: 3, host_memory: 7, suspended: 2, unknown: 1, device_frozen: 5 } as HangSplit
    expect(hangCauseLine(newer)).toBe(
      '3 app blocked, 7 host out of memory, 2 app suspended, 1 unknown, 5 device_frozen'
    )
  })
})

describe('mergeOptions', () => {
  it('merges nested objects and lets the override replace leaves', () => {
    const base = { plugins: { tooltip: { backgroundColor: '#111', borderWidth: 1 } }, scales: { x: { min: 0 } } }
    const over = { plugins: { tooltip: { borderWidth: 2 } }, scales: { x: { min: 5 } } }
    expect(mergeOptions(base, over)).toEqual({
      plugins: { tooltip: { backgroundColor: '#111', borderWidth: 2 } },
      scales: { x: { min: 5 } }
    })
    expect(base.plugins.tooltip.borderWidth).toBe(1)
  })
  it('keeps the base when there is no override and replaces arrays whole', () => {
    expect(mergeOptions({ a: [1, 2] }, undefined)).toEqual({ a: [1, 2] })
    expect(mergeOptions({ a: [1, 2] }, { a: [3] })).toEqual({ a: [3] })
  })
})

describe('rateAxis', () => {
  it('labels 0-1 ticks as percentages', () => {
    expect(rateAxis(1).ticks.callback(0.025)).toBe('2.5%')
    expect(rateAxis().ticks.callback('0.5')).toBe('50%')
  })
})

describe('olderLines', () => {
  it('judges current lines from the reference list, not the filtered rows', () => {
    const filtered = rows(['0.99.118'])
    expect(olderLines(filtered, rows(LIVE), new Set()).map((g) => g.label)).toEqual(['0.99.118'])
    expect(olderLines(rows(LIVE), rows(LIVE), new Set()).map((g) => g.label)).toEqual(['0.99.x (2 versions)'])
  })
  it('keeps an expanded old line, header included', () => {
    expect(olderLines(rows(LIVE), rows(LIVE), new Set(['line:0.99'])).map((g) => g.label)).toEqual([
      '0.99.x ▾',
      '0.99.118',
      '0.99.30'
    ])
  })
})

describe('groupVersionRates', () => {
  const r = (version: string, numerator: number, denominator: number, rate: number | null = numerator / denominator) => ({
    version,
    numerator,
    denominator,
    rate
  })
  it('recombines a group from member counts, not by averaging rates', () => {
    const out = groupVersionRates(
      [r('1.1.0', 0, 5), r('1.0.5', 1, 10), r('1.0.4', 1, 10), r('0.99.2', 9, 10), r('0.99.1', 1, 990)],
      new Set()
    )
    const line = out.find((g) => g.key === 'line:0.99')!
    expect(line).toMatchObject({ numerator: 10, denominator: 1000, members: ['0.99.2', '0.99.1'] })
    expect(line.rate).toBeCloseTo(0.01)
    expect(out.find((g) => g.key === '1.0.5')!.rate).toBeCloseTo(0.1)
  })
  it('keeps null when the filters make every member inapplicable or nothing was counted', () => {
    const out = groupVersionRates([r('1.0.5', 0, 4, null), r('1.0.4', 0, 0, null)], new Set())
    expect(out.map((g) => g.rate)).toEqual([null, null])
    expect(groupVersionRates([r('1.0.5', 0, 0, 0)], new Set())[0].rate).toBeNull()
  })
})

describe('rowAction', () => {
  it('drills a filterable row, refuses a sanitized or empty one', () => {
    expect(rowAction(single('Voron 2.4'))).toBe('drill')
    expect(rowAction(single("CdO - Cubo dell'Odio"))).toBe('none')
    expect(rowAction(single(''))).toBe('none')
    expect(rowHint(single("CdO - Cubo dell'Odio"))).toBe('not filterable')
    expect(rowHint(single('pi'))).toBe('click to filter')
    expect(rowLabel({ ...single('x'), expandable: true })).toBe('x ▸')
  })
  it('refuses a group when any member is not filterable', () => {
    expect(rowAction({ ...single('a'), members: ['Voron 2.4', "Cubo dell'Odio"] })).toBe('none')
  })
})

describe('planActivation', () => {
  it('drills several keys in one step and skips the ones it cannot', () => {
    expect(planActivation([['version', single('1.0.5')], ['board', single('k1')]])).toEqual({
      kind: 'drill',
      filters: { version: ['1.0.5'], board: ['k1'] }
    })
    expect(planActivation([['version', single('1.0.5')], ['board', single('')]])).toEqual({
      kind: 'drill',
      filters: { version: ['1.0.5'] }
    })
    expect(planActivation([['board', single('')]])).toBeNull()
  })
})

describe('isRenderError', () => {
  it('matches the render function in a development build and a production build', () => {
    expect(isRenderError('render function')).toBe(true)
    expect(isRenderError('https://vuejs.org/error-reference/#runtime-1')).toBe(true)
    expect(isRenderError(1)).toBe(true)
  })
  it('leaves handler, watcher and lifecycle errors alone', () => {
    for (const info of [
      'component event handler', 'native event handler', 'watcher callback', 'watcher getter',
      'mounted hook', 'setup function', 'scheduler flush',
      'https://vuejs.org/error-reference/#runtime-5', 'https://vuejs.org/error-reference/#runtime-6',
      'https://vuejs.org/error-reference/#runtime-14', 'https://vuejs.org/error-reference/#runtime-m',
      6, 0, undefined, ''
    ]) {
      expect(isRenderError(info), String(info)).toBe(false)
    }
  })
})

describe('versionChips', () => {
  it('groups the picker like the charts and counts shown members per chip', () => {
    const chips = versionChips(rows(LIVE), new Set(), new Set(['1.0.5', '0.99.30']))
    expect(chips.map((c) => [c.label, c.shown])).toEqual([
      ['1.1.0 betas (2)', 0],
      ['1.0.5', 1],
      ['1.0.4', 0],
      ['1.0.0', 0],
      ['0.99.x (2 versions)', 1]
    ])
  })
  it('lists an open line as its header and its members, each counted on its own', () => {
    const chips = versionChips(rows(LIVE), new Set(['line:0.99']), new Set(['0.99.30']))
    expect(chips.slice(-3).map((c) => [c.label, c.expanded ?? false, c.shown])).toEqual([
      ['0.99.x ▾', true, 1],
      ['0.99.118', false, 0],
      ['0.99.30', false, 1]
    ])
  })
})

describe('low-count material sets', () => {
  it('dims a set with fewer than five prints and keeps five or more', () => {
    expect(LOW_COUNT_PRINTS).toBe(5)
    expect(isLowCount(4)).toBe(true)
    expect(isLowCount(4.9)).toBe(true)
    expect(isLowCount(0)).toBe(true)
    expect(isLowCount(5)).toBe(false)
    expect(isLowCount(120)).toBe(false)
  })
  it('dims a series color by giving it an alpha', () => {
    expect(dimmed('#3b82f6')).toBe('#3b82f659')
    expect(dimmed(seriesColor(1))).toBe(`${seriesColor(1)}59`)
  })
})

describe('formatRateOf', () => {
  it('shows the counts a rate rests on beside it', () => {
    expect(formatRateOf(1, 1, 1)).toBe('100.0% (1/1)')
    expect(formatRateOf(0.25, 1, 4)).toBe('25.0% (1/4)')
    expect(formatRateOf(0.5, 1200, 2400)).toBe('50.0% (1,200/2,400)')
  })
  it('rounds weighted counts and reads n/a with no denominator', () => {
    expect(formatRateOf(0.5, 1.6, 3.2)).toBe('50.0% (2/3)')
    expect(formatRateOf(null, 0, 0)).toBe('n/a')
  })
  it('reads n/a for a missing or non-finite rate, in both formatters', () => {
    expect(formatRate(undefined as never)).toBe('n/a')
    expect(formatRate(NaN)).toBe('n/a')
    expect(formatRateOf(undefined as never, 1, 1)).toBe('n/a')
    expect(formatRateOf(NaN, 1, 1)).toBe('n/a')
  })
  it('shows the rate alone when the payload carries no counts', () => {
    expect(formatRateOf(0.5, undefined as unknown as number, NaN)).toBe('50.0%')
  })
})
