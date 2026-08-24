// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file bt_agent.cpp
 * @brief BlueZ Agent1 registration for "Just Works" pairing.
 *
 * Without a registered agent, BlueZ completes Pair() at the protocol level
 * but does NOT exchange link keys — the device ends up paired but not bonded,
 * and the kernel's input plugin refuses to create an HID evdev node
 * (ClassicBondedOnly=true by default).
 *
 * bluetoothctl works because it registers an agent automatically. We need to
 * do the same. The agent uses "NoInputNoOutput" capability (appropriate for a
 * headless kiosk device) and auto-accepts all pairing requests.
 */

#include "bluetooth_plugin.h"
#include "bt_context.h"

#include <cstdio>
#include <cstring>

static const char* AGENT_PATH = "/helix/bt/agent";

// --- Agent1 method handlers (auto-accept everything) ---

static int agent_release(sd_bus_message* m, void* /*userdata*/, sd_bus_error* /*error*/) {
    fprintf(stderr, "[bt] agent: Release called\n");
    return sd_bus_reply_method_return(m, "");
}

static int agent_request_confirmation(sd_bus_message* m, void* /*userdata*/,
                                      sd_bus_error* /*error*/) {
    const char* device = nullptr;
    uint32_t passkey = 0;
    sd_bus_message_read(m, "ou", &device, &passkey);
    fprintf(stderr, "[bt] agent: RequestConfirmation for %s passkey=%u — auto-accepting\n",
            device ? device : "(null)", passkey);
    return sd_bus_reply_method_return(m, "");
}

static int agent_request_authorization(sd_bus_message* m, void* /*userdata*/,
                                       sd_bus_error* /*error*/) {
    const char* device = nullptr;
    sd_bus_message_read(m, "o", &device);
    fprintf(stderr, "[bt] agent: RequestAuthorization for %s — auto-accepting\n",
            device ? device : "(null)");
    return sd_bus_reply_method_return(m, "");
}

static int agent_authorize_service(sd_bus_message* m, void* /*userdata*/, sd_bus_error* /*error*/) {
    const char* device = nullptr;
    const char* uuid = nullptr;
    sd_bus_message_read(m, "os", &device, &uuid);
    fprintf(stderr, "[bt] agent: AuthorizeService for %s uuid=%s — auto-accepting\n",
            device ? device : "(null)", uuid ? uuid : "(null)");
    return sd_bus_reply_method_return(m, "");
}

static int agent_cancel(sd_bus_message* m, void* /*userdata*/, sd_bus_error* /*error*/) {
    fprintf(stderr, "[bt] agent: Cancel called\n");
    return sd_bus_reply_method_return(m, "");
}

// clang-format off
static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release",              "",   "", agent_release,              SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation",  "ou", "", agent_request_confirmation, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o",  "", agent_request_authorization,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService",     "os", "", agent_authorize_service,    SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel",               "",   "", agent_cancel,              SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};
// clang-format on

extern "C" int helix_bt_register_agent(helix_bt_context* ctx) {
    if (!ctx || !ctx->bus || !ctx->bus_thread)
        return -EINVAL;

    int r = 0;

    try {
        ctx->bus_thread->run_sync([&](sd_bus* bus) {
            // Export the Agent1 object
            r = sd_bus_add_object_vtable(bus, &ctx->agent_slot, AGENT_PATH, "org.bluez.Agent1",
                                         agent_vtable, ctx);
            if (r < 0) {
                fprintf(stderr, "[bt] agent: failed to add vtable: %s\n", strerror(-r));
                return;
            }

            // Register with AgentManager1
            sd_bus_error error = SD_BUS_ERROR_NULL;
            r = sd_bus_call_method(bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1",
                                   "RegisterAgent", &error, nullptr, "os", AGENT_PATH,
                                   "NoInputNoOutput");
            if (r < 0) {
                fprintf(stderr, "[bt] agent: RegisterAgent failed: %s\n",
                        error.message ? error.message : strerror(-r));
                sd_bus_error_free(&error);
                sd_bus_slot_unref(ctx->agent_slot);
                ctx->agent_slot = nullptr;
                return;
            }
            sd_bus_error_free(&error);

            // Make us the default agent
            error = SD_BUS_ERROR_NULL;
            r = sd_bus_call_method(bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1",
                                   "RequestDefaultAgent", &error, nullptr, "o", AGENT_PATH);
            if (r < 0) {
                fprintf(stderr, "[bt] agent: RequestDefaultAgent failed: %s\n",
                        error.message ? error.message : strerror(-r));
                sd_bus_error_free(&error);
                // Non-fatal — RegisterAgent already succeeded, we just won't
                // be the default. Pairing we initiate will still use our agent.
            } else {
                sd_bus_error_free(&error);
            }

            fprintf(stderr, "[bt] agent: registered at %s (NoInputNoOutput)\n", AGENT_PATH);
        });
    } catch (const std::exception& e) {
        fprintf(stderr, "[bt] agent: exception during registration: %s\n", e.what());
        r = -EIO;
    }

    return r;
}

extern "C" void helix_bt_unregister_agent(helix_bt_context* ctx) {
    if (!ctx || !ctx->bus || !ctx->bus_thread)
        return;

    try {
        ctx->bus_thread->run_sync([ctx](sd_bus* bus) {
            sd_bus_error error = SD_BUS_ERROR_NULL;
            sd_bus_call_method(bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1",
                               "UnregisterAgent", &error, nullptr, "o", AGENT_PATH);
            sd_bus_error_free(&error);

            if (ctx->agent_slot) {
                sd_bus_slot_unref(ctx->agent_slot);
                ctx->agent_slot = nullptr;
            }

            fprintf(stderr, "[bt] agent: unregistered\n");
        });
    } catch (const std::exception&) {
        // Best-effort during shutdown
        if (ctx->agent_slot) {
            sd_bus_slot_unref(ctx->agent_slot);
            ctx->agent_slot = nullptr;
        }
    }
}
