<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Hardware</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Avg Build Volume"
            :value="formatVolume(data.avg_build_volume)"
            color="var(--accent-blue)"
          />
          <MetricCard
            title="Avg Fan Count"
            :value="data.avg_counts.fans.toFixed(1)"
            color="var(--accent-green)"
          />
          <MetricCard
            title="Avg Sensor Count"
            :value="data.avg_counts.sensors.toFixed(1)"
            color="var(--accent-yellow)"
          />
          <MetricCard
            title="Avg Macro Count"
            :value="data.avg_counts.macros.toFixed(0)"
            color="var(--accent-purple)"
          />
        </div>

        <div class="chart-section">
          <h3>Top 20 Printer Models</h3>
          <BarChart :data="printerChartData" :options="horizontalOpts" />
        </div>

        <div class="chart-section">
          <h3>Kinematics Breakdown</h3>
          <PieChart :data="kinematicsChartData" />
        </div>

        <div class="chart-section">
          <h3>MCU Chip Distribution</h3>
          <BarChart :data="mcuChartData" :options="horizontalOpts" />
        </div>

        <div class="chart-section">
          <h3>Capability Adoption</h3>
          <BarChart :data="capabilityChartData" :options="percentHorizontalOpts" />
        </div>
      </template>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import MetricCard from '@/components/MetricCard.vue'
import BarChart from '@/components/BarChart.vue'
import PieChart from '@/components/PieChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { HardwareData } from '@/services/api'
import type { ChartOptions } from 'chart.js'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<HardwareData | null>(null)
const loading = ref(true)
const error = ref('')

const horizontalOpts: ChartOptions<'bar'> = { indexAxis: 'y' }

const percentHorizontalOpts: ChartOptions<'bar'> = {
  indexAxis: 'y',
  scales: {
    x: {
      min: 0,
      max: 100,
      ticks: {
        callback: (value) => `${value}%`,
        color: '#94a3b8'
      },
      grid: { color: 'rgba(45, 51, 72, 0.5)' }
    },
    y: {
      ticks: { color: '#94a3b8' },
      grid: { color: 'rgba(45, 51, 72, 0.5)' }
    }
  }
}

function formatVolume(vol: { x: number; y: number; z: number }): string {
  return `${Math.round(vol.x)}x${Math.round(vol.y)}x${Math.round(vol.z)} mm`
}

const printerChartData = computed(() => {
  const top20 = data.value?.printer_models.slice(0, 20) ?? []
  return {
    labels: top20.map(p => p.name),
    datasets: [{
      label: 'Devices',
      data: top20.map(p => p.count),
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

const mcuChartData = computed(() => ({
  labels: data.value?.mcu_chips.map(m => m.name) ?? [],
  datasets: [{
    label: 'Devices',
    data: data.value?.mcu_chips.map(m => m.count) ?? [],
    backgroundColor: '#3b82f6'
  }]
}))

const CAP_NAMES = ['Has AMS', 'Has LED', 'Has Power Control', 'Has Chamber', 'Has Accelerometer', 'Has Filament Sensor', 'Has Probe', 'Has Multi Extruder']

const capabilityChartData = computed(() => {
  const caps = data.value?.capabilities
  if (!caps || !caps.total) return { labels: [], datasets: [] }
  const pcts = caps.bits.map(count => (count / caps.total) * 100)
  return {
    labels: CAP_NAMES.slice(0, caps.bits.length),
    datasets: [{
      label: 'Adoption %',
      data: pcts,
      backgroundColor: '#8b5cf6'
    }]
  }
})

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getHardware(filters.queryString)
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
