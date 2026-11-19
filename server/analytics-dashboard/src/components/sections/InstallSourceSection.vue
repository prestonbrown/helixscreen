<template>
  <DashSection title="Install sources" :loading="loading" :error="error">
    <DistributionBar
      v-if="data"
      :rows="ungrouped(data.install_sources)"
      title="Installed Through"
      :caption="`Distinct devices with a session, ${span}. Unknown: no record of the channel, including installs that predate it`"
    />
    <DistributionBar
      v-if="data"
      :rows="ungrouped(data.host_mods)"
      title="Host Firmware Mod"
      :caption="`Distinct devices with a session, ${span}`"
    />
  </DashSection>
</template>

<script setup lang="ts">
import DashSection from '@/components/DashSection.vue'
import DistributionBar from '@/components/DistributionBar.vue'
import { useDashData } from '@/composables/useDashData'
import { api } from '@/services/api'
import { ungrouped } from '@/utils/versions'

const { data, loading, error, span } = useDashData(api.getAdoption)
</script>
