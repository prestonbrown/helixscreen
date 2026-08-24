// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Portable "place this static in external PSRAM .bss" attribute.
//
// On the ESP32 firmware (with CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y)
// this expands to EXT_RAM_BSS_ATTR, moving a large static out of scarce internal
// DRAM into external PSRAM. On every other platform it is empty, so the
// declaration is byte-identical.
//
// Use ONLY for large statics that are first touched at app runtime (after PSRAM
// is initialized) and are NEVER accessed from a DMA engine or ISR. App-state
// singletons and message/scratch buffers qualify; draw/DMA buffers and
// interrupt-touched data do NOT — those must stay internal.
#if defined(HELIX_PLATFORM_ESP32)
#include "esp_attr.h"
#define HELIX_PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define HELIX_PSRAM_BSS
#endif
