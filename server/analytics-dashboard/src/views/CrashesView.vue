<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Crashes</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="chart-section">
          <h3>Crash Rate by Version</h3>
          <BarChart :data="versionChartData" :options="barOptions" />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Crashes by Signal</h3>
            <PieChart :data="signalChartData" />
          </div>
          <MetricCard
            title="Average Uptime"
            :value="formatDuration(data.avg_uptime_sec)"
            subtitle="before crash"
            color="var(--accent-yellow)"
          />
        </div>

        <div class="chart-section">
          <h3>Recent Crashes</h3>
          <div v-if="crashListLoading" class="loading">Loading crash list...</div>
          <div v-else-if="crashList.length === 0" class="empty-state">No crash events found in this period.</div>
          <div v-else class="table-wrapper">
            <table class="crash-table">
              <thead>
                <tr>
                  <th>Time</th>
                  <th>Version</th>
                  <th>Signal</th>
                  <th>Platform</th>
                  <th>Uptime</th>
                  <th>Device</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="(crash, i) in crashList" :key="i">
                  <td class="mono">{{ formatTimestamp(crash.timestamp) }}</td>
                  <td><span class="badge version">{{ crash.version || '—' }}</span></td>
                  <td><span class="badge" :class="signalClass(crash.signal)">{{ crash.signal || '—' }}</span></td>
                  <td>{{ crash.platform || '—' }}</td>
                  <td class="mono">{{ formatDuration(crash.uptime_sec) }}</td>
                  <td class="mono device-id">{{ shortDeviceId(crash.device_id) }}</td>
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
import BarChart from '@/components/BarChart.vue'
import PieChart from '@/components/PieChart.vue'
import MetricCard from '@/components/MetricCard.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { CrashesData, CrashListData } from '@/services/api'
import type { ChartOptions } from 'chart.js'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<CrashesData | null>(null)
const crashList = ref<CrashListData['crashes']>([])
const loading = ref(true)
const crashListLoading = ref(true)
const error = ref('')

function formatDuration(seconds: number): string {
  if (!seconds) return '0m'
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  return h > 0 ? `${h}h ${m}m` : `${m}m`
}

function formatTimestamp(ts: string): string {
  try {
    const d = new Date(ts)
    return d.toLocaleDateString('en-US', { month: 'short', day: 'numeric' }) +
      ' ' + d.toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit', hour12: false })
  } catch {
    return ts
  }
}

function shortDeviceId(id: string): string {
  if (!id) return '—'
  return id.slice(0, 8) + '...'
}

function signalClass(signal: string): string {
  if (signal === 'SIGSEGV') return 'signal-segv'
  if (signal === 'SIGABRT') return 'signal-abrt'
  return 'signal-other'
}

const barOptions: ChartOptions<'bar'> = {
  scales: {
    y: {
      ticks: {
        callback: (value) => `${value}%`,
        color: '#94a3b8',
      },
      grid: { color: 'rgba(45, 51, 72, 0.5)' },
    },
    x: {
      ticks: { color: '#94a3b8' },
      grid: { color: 'rgba(45, 51, 72, 0.5)' },
    },
  },
}

const versionChartData = computed(() => ({
  labels: data.value?.by_version.map(v => v.version) ?? [],
  datasets: [{
    label: 'Crash Rate %',
    data: data.value?.by_version.map(v => v.rate * 100) ?? [],
    backgroundColor: '#ef4444'
  }]
}))

const signalChartData = computed(() => ({
  labels: data.value?.by_signal.map(s => s.signal) ?? [],
  datasets: [{
    data: data.value?.by_signal.map(s => s.count) ?? [],
    backgroundColor: COLORS
  }]
}))

async function fetchData() {
  loading.value = true
  crashListLoading.value = true
  error.value = ''
  try {
    const [crashesData, listData] = await Promise.all([
      api.getCrashes(filters.queryString),
      api.getCrashList(filters.queryString),
    ])
    data.value = crashesData
    crashList.value = listData.crashes
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
    crashListLoading.value = false
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

.crash-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

.crash-table th {
  text-align: left;
  padding: 10px 14px;
  font-weight: 500;
  color: var(--text-secondary);
  border-bottom: 1px solid var(--border);
  white-space: nowrap;
}

.crash-table td {
  padding: 8px 14px;
  border-bottom: 1px solid var(--border);
  color: var(--text-primary);
  white-space: nowrap;
}

.crash-table tbody tr:last-child td {
  border-bottom: none;
}

.crash-table tbody tr:hover {
  background: rgba(255, 255, 255, 0.03);
}

.mono {
  font-family: 'SF Mono', 'Fira Code', monospace;
  font-size: 12px;
}

.device-id {
  color: var(--text-secondary);
}

.badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 12px;
  font-weight: 500;
}

.badge.version {
  background: rgba(59, 130, 246, 0.15);
  color: #60a5fa;
}

.badge.signal-segv {
  background: rgba(239, 68, 68, 0.15);
  color: #f87171;
}

.badge.signal-abrt {
  background: rgba(245, 158, 11, 0.15);
  color: #fbbf24;
}

.badge.signal-other {
  background: rgba(139, 92, 246, 0.15);
  color: #a78bfa;
}
</style>
