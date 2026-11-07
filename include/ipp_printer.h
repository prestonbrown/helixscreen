// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ipp_protocol.h"
#include "label_printer.h"
#include "sheet_label_layout.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class HttpResponse; // NAMESPACE_OK: libhv declares it globally

namespace helix {

namespace ipp {

/// POST an encoded IPP request to http://host:port/resource_path (https:// for
/// Transport::Tls, accepting the printer's self-signed certificate) and return the final
/// HTTP response. Interim 1xx responses are read past (RFC 9110 15.2): some printers
/// send an unsolicited 100 Continue before the real answer. nullptr when the connection
/// fails or no final response arrives.
std::shared_ptr<HttpResponse> post_request(const std::string& host, uint16_t port,
                                           const std::string& resource_path,
                                           const std::vector<uint8_t>& body, int timeout_s,
                                           Transport transport = Transport::Plain);

} // namespace ipp

/**
 * @brief IPP sheet label printer backend
 *
 * Prints filament spool labels on sheets of Avery-style labels using
 * standard IPP printers (inkjet/laser). Sends PWG Raster over HTTP POST.
 *
 * Thread safety: print() runs async on a detached thread. Callbacks
 * are dispatched to the UI thread via queue_update().
 */
class IppPrinter : public ILabelPrinter {
  public:
    IppPrinter();
    ~IppPrinter() override;

    // Non-copyable
    IppPrinter(const IppPrinter&) = delete;
    IppPrinter& operator=(const IppPrinter&) = delete;

    // === ILabelPrinter interface ===

    [[nodiscard]] std::string name() const override;
    void print(const LabelBitmap& bitmap, const LabelSize& size, PrintCallback callback) override;
    [[nodiscard]] std::vector<LabelSize> supported_sizes() const override;

    // === IPP-specific API ===

    /// Set the target printer (host, port, resource path from mDNS TXT "rp").
    /// The path is normalized; an empty one targets ipp::kDefaultResourcePath.
    /// A new host or port forgets that the previous one needed IPPS.
    void set_target(const std::string& host, uint16_t port, const std::string& resource_path);

    /// The normalized resource path print() posts to
    [[nodiscard]] const std::string& resource_path() const {
        return resource_path_;
    }

    /// Set the sheet template index (into get_sheet_templates())
    void set_sheet_template(int index);

    /// Set number of labels to print per sheet (0 = fill entire sheet)
    void set_label_count(int count);

    /// Set start position on sheet (0-based, skip already-used labels)
    void set_start_position(int start);

    /// Get supported sizes (returns LabelSize for each sheet template)
    static std::vector<LabelSize> supported_sizes_static();

  private:
    std::string host_;
    uint16_t port_ = 631;
    std::string resource_path_ = ipp::kDefaultResourcePath;
    int sheet_template_index_ = 0;
    int label_count_ = 1;
    int start_position_ = 0;
    /// Set once this target answered 426 and then took the job over IPPS, so later jobs
    /// go straight to IPPS; cleared when an IPPS job cannot connect. Shared with the
    /// job's worker thread, and replaced (not reset) on a new target so a job still
    /// running for the old one cannot mark the new one.
    std::shared_ptr<std::atomic<bool>> needs_tls_ = std::make_shared<std::atomic<bool>>(false);
};

} // namespace helix
