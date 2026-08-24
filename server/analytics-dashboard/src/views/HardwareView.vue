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
          <BarChart :data="printerChartData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section">
          <h3>Kinematics Breakdown</h3>
          <PieChart :data="kinematicsChartData" />
        </div>

        <div class="chart-section">
          <h3>MCU Chip Distribution</h3>
          <BarChart :data="mcuChartData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section">
          <h3>Capability Adoption</h3>
          <BarChart :data="capabilityChartData" :options="percentHorizontalOpts" />
        </div>

        <div class="chart-section">
          <h3>Host RAM Distribution</h3>
          <BarChart :data="ramChartData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section" v-if="data.ams_backends.length > 0">
          <h3>AMS Backend Distribution</h3>
          <BarChart :data="amsChartData" :options="horizontalBarOpts" />
        </div>

        <div class="split-row">
          <div class="chart-section">
            <h3>Helix Macros Installed</h3>
            <template v-if="helixMacros.reported > 0">
              <PieChart :data="helixMacrosChartData" />
              <p class="chart-note">
                {{ pct(helixMacros.installed, helixMacros.reported) }} of
                {{ helixMacros.reported }} reporting devices have
                <code>helix_macros.cfg</code>
              </p>
            </template>
            <p v-else class="chart-note empty">No devices have reported this yet.</p>
          </div>

          <div class="chart-section">
            <h3>Moonraker Topology</h3>
            <template v-if="moonrakerLocality.reported > 0">
              <PieChart :data="moonrakerLocalityChartData" />
              <p class="chart-note">
                {{ pct(moonrakerLocality.remote, moonrakerLocality.reported) }} of
                {{ moonrakerLocality.reported }} reporting devices drive Moonraker
                over the network
              </p>
            </template>
            <!-- The day-one state. An empty chart here would read as "0% remote",
                 which is a conclusion rather than an absence of data. -->
            <p v-else class="chart-note empty">
              No devices have reported this yet. Requires a client release.
            </p>
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
import BarChart from '@/components/BarChart.vue'
import PieChart from '@/components/PieChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { HardwareData } from '@/services/api'
import type { ChartOptions } from 'chart.js'
import { horizontalBarOpts } from '@/utils/chart'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<HardwareData | null>(null)
const loading = ref(true)
const error = ref('')

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
      ticks: { autoSkip: false, color: '#94a3b8' },
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

const CAP_NAMES = [
  'Has Chamber', 'Has Accelerometer', 'Has Firmware Retraction', 'Has Exclude Object',
  'Has Timelapse', 'Has Klippain ShakeTune', 'Has Speaker',
  'Has Probe', 'Has LED', 'Has Filament Sensor', 'Has Multi Extruder', 'Has AMS', 'Has Heater Bed'
]

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

const ramChartData = computed(() => ({
  labels: data.value?.ram_distribution.map(r => r.name) ?? [],
  datasets: [{
    label: 'Devices',
    data: data.value?.ram_distribution.map(r => r.count) ?? [],
    backgroundColor: '#06b6d4'
  }]
}))

const amsChartData = computed(() => ({
  labels: data.value?.ams_backends.map(a => a.name) ?? [],
  datasets: [{
    label: 'Devices',
    data: data.value?.ams_backends.map(a => a.count) ?? [],
    backgroundColor: '#f59e0b'
  }]
}))

// Defensive defaults: the dashboard can be served against a worker that predates
// these fields, and an undefined here would blank the whole view.
const ZERO_SPLIT = { installed: 0, not_installed: 0, local: 0, remote: 0, reported: 0 }

const helixMacros = computed(() => data.value?.helix_macros ?? ZERO_SPLIT)
const moonrakerLocality = computed(() => data.value?.moonraker_locality ?? ZERO_SPLIT)

function pct(part: number, total: number): string {
  if (!total) return '0%'
  return `${Math.round((part / total) * 100)}%`
}

const helixMacrosChartData = computed(() => ({
  labels: ['Installed', 'Not installed'],
  datasets: [{
    data: [helixMacros.value.installed, helixMacros.value.not_installed],
    backgroundColor: ['#10b981', '#64748b']
  }]
}))

const moonrakerLocalityChartData = computed(() => ({
  labels: ['On the printer', 'Over the network'],
  datasets: [{
    data: [moonrakerLocality.value.local, moonrakerLocality.value.remote],
    backgroundColor: ['#3b82f6', '#f59e0b']
  }]
}))

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

.split-row {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: 16px;
}

.chart-note {
  margin-top: 12px;
  font-size: 13px;
  color: var(--text-secondary);
}

.chart-note.empty {
  padding: 24px 0;
  text-align: center;
}

.chart-note code {
  font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
  font-size: 12px;
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
