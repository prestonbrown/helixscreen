// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace helix::bluetooth {

struct RfcommSendResult {
    bool success = false;
    std::string error;
    std::vector<uint8_t> response; ///< Response data (for send_receive)
};

/// Send raw data over RFCOMM to a Bluetooth device.
/// Handles the full lifecycle: init context, connect, write loop, drain, disconnect, deinit.
/// Thread-safe: serializes all RFCOMM prints through a shared mutex.
/// @param mac              Bluetooth MAC address (e.g. "AA:BB:CC:DD:EE:FF")
/// @param fallback_channel RFCOMM channel to use if resolver cache+SDP fail
/// @param data             Raw bytes to send
/// @param log_tag          Short prefix for log messages (e.g. "Phomemo BT")
RfcommSendResult rfcomm_send(const std::string& mac, int fallback_channel,
                             const std::vector<uint8_t>& data, const std::string& log_tag);

/// Send data over RFCOMM and read a response.
/// Same lifecycle as rfcomm_send but reads up to `response_len` bytes after sending.
RfcommSendResult rfcomm_send_receive(const std::string& mac, int fallback_channel,
                                     const std::vector<uint8_t>& data, size_t response_len,
                                     const std::string& log_tag);

} // namespace helix::bluetooth

namespace helix::label {

/// SPP (Serial Port Profile) UUID-16 — used for SDP channel lookup on
/// classic Bluetooth printers that expose an RFCOMM service record.
constexpr uint16_t SPP_UUID16 = 0x1101;

/// Resolve the RFCOMM channel for a label printer MAC.
/// Consults LabelPrinterSettingsManager cache; on miss runs SDP and caches
/// the result.
/// @param mac              Printer MAC (e.g. "3A:10:AD:FF:1A:77")
/// @param fallback_channel Used if SDP fails AND cache is empty (0 = no fallback)
/// @return Channel (1..30) or -1 on total failure.
int resolve_label_printer_channel(const std::string& mac, int fallback_channel);

} // namespace helix::label
