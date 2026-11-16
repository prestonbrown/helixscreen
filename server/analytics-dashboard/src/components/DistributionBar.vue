<template>
  <div class="chart-section">
    <h3>{{ title }}</h3>
    <ChartCaption :text="caption" />
    <NoData v-if="rows.length === 0" :era="era" />
    <BarChart
      v-else
      :data="chartData"
      :options="options"
      :height="barChartHeight(rows.length)"
      @select="onSelect"
    />
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useRouter } from 'vue-router'
import type { ChartOptions } from 'chart.js'
import BarChart from '@/components/BarChart.vue'
import ChartCaption from '@/components/ChartCaption.vue'
import NoData from '@/components/NoData.vue'
import type { DeltaEra } from '@/services/api'
import { useFiltersStore } from '@/stores/filters'
import { horizontalBarOpts } from '@/utils/chart'
import { rowHint, rowLabel } from '@/utils/drill'
import { barChartHeight, seriesColor } from '@/utils/series'
import type { DashFilterKey } from '@/utils/urlState'
import type { Grouped } from '@/utils/versions'

const props = withDefaults(
  defineProps<{
    rows: Grouped[]
    /** Omitted for a dimension the dashboard filters cannot express: bars are then not clickable. */
    filterKey?: DashFilterKey
    caption: string
    title: string
    /** Dataset label shown in the tooltip. */
    valueLabel?: string
    era?: DeltaEra | null
  }>(),
  { valueLabel: 'Devices', era: null }
)

const filters = useFiltersStore()
const router = useRouter()

const chartData = computed(() => ({
  labels: props.rows.map(rowLabel),
  datasets: [{
    label: props.valueLabel,
    data: props.rows.map((r) => r.count),
    backgroundColor: props.rows.map((_, i) => seriesColor(i))
  }]
}))

const options = computed((): ChartOptions<'bar'> => ({
  ...horizontalBarOpts,
  plugins: {
    legend: { display: false },
    tooltip: {
      callbacks: {
        afterLabel: (ctx) => rowHint(props.rows[ctx.dataIndex])
      }
    }
  },
  onHover: (evt, elements) => {
    const target = evt.native?.target
    if (target instanceof HTMLElement) target.style.cursor = elements.length && props.filterKey ? 'pointer' : 'default'
  }
}))

function onSelect(index: number) {
  const row = props.rows[index]
  if (row && props.filterKey) filters.activate(router, [[props.filterKey, row]])
}
</script>
