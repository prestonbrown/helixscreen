<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Prints</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="chart-section">
          <h3>Success Rate Over Time</h3>
          <LineChart :data="successRateChartData" />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Success by Filament Type</h3>
            <BarChart :data="filamentChartData" />
          </div>
          <MetricCard
            title="Average Print Duration"
            :value="formatDuration(data.avg_duration_sec)"
            subtitle="across all completed prints"
          />
        </div>

        <template v-if="data.start_context">
          <h3 class="section-header">Print Start Insights</h3>

          <div class="grid-2col">
            <div class="chart-section">
              <h3>Slicer Distribution</h3>
              <PieChart :data="slicerChartData" />
            </div>
            <div class="chart-section">
              <h3>Source Distribution</h3>
              <PieChart :data="sourceChartData" />
            </div>
          </div>

          <div class="grid-2col">
            <div class="chart-section">
              <h3>File Size Buckets</h3>
              <BarChart :data="fileSizeChartData" />
            </div>
            <div class="metrics-col">
              <MetricCard
                title="Thumbnail Adoption"
                :value="`${data.start_context.thumbnail_rate.toFixed(1)}%`"
                color="var(--accent-green)"
              />
              <MetricCard
                title="AMS Usage"
                :value="`${data.start_context.ams_rate.toFixed(1)}%`"
                color="var(--accent-blue)"
              />
            </div>
          </div>
        </template>
      </template>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import LineChart from '@/components/LineChart.vue'
import BarChart from '@/components/BarChart.vue'
import PieChart from '@/components/PieChart.vue'
import MetricCard from '@/components/MetricCard.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { PrintsData } from '@/services/api'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<PrintsData | null>(null)
const loading = ref(true)
const error = ref('')

function formatDuration(seconds: number): string {
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  return h > 0 ? `${h}h ${m}m` : `${m}m`
}

const successRateChartData = computed(() => ({
  labels: data.value?.success_rate_over_time.map(d => d.date) ?? [],
  datasets: [{
    label: 'Success Rate %',
    data: data.value?.success_rate_over_time.map(d => d.rate) ?? [],
    borderColor: '#10b981',
    backgroundColor: 'rgba(16, 185, 129, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const filamentChartData = computed(() => ({
  labels: data.value?.by_filament.map(f => f.type) ?? [],
  datasets: [{
    label: 'Success Rate %',
    data: data.value?.by_filament.map(f => f.success_rate) ?? [],
    backgroundColor: '#3b82f6'
  }]
}))

const slicerChartData = computed(() => ({
  labels: data.value?.start_context?.slicers.map(s => s.name) ?? [],
  datasets: [{
    data: data.value?.start_context?.slicers.map(s => s.count) ?? [],
    backgroundColor: COLORS
  }]
}))

const sourceChartData = computed(() => ({
  labels: data.value?.start_context?.sources.map(s => s.name) ?? [],
  datasets: [{
    data: data.value?.start_context?.sources.map(s => s.count) ?? [],
    backgroundColor: COLORS
  }]
}))

const fileSizeChartData = computed(() => ({
  labels: data.value?.start_context?.file_size_buckets.map(b => b.name) ?? [],
  datasets: [{
    label: 'Files',
    data: data.value?.start_context?.file_size_buckets.map(b => b.count) ?? [],
    backgroundColor: '#f59e0b'
  }]
}))

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getPrints(filters.queryString)
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
  align-items: start;
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

.section-header {
  font-size: 16px;
  font-weight: 600;
  margin-bottom: 16px;
  margin-top: 8px;
  padding-top: 16px;
  border-top: 1px solid var(--border);
}

.metrics-col {
  display: flex;
  flex-direction: column;
  gap: 16px;
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
