<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Memory</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Avg RSS"
            :value="formatMB(avgRss)"
            subtitle="resident memory"
            color="var(--accent-blue)"
          />
          <MetricCard
            title="Peak VM"
            :value="formatMB(peakVm)"
            subtitle="virtual memory"
            color="var(--accent-red)"
          />
          <MetricCard
            title="P95 RSS"
            :value="formatMB(p95Rss)"
            subtitle="95th percentile"
            color="var(--accent-yellow)"
          />
        </div>

        <div class="chart-section">
          <h3>RSS Over Time</h3>
          <LineChart :data="rssChartData" />
        </div>

        <div class="chart-section">
          <h3>Avg RSS by Platform</h3>
          <BarChart :data="platformChartData" :options="horizontalOpts" />
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
import type { MemoryData } from '@/services/api'
import type { ChartOptions } from 'chart.js'

const filters = useFiltersStore()
const data = ref<MemoryData | null>(null)
const loading = ref(true)
const error = ref('')

const horizontalOpts: ChartOptions<'bar'> = { indexAxis: 'y' }

function formatMB(kb: number): string {
  return `${(kb / 1024).toFixed(1)} MB`
}

const avgRss = computed(() => {
  if (!data.value?.rss_over_time.length) return 0
  const sum = data.value.rss_over_time.reduce((a, d) => a + d.avg_rss_kb, 0)
  return sum / data.value.rss_over_time.length
})

const peakVm = computed(() => {
  if (!data.value?.vm_peak_trend.length) return 0
  return Math.max(...data.value.vm_peak_trend.map(d => d.avg_vm_peak_kb))
})

const p95Rss = computed(() => {
  if (!data.value?.rss_over_time.length) return 0
  return Math.max(...data.value.rss_over_time.map(d => d.p95_rss_kb))
})

const rssChartData = computed(() => ({
  labels: data.value?.rss_over_time.map(d => d.date) ?? [],
  datasets: [
    {
      label: 'Avg RSS (KB)',
      data: data.value?.rss_over_time.map(d => d.avg_rss_kb) ?? [],
      borderColor: '#3b82f6',
      backgroundColor: 'rgba(59, 130, 246, 0.1)',
      fill: true,
      tension: 0.3
    },
    {
      label: 'P95 RSS (KB)',
      data: data.value?.rss_over_time.map(d => d.p95_rss_kb) ?? [],
      borderColor: '#f59e0b',
      backgroundColor: 'rgba(245, 158, 11, 0.1)',
      fill: false,
      tension: 0.3
    }
  ]
}))

const platformChartData = computed(() => ({
  labels: data.value?.rss_by_platform.map(p => p.platform) ?? [],
  datasets: [{
    label: 'Avg RSS (KB)',
    data: data.value?.rss_by_platform.map(p => p.avg_rss_kb) ?? [],
    backgroundColor: '#3b82f6'
  }]
}))

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getMemory(filters.queryString)
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
  grid-template-columns: repeat(3, 1fr);
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
