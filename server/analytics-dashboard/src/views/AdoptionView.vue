<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Adoption</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="grid-2col">
          <div class="chart-section">
            <h3>Platform Distribution</h3>
            <PieChart :data="platformChartData" />
          </div>
          <div class="chart-section">
            <h3>Version Distribution</h3>
            <BarChart :data="versionChartData" :options="horizontalOpts" />
          </div>
        </div>

        <div class="chart-section">
          <h3>Top Printer Models</h3>
          <BarChart :data="printerChartData" :options="horizontalOpts" />
        </div>

        <div class="chart-section">
          <h3>Kinematics Breakdown</h3>
          <PieChart :data="kinematicsChartData" />
        </div>
      </template>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import PieChart from '@/components/PieChart.vue'
import BarChart from '@/components/BarChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { AdoptionData } from '@/services/api'
import type { ChartOptions } from 'chart.js'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<AdoptionData | null>(null)
const loading = ref(true)
const error = ref('')

const horizontalOpts: ChartOptions<'bar'> = { indexAxis: 'y' }

const platformChartData = computed(() => ({
  labels: data.value?.platforms.map(p => p.name) ?? [],
  datasets: [{
    data: data.value?.platforms.map(p => p.count) ?? [],
    backgroundColor: COLORS
  }]
}))

const versionChartData = computed(() => ({
  labels: data.value?.versions.map(v => v.name) ?? [],
  datasets: [{
    label: 'Devices',
    data: data.value?.versions.map(v => v.count) ?? [],
    backgroundColor: '#3b82f6'
  }]
}))

const printerChartData = computed(() => {
  const top10 = data.value?.printer_models.slice(0, 10) ?? []
  return {
    labels: top10.map(p => p.name),
    datasets: [{
      label: 'Devices',
      data: top10.map(p => p.count),
      backgroundColor: '#10b981'
    }]
  }
})

const kinematicsChartData = computed(() => ({
  labels: data.value?.kinematics.map(k => k.name) ?? [],
  datasets: [{
    data: data.value?.kinematics.map(k => k.count) ?? [],
    backgroundColor: COLORS
  }]
}))

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getAdoption(filters.queryString)
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
  }
}

watch(() => filters.queryString, fetchData, { immediate: true })
</script>

<style scoped>
.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 24px;
}

.page-header h2 {
  font-size: 20px;
  font-weight: 600;
}

.grid-2col {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
  margin-bottom: 24px;
}

.chart-section {
  margin-bottom: 24px;
}

.chart-section h3 {
  font-size: 14px;
  font-weight: 500;
  color: var(--text-secondary);
  margin-bottom: 12px;
}

.loading, .error {
  padding: 40px;
  text-align: center;
  color: var(--text-secondary);
}

.error {
  color: var(--accent-red);
}
</style>
