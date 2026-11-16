<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Fleet</h2>
      </div>

      <FleetSizeSection />
      <PlatformsSection />
      <VersionsSection />
      <BoardsSection />
      <InstallSourceSection />
      <ModelsSection />
      <HardwareSection />
      <RetentionSection />
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import AppLayout from '@/components/AppLayout.vue'
import FleetSizeSection from '@/components/sections/FleetSizeSection.vue'
import PlatformsSection from '@/components/sections/PlatformsSection.vue'
import VersionsSection from '@/components/sections/VersionsSection.vue'
import BoardsSection from '@/components/sections/BoardsSection.vue'
import InstallSourceSection from '@/components/sections/InstallSourceSection.vue'
import ModelsSection from '@/components/sections/ModelsSection.vue'
import HardwareSection from '@/components/sections/HardwareSection.vue'
import RetentionSection from '@/components/sections/RetentionSection.vue'
</script>
