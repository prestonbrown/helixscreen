import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import type { LocationQuery, Router } from 'vue-router'

export const useFiltersStore = defineStore('filters', () => {
  const range = ref('30d')
  const platform = ref<string[]>([])
  const version = ref<string[]>([])
  const model = ref<string[]>([])

  const queryParams = computed(() => {
    const params: Record<string, string> = { range: range.value }
    if (platform.value.length) params.platform = platform.value.join(',')
    if (version.value.length) params.version = version.value.join(',')
    if (model.value.length) params.model = model.value.join(',')
    return params
  })

  const queryString = computed(() => {
    const p = new URLSearchParams(queryParams.value)
    return p.toString()
  })

  function setRange(r: string) {
    range.value = r
  }

  function setPlatform(vals: string[]) {
    platform.value = vals
  }

  function setVersion(vals: string[]) {
    version.value = vals
  }

  function setModel(vals: string[]) {
    model.value = vals
  }

  function clearFilters() {
    platform.value = []
    version.value = []
    model.value = []
  }

  function syncFromRoute(query: LocationQuery) {
    if (query.range && typeof query.range === 'string') {
      range.value = query.range
    }
    platform.value = query.platform ? String(query.platform).split(',') : []
    version.value = query.version ? String(query.version).split(',') : []
    model.value = query.model ? String(query.model).split(',') : []
  }

  function syncToRoute(router: Router) {
    router.replace({ query: queryParams.value })
  }

  return {
    range,
    platform,
    version,
    model,
    queryParams,
    queryString,
    setRange,
    setPlatform,
    setVersion,
    setModel,
    clearFilters,
    syncFromRoute,
    syncToRoute
  }
})
