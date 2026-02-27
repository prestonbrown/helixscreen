<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Reliability</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Worst-Case Outage"
            :value="formatDuration(data.max_disconnect_sec)"
            subtitle="longest single outage"
            color="var(--accent-red)"
          />
        </div>

        <div class="chart-section">
          <h3>Uptime % Over Time</h3>
          <LineChart :data="uptimeChartData" :options="uptimeOpts" />
        </div>

        <div class="chart-section">
          <h3>Disconnect Frequency Over Time</h3>
          <LineChart :data="disconnectChartData" />
        </div>

        <div class="chart-section">
          <h3>Top Error Categories</h3>
          <BarChart :data="errorCategoryChartData" :options="horizontalOpts" />
        </div>

        <div class="chart-section">
          <h3>Error Codes</h3>
          <div v-if="data.error_codes.length === 0" class="empty-state">No error codes found in this period.</div>
          <div v-else class="table-wrapper">
            <table class="data-table">
              <thead>
                <tr>
                  <th>Category</th>
                  <th>Code</th>
                  <th>Count</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="(row, i) in data.error_codes" :key="i">
                  <td><span class="badge">{{ row.category }}</span></td>
                  <td class="mono">{{ row.code }}</td>
                  <td class="mono">{{ row.count.toLocaleString() }}</td>
                </tr>
              </tbody>
            </table>
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
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { ReliabilityData } from '@/services/api'
import type { ChartOptions } from 'chart.js'

const filters = useFiltersStore()
const data = ref<ReliabilityData | null>(null)
const loading = ref(true)
const error = ref('')

const horizontalOpts: ChartOptions<'bar'> = { indexAxis: 'y' }

const uptimeOpts: ChartOptions<'line'> = {
  scales: {
    y: {
      min: 0,
      max: 100,
      ticks: {
        callback: (value) => `${value}%`,
        color: '#94a3b8'
      },
      grid: { color: 'rgba(45, 51, 72, 0.5)' }
    },
    x: {
      ticks: { color: '#94a3b8' },
      grid: { color: 'rgba(45, 51, 72, 0.5)' }
    }
  }
}

function formatDuration(seconds: number): string {
  if (!seconds) return '0s'
  if (seconds < 60) return `${Math.round(seconds)}s`
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  const s = Math.round(seconds % 60)
  if (h > 0) return m > 0 ? `${h}h ${m}m` : `${h}h`
  return s > 0 ? `${m}m ${s}s` : `${m}m`
}

const uptimeChartData = computed(() => ({
  labels: data.value?.uptime_trend.map(d => d.date) ?? [],
  datasets: [{
    label: 'Uptime %',
    data: data.value?.uptime_trend.map(d => d.avg_uptime_pct) ?? [],
    borderColor: '#10b981',
    backgroundColor: 'rgba(16, 185, 129, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const disconnectChartData = computed(() => ({
  labels: data.value?.disconnect_trend.map(d => d.date) ?? [],
  datasets: [{
    label: 'Avg Disconnects',
    data: data.value?.disconnect_trend.map(d => d.avg_disconnects) ?? [],
    borderColor: '#ef4444',
    backgroundColor: 'rgba(239, 68, 68, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const errorCategoryChartData = computed(() => ({
  labels: data.value?.error_categories.map(c => c.category) ?? [],
  datasets: [{
    label: 'Count',
    data: data.value?.error_categories.map(c => c.count) ?? [],
    backgroundColor: '#ef4444'
  }]
}))

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getReliability(filters.queryString)
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

.chart-section {
  margin-bottom: 24px;
}

.chart-section h3 {
  font-size: 14px;
  font-weight: 500;
  color: var(--text-secondary);
  margin-bottom: 12px;
}

.loading, .error, .empty-state {
  padding: 40px;
  text-align: center;
  color: var(--text-secondary);
}

.error {
  color: var(--accent-red);
}

.empty-state {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 8px;
}

.table-wrapper {
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: 8px;
  overflow-x: auto;
}

.data-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

.data-table th {
  text-align: left;
  padding: 10px 14px;
  font-weight: 500;
  color: var(--text-secondary);
  border-bottom: 1px solid var(--border);
  white-space: nowrap;
}

.data-table td {
  padding: 8px 14px;
  border-bottom: 1px solid var(--border);
  color: var(--text-primary);
  white-space: nowrap;
}

.data-table tbody tr:last-child td {
  border-bottom: none;
}

.data-table tbody tr:hover {
  background: rgba(255, 255, 255, 0.03);
}

.mono {
  font-family: 'SF Mono', 'Fira Code', monospace;
  font-size: 12px;
}

.badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 12px;
  font-weight: 500;
  background: rgba(139, 92, 246, 0.15);
  color: #a78bfa;
}
</style>
