// SPDX-License-Identifier: GPL-3.0-or-later

#include "vendor_state_push.h"

#include "app_globals.h"
#include "i_moonraker_client.h"
#include "printer_state.h"

#include <utility>

#include "hv/json.hpp"

namespace helix {

using json = nlohmann::json;

VendorStateSource vendor_state_source(PluginFeatureState feature) {
    return feature == PluginFeatureState::Ready ? VendorStateSource::Push : VendorStateSource::Poll;
}

std::optional<json> vendor_state_payload(const json& notification, const std::string& vendor) {
    auto params = notification.find("params");
    if (params == notification.end() || !params->is_array() || params->empty()) {
        return std::nullopt;
    }
    const json& message = (*params)[0];
    if (!message.is_object()) {
        return std::nullopt;
    }
    auto name = message.find("vendor");
    auto payload = message.find("payload");
    if (name == message.end() || !name->is_string() ||
        name->get_ref<const std::string&>() != vendor || payload == message.end()) {
        return std::nullopt;
    }
    return *payload;
}

bool VendorStateTracker::update(PluginFeatureState feature, int probe_seq, uint64_t connection) {
    const bool new_probe = probe_seq != probe_seq_;
    if (new_probe) {
        probe_seq_ = probe_seq;
        probe_connection_ = connection;
    }
    const bool push =
        vendor_state_source(feature) == VendorStateSource::Push && probe_connection_ == connection;
    const bool was_pushing = pushing_.exchange(push);
    return push && (!was_pushing || new_probe);
}

void VendorStateTracker::reset() {
    pushing_.store(false);
    probe_seq_ = -1;
    probe_connection_ = 0;
}

PluginFeatureProbe current_plugin_feature(const std::string& feature, int min_version) {
    PrinterState& ps = get_printer_state();
    if (!ps.are_subjects_initialized()) {
        return {};
    }
    PrinterPluginStatusState& status = ps.plugin_status_state();
    return {status.plugin_feature(feature, min_version),
            lv_subject_get_int(status.get_helix_plugin_probe_seq_subject())};
}

void subscribe_vendor_state(IMoonrakerClient& client, const std::string& handler_name,
                            const std::string& vendor, LifetimeToken token,
                            std::function<void(const json&)> on_payload) {
    client.register_method_callback(
        kVendorStateNotifyMethod, handler_name,
        [vendor, token, on_payload = std::move(on_payload)](const json& msg) {
            // BG THREAD: pick this vendor's payload out of the envelope here,
            // touching nothing but the message.
            std::optional<json> payload = vendor_state_payload(msg, vendor);
            if (!payload) {
                return;
            }
            token.defer("vendor_state_push",
                        [on_payload, payload = std::move(*payload)]() { on_payload(payload); });
        });
}

void unsubscribe_vendor_state(IMoonrakerClient& client, const std::string& handler_name) {
    client.unregister_method_callback(kVendorStateNotifyMethod, handler_name);
}

} // namespace helix
