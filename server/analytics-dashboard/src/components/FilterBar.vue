<template>
  <div class="filter-bar">
    <div class="filter-group" v-for="filter in filters" :key="filter.key">
      <div class="filter-dropdown" :class="{ open: openDropdown === filter.key }">
        <button class="filter-trigger" @click="toggleDropdown(filter.key)">
          <span class="filter-label">{{ filter.label }}</span>
          <span v-if="filter.selected.length" class="filter-count">{{ filter.selected.length }}</span>
          <span class="filter-arrow">&#9662;</span>
        </button>
        <div v-if="openDropdown === filter.key" class="filter-menu">
          <label
            v-for="opt in filter.options"
            :key="opt"
            class="filter-option"
            :class="{ selected: filter.selected.includes(opt) }"
          >
            <input
              type="checkbox"
              :checked="filter.selected.includes(opt)"
              @change="toggleOption(filter.key, opt)"
            />
            {{ opt }}
          </label>
          <div v-if="!filter.options.length" class="filter-empty">No options</div>
        </div>
      </div>
    </div>
    <DateRangePicker v-model="filtersStore.range" />
    <button v-if="hasActiveFilters" class="clear-btn" @click="filtersStore.clearFilters()">
      Clear
    </button>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import DateRangePicker from '@/components/DateRangePicker.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'

const filtersStore = useFiltersStore()

const platformOptions = ref<string[]>([])
const versionOptions = ref<string[]>([])
const modelOptions = ref<string[]>([])
const openDropdown = ref<string | null>(null)

const filters = computed(() => [
  { key: 'platform', label: 'Platform', options: platformOptions.value, selected: filtersStore.platform },
  { key: 'version', label: 'Version', options: versionOptions.value, selected: filtersStore.version },
  { key: 'model', label: 'Model', options: modelOptions.value, selected: filtersStore.model }
])

const hasActiveFilters = computed(() =>
  filtersStore.platform.length > 0 ||
  filtersStore.version.length > 0 ||
  filtersStore.model.length > 0
)

function toggleDropdown(key: string) {
  openDropdown.value = openDropdown.value === key ? null : key
}

function toggleOption(key: string, value: string) {
  const store = filtersStore
  const current = [...store[key as 'platform' | 'version' | 'model']]
  const idx = current.indexOf(value)
  if (idx >= 0) {
    current.splice(idx, 1)
  } else {
    current.push(value)
  }
  if (key === 'platform') store.setPlatform(current)
  else if (key === 'version') store.setVersion(current)
  else if (key === 'model') store.setModel(current)
}

function handleClickOutside(e: MouseEvent) {
  const target = e.target as HTMLElement
  if (!target.closest('.filter-dropdown')) {
    openDropdown.value = null
  }
}

onMounted(async () => {
  document.addEventListener('click', handleClickOutside)
  try {
    const data = await api.getAdoption('range=90d')
    platformOptions.value = data.platforms.map(p => p.name)
    versionOptions.value = data.versions.map(v => v.name)
    modelOptions.value = data.printer_models.map(m => m.name)
  } catch {
    // Options will remain empty
  }
})

onUnmounted(() => {
  document.removeEventListener('click', handleClickOutside)
})
</script>

<style scoped>
.filter-bar {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.filter-group {
  position: relative;
}

.filter-dropdown {
  position: relative;
}

.filter-trigger {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 12px;
  background: transparent;
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-secondary);
  font-size: 13px;
  cursor: pointer;
  transition: color 0.15s, border-color 0.15s;
}

.filter-trigger:hover {
  color: var(--text-primary);
  border-color: var(--text-secondary);
}

.filter-count {
  background: var(--accent-blue);
  color: #fff;
  font-size: 11px;
  padding: 1px 6px;
  border-radius: 10px;
  font-weight: 500;
}

.filter-arrow {
  font-size: 10px;
  opacity: 0.6;
}

.filter-menu {
  position: absolute;
  top: calc(100% + 4px);
  left: 0;
  min-width: 180px;
  max-height: 240px;
  overflow-y: auto;
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 4px;
  z-index: 100;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
}

.filter-option {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 8px;
  border-radius: 4px;
  color: var(--text-secondary);
  font-size: 13px;
  cursor: pointer;
  transition: background 0.15s;
}

.filter-option:hover {
  background: rgba(59, 130, 246, 0.08);
}

.filter-option.selected {
  color: var(--text-primary);
}

.filter-option input[type="checkbox"] {
  accent-color: var(--accent-blue);
}

.filter-empty {
  padding: 8px;
  color: var(--text-secondary);
  font-size: 12px;
  text-align: center;
}

.clear-btn {
  padding: 6px 12px;
  background: transparent;
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-secondary);
  font-size: 13px;
  cursor: pointer;
  transition: color 0.15s, border-color 0.15s;
}

.clear-btn:hover {
  color: var(--accent-red);
  border-color: var(--accent-red);
}
</style>
