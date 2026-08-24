// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compile-only drift protection: if helix::IMoonrakerClient gains a pure-virtual
// method and neither helix::MoonrakerClient nor MoonrakerClientMock provides it,
// MoonrakerClientMock becomes abstract and this fails to build.

#include "i_moonraker_client.h"

#include "../catch_amalgamated.hpp"

#ifdef HELIX_ENABLE_MOCKS
#include "moonraker_client_mock.h"

#include <type_traits>

TEST_CASE("MoonrakerClientMock satisfies helix::IMoonrakerClient interface", "[compile][drift]") {
    static_assert(std::is_base_of_v<helix::IMoonrakerClient, MoonrakerClientMock>,
                  "MoonrakerClientMock must derive from helix::IMoonrakerClient");
    static_assert(!std::is_abstract_v<MoonrakerClientMock>,
                  "MoonrakerClientMock must implement every pure virtual from IMoonrakerClient");
    SUCCEED("IMoonrakerClient ↔ MoonrakerClientMock parity verified at compile time");
}

// Pins: helix::IMoonrakerClient exposes the full consumer surface (Plan 3 Task 2).
// clang-format off
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::register_notify_update),
                             helix::SubscriptionId (helix::IMoonrakerClient::*)(std::function<void(const json&)>)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::unsubscribe_notify_update),
                             bool (helix::IMoonrakerClient::*)(helix::SubscriptionId)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::register_method_callback),
                             void (helix::IMoonrakerClient::*)(const std::string&, const std::string&,
                                                               std::function<void(const json&)>)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::unregister_method_callback),
                             bool (helix::IMoonrakerClient::*)(const std::string&, const std::string&)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::dispatch_status_update),
                             void (helix::IMoonrakerClient::*)(const json&, bool)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::get_connection_state),
                             helix::ConnectionState (helix::IMoonrakerClient::*)() const>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::add_connected_observer),
                             void (helix::IMoonrakerClient::*)(const std::string&, std::function<void()>)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::remove_connected_observer),
                             bool (helix::IMoonrakerClient::*)(const std::string&)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::force_reconnect),
                             void (helix::IMoonrakerClient::*)()>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::register_event_handler),
                             void (helix::IMoonrakerClient::*)(helix::MoonrakerEventCallback)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::suppress_disconnect_modal),
                             void (helix::IMoonrakerClient::*)(uint32_t)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::is_disconnect_modal_suppressed),
                             bool (helix::IMoonrakerClient::*)() const>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::hardware),
                             helix::PrinterDiscovery (helix::IMoonrakerClient::*)() const>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::parse_objects),
                             void (helix::IMoonrakerClient::*)(const json&)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::clear_discovery_cache),
                             void (helix::IMoonrakerClient::*)()>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::cancel_request),
                             bool (helix::IMoonrakerClient::*)(helix::RequestId)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::set_state_change_callback),
                             void (helix::IMoonrakerClient::*)(
                                 std::function<void(helix::ConnectionState, helix::ConnectionState)>)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::set_connection_timeout),
                             void (helix::IMoonrakerClient::*)(uint32_t)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::set_default_request_timeout),
                             void (helix::IMoonrakerClient::*)(uint32_t)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::configure_timeouts),
                             void (helix::IMoonrakerClient::*)(uint32_t, uint32_t, uint32_t, uint32_t,
                                                               uint32_t)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::process_timeouts),
                             void (helix::IMoonrakerClient::*)()>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::lifetime_weak),
                             std::weak_ptr<bool> (helix::IMoonrakerClient::*)() const>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::get_last_url),
                             const std::string& (helix::IMoonrakerClient::*)() const>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::set_auto_reconnect),
                             void (helix::IMoonrakerClient::*)(bool)>);
static_assert(
    std::is_same_v<decltype(&helix::IMoonrakerClient::set_on_hardware_discovered),
                   void (helix::IMoonrakerClient::*)(
                       std::function<void(const helix::PrinterDiscovery&)>)>);
static_assert(
    std::is_same_v<decltype(&helix::IMoonrakerClient::set_on_discovery_complete),
                   void (helix::IMoonrakerClient::*)(
                       std::function<void(const helix::PrinterDiscovery&, const json&)>)>);
static_assert(std::is_same_v<decltype(&helix::IMoonrakerClient::set_bed_mesh_callback),
                             void (helix::IMoonrakerClient::*)(std::function<void(const json&)>)>);
// clang-format on
#endif
