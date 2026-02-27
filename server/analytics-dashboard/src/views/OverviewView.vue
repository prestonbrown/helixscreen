<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Overview</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard title="Active Devices" :value="data.active_devices" />
          <MetricCard title="Total Events" :value="data.total_events.toLocaleString()" />
          <MetricCard
            title="Crash Rate"
            :value="`${(data.crash_rate * 100).toFixed(1)}%`"
            :color="data.crash_rate * 100 > 5 ? 'var(--accent-red)' : 'var(--accent-green)'"
          />
          <MetricCard
            title="Print Success Rate"
            :value="`${(data.print_success_rate * 100).toFixed(1)}%`"
            color="var(--accent-green)"
          />
        </div>

        <div class="chart-section">
          <h3>Events Over Time</h3>
          <LineChart :data="eventsChartData" />
        </div>

        <div class="chart-section">
          <h3>Daily Active Devices</h3>
          <LineChart :data="dailyActiveChartData" />
        </div>

        <div class="chart-section">
          <h3>Cumulative Device Growth</h3>
          <LineChart :data="cumulativeChartData" />
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
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { OverviewData } from '@/services/api'

const filters = useFiltersStore()
const data = ref<OverviewData | null>(null)
const loading = ref(true)
const error = ref('')

const eventsChartData = computed(() => ({
  labels: data.value?.events_over_time.map(e => e.date) ?? [],
  datasets: [{
    label: 'Events',
    data: data.value?.events_over_time.map(e => e.count) ?? [],
    borderColor: '#3b82f6',
    backgroundColor: 'rgba(59, 130, 246, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const dailyActiveChartData = computed(() => ({
  labels: data.value?.daily_active_devices.map(e => e.date) ?? [],
  datasets: [{
    label: 'Active Devices',
    data: data.value?.daily_active_devices.map(e => e.devices) ?? [],
    borderColor: '#8b5cf6',
    backgroundColor: 'rgba(139, 92, 246, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

const cumulativeChartData = computed(() => ({
  labels: data.value?.cumulative_devices.map(e => e.date) ?? [],
  datasets: [{
    label: 'Total Devices',
    data: data.value?.cumulative_devices.map(e => e.total) ?? [],
    borderColor: '#10b981',
    backgroundColor: 'rgba(16, 185, 129, 0.1)',
    fill: true,
    tension: 0.3
  }]
}))

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getOverview(filters.queryString)
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

.loading, .error {
  padding: 40px;
  text-align: center;
  color: var(--text-secondary);
}

.error {
  color: var(--accent-red);
}
</style>
