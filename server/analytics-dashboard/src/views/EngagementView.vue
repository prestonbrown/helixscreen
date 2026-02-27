<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Engagement</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Median Brightness"
            :value="`${Math.round(data.brightness.p50)}%`"
            color="var(--accent-yellow)"
          />
        </div>

        <div class="chart-section">
          <h3>Time per Panel</h3>
          <BarChart :data="panelTimeChartData" :options="horizontalOpts" />
        </div>

        <div class="chart-section">
          <h3>Visit Count per Panel</h3>
          <BarChart :data="panelVisitsChartData" :options="horizontalOpts" />
        </div>

        <div class="chart-section">
          <h3>Avg Session Duration Over Time</h3>
          <LineChart :data="sessionDurationChartData" />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Theme Distribution</h3>
            <PieChart :data="themeChartData" />
          </div>
          <div class="chart-section">
            <h3>Locale Breakdown</h3>
            <PieChart :data="localeChartData" />
          </div>
        </div>
      </template>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import MetricCard from '@/components/MetricCard.vue'
import LineChart from '@/components/LineChart.vue'
import BarChart from '@/components/BarChart.vue'
import PieChart from '@/components/PieChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { EngagementData } from '@/services/api'
import type { ChartOptions } from 'chart.js'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<EngagementData | null>(null)
const loading = ref(true)
const error = ref('')

const horizontalOpts: ChartOptions<'bar'> = { indexAxis: 'y' }

function formatTime(seconds: number): string {
  if (seconds < 60) return `${Math.round(seconds)}s`
  const m = Math.floor(seconds / 60)
  const s = Math.round(seconds % 60)
  if (m < 60) return s > 0 ? `${m}m ${s}s` : `${m}m`
  const h = Math.floor(m / 60)
  const rm = m % 60
  return rm > 0 ? `${h}h ${rm}m` : `${h}h`
}

const panelTimeChartData = computed(() => {
  const sorted = [...(data.value?.panel_time ?? [])].sort((a, b) => b.total_time_sec - a.total_time_sec)
  return {
    labels: sorted.map(p => `${p.panel} (${formatTime(p.total_time_sec)})`),
    datasets: [{
      label: 'Seconds',
      data: sorted.map(p => p.total_time_sec),
      backgroundColor: '#3b82f6'
    }]
  }
})

const panelVisitsChartData = computed(() => ({
  labels: data.value?.panel_visits.map(p => p.panel) ?? [],
  datasets: [{
    label: 'Visits',
    data: data.value?.panel_visits.map(p => p.total_visits) ?? [],
    backgroundColor: '#10b981'
  }]
}))

const sessionDurationChartData = computed(() => ({
  labels: data.value?.session_duration_trend.map(d => d.date) ?? [],
  datasets: [{
    label: 'Avg Duration (s)',
    data: data.value?.session_duration_trend.map(d => d.avg_session_sec) ?? [],
    borderColor: '#8b5cf6',
    backgroundColor: 'rgba(139, 92, 246, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const themeChartData = computed(() => ({
  labels: data.value?.themes.map(t => t.name) ?? [],
  datasets: [{
    data: data.value?.themes.map(t => t.count) ?? [],
    backgroundColor: COLORS
  }]
}))

const localeChartData = computed(() => ({
  labels: data.value?.locales.map(l => l.name) ?? [],
  datasets: [{
    data: data.value?.locales.map(l => l.count) ?? [],
    backgroundColor: COLORS
  }]
}))

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getEngagement(filters.queryString)
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

.metrics-row {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 16px;
  margin-bottom: 24px;
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
