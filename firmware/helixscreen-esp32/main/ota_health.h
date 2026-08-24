// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Call once after the UI is up. On a normal boot: logs slot + version.
// On the first boot after an OTA (PENDING_VERIFY): marks the image valid so
// the bootloader won't roll back. "UI built" is the v1 health criterion;
// Plan 5 extends it (WiFi up, Moonraker reachable) before the confirm.
void ota_health_confirm(void);
