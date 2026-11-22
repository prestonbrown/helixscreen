// SPDX-License-Identifier: GPL-3.0-or-later
//
// esp_restart_noos resets the other CPU wherever it is, stalls it, then
// disables the caches. If that CPU was mid-access to PSRAM (a task stack in
// PSRAM, an ISR copying from the framebuffer), the restart can hang until the
// RTC watchdog resets the chip, which then boots as RTCWDT_RTC_RST instead of
// a software reset (prestonbrown/helixscreen#1752). Parking that CPU first, in
// a task spinning in IRAM with its interrupts off, leaves it with no external
// memory access in flight when it is reset.
//
// The parker is a task rather than an IPC ISR stall: a task only runs between
// critical sections, so the parked CPU holds no spinlock the restarting CPU
// could then wait on.

#include "restart_quiesce.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdbool.h>

static const char* TAG = "restart";

#define PARK_STACK_BYTES 2048
#define PARK_WAIT_US 20000

static volatile bool s_parked;

static void IRAM_ATTR park_task(void* arg) {
    (void)arg;
    portDISABLE_INTERRUPTS();
    s_parked = true;
    for (;;) {
    }
}

// Parks the CPU the caller is not on. An unpinned caller that migrates after
// this read is still safe: the parked CPU never schedules again, so the caller
// carries on, and esp_restart_noos runs, on the other one.
// Logs only before the park: a task on the parked CPU may hold the log lock.
static void restart_quiesce_shutdown(void) {
    const int cpu = xPortGetCoreID() == 0 ? 1 : 0;
    if (xTaskCreatePinnedToCoreWithCaps(park_task, "rst_park", PARK_STACK_BYTES, NULL,
                                        configMAX_PRIORITIES - 1, NULL, cpu,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "no park task; restarting with CPU %d live", cpu);
        return;
    }
    const int64_t deadline = esp_timer_get_time() + PARK_WAIT_US;
    while (!s_parked && esp_timer_get_time() < deadline) {
    }
}

void restart_quiesce_register(void) {
#if CONFIG_FREERTOS_UNICORE
    return;
#endif
    if (esp_register_shutdown_handler(restart_quiesce_shutdown) != ESP_OK) {
        ESP_LOGW(TAG, "no restart quiesce handler; esp_restart() runs with both CPUs live");
    }
}
