// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "ipp_printer.h"

#include "ui_update_queue.h"

#include "http_executor.h"
#include "hv/HttpClient.h"
#include "ipp_protocol.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "pwg_raster.h"
#include "sheet_label_layout.h"
#include "translation_loader.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace helix {

namespace ipp {

std::shared_ptr<HttpResponse> post_request(const std::string& host, uint16_t port,
                                           const std::string& resource_path,
                                           const std::vector<uint8_t>& body, int timeout_s,
                                           Transport transport) {
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_POST;
    req->url = fmt::format("{}://{}:{}/{}", transport == Transport::Tls ? "https" : "http", host,
                           port, resource_path);
    req->timeout = timeout_s;
    req->SetHeader("Content-Type", "application/ipp");
    req->body.assign(reinterpret_cast<const char*>(body.data()), body.size());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    auto resp = std::make_shared<HttpResponse>();
    hv::HttpClient client;
    if (client.send(req.get(), resp.get()) != 0) {
        return nullptr;
    }
    // 101 Switching Protocols is a final answer to an Upgrade, not an interim one.
    // libhv has no per-read timeout for recvResponse(): each read keeps the socket
    // receive timeout send() left behind, at most timeout_s, so a printer that
    // stalls after an interim response holds this thread under 2 * timeout_s.
    while (resp->status_code >= 100 && resp->status_code < 200 &&
           resp->status_code != HTTP_STATUS_SWITCHING_PROTOCOLS) {
        if (std::chrono::steady_clock::now() >= deadline) {
            spdlog::error("IPP Printer: no final response within {}s", timeout_s);
            return nullptr;
        }
        spdlog::debug("IPP Printer: skipping interim HTTP {}", static_cast<int>(resp->status_code));
        if (client.recvResponse(resp.get()) != 0) {
            return nullptr;
        }
    }
    return resp;
}

} // namespace ipp

IppPrinter::IppPrinter() = default;
IppPrinter::~IppPrinter() = default;

std::string IppPrinter::name() const {
    return "IPP Printer";
}

void IppPrinter::set_target(const std::string& host, uint16_t port,
                            const std::string& resource_path) {
    if (host != host_ || port != port_) {
        needs_tls_ = std::make_shared<std::atomic<bool>>(false);
    }
    host_ = host;
    port_ = port;
    resource_path_ = ipp::normalize_resource_path(resource_path);
}

void IppPrinter::set_sheet_template(int index) {
    sheet_template_index_ = index;
}

void IppPrinter::set_label_count(int count) {
    label_count_ = count;
}

void IppPrinter::set_start_position(int start) {
    start_position_ = start;
}

std::vector<LabelSize> IppPrinter::supported_sizes() const {
    return supported_sizes_static();
}

std::vector<LabelSize> IppPrinter::supported_sizes_static() {
    const auto& templates = label::get_sheet_templates();
    std::vector<LabelSize> sizes;
    sizes.reserve(templates.size());
    for (const auto& tmpl : templates) {
        sizes.push_back(label::sheet_template_to_label_size(tmpl));
    }
    return sizes;
}

void IppPrinter::print(const LabelBitmap& bitmap, const LabelSize& size, PrintCallback callback) {
    // Capture all state by copy for the detached thread
    auto host = host_;
    auto port = port_;
    auto resource_path = resource_path_;
    auto template_index = sheet_template_index_;
    auto label_count = label_count_;
    auto start_position = start_position_;
    auto needs_tls = needs_tls_;

    if (host.empty()) {
        spdlog::error("IPP Printer: no target host configured");
        helix::ui::queue_update("IppPrinter::print", [callback]() {
            callback(false, lv_tr("No target printer configured"));
        });
        return;
    }

    spdlog::warn("IPP Printer: printing to {}:{}/{}, template={}, count={}, start={}", host, port,
                 resource_path, template_index, label_count, start_position);

    // Route through HttpExecutor::fast() bounded pool — raw std::thread spawn
    // crashes with std::terminate on AD5M under thread exhaustion (#724, #837).
    helix::http::HttpExecutor::fast().submit([host, port, resource_path, template_index,
                                              label_count, start_position, needs_tls, bitmap, size,
                                              callback]() {
        // The callback is UI-facing, so it and its message's translation run on the main thread.
        auto fail = [callback](const char* fmt_tag, auto... args) {
            static_assert(!(std::is_pointer_v<decltype(args)> || ...), "pass std::string");
            spdlog::error("IPP Printer: {}", fmt::format(fmt::runtime(fmt_tag), args...));
            helix::ui::queue_update("IppPrinter::print", [callback, fmt_tag, args...]() {
                callback(false, fmt::format(fmt::runtime(lv_tr(fmt_tag)), args...));
            });
        };
        try {
            // Get the sheet template
            const auto& templates = label::get_sheet_templates();
            if (template_index < 0 || template_index >= static_cast<int>(templates.size())) {
                fail(TR_NOOP("Invalid sheet template index: {} (have {})"), template_index,
                     templates.size());
                return;
            }
            const auto& tmpl = templates[template_index];
            auto paper_size = tmpl.paper;

            spdlog::debug("IPP Printer: using template '{}' ({}x{} grid, {} paper)", tmpl.name,
                          tmpl.columns, tmpl.rows,
                          paper_size == label::PaperSize::LETTER ? "Letter" : "A4");

            // Tile labels onto a full page
            constexpr int dpi = 300;
            auto page_bitmap =
                label::tile_labels_on_page(bitmap, tmpl, label_count, start_position, dpi);

            // Get page dimensions for PWG header
            auto page_dims = label::get_page_dimensions(paper_size, dpi);

            // Build PWG Raster data
            helix::pwg::PageParams params;
            params.hw_resolution_x = dpi;
            params.hw_resolution_y = dpi;
            params.width_px = static_cast<uint32_t>(page_dims.width_px);
            params.height_px = static_cast<uint32_t>(page_dims.height_px);
            params.color_space = helix::pwg::ColorSpace::SGRAY;
            params.bits_per_color = 8;
            params.bits_per_pixel = 8;
            params.num_colors = 1;
            auto media_keyword = label::get_pwg_media_keyword(paper_size);
            params.page_size_name = media_keyword;

            auto pwg_data = helix::pwg::generate(page_bitmap, params);
            spdlog::info("IPP Printer: generated {} bytes of PWG Raster data", pwg_data.size());

            auto send_job = [&](ipp::Transport transport) {
                auto printer_uri = ipp::make_printer_uri(host, port, resource_path, transport);
                auto request = ipp::build_print_job(printer_uri, "HelixScreen Label",
                                                    "image/pwg-raster", media_keyword);
                request.begin_job_attributes();
                request.set_copies(1);

                // Encode IPP request with PWG document data
                auto ipp_body = request.encode_with_data(pwg_data);
                spdlog::debug("IPP Printer: encoded IPP request: {} bytes total", ipp_body.size());
                return ipp::post_request(host, port, resource_path, ipp_body, 30, transport);
            };

            auto transport = needs_tls->load() ? ipp::Transport::Tls : ipp::Transport::Plain;
            auto resp = send_job(transport);

            // 426 is a printer that accepts jobs only encrypted: IPPS on the same port.
            if (transport == ipp::Transport::Plain && resp != nullptr &&
                resp->status_code == HTTP_STATUS_UPGRADE_REQUIRED) {
                spdlog::info("IPP Printer: {}:{} requires encryption, resending over IPPS", host,
                             port);
                transport = ipp::Transport::Tls;
                resp = send_job(transport);
            }
            const bool encrypted = transport == ipp::Transport::Tls;
            if (encrypted) {
                needs_tls->store(resp != nullptr);
            }

            if (resp == nullptr) {
                if (encrypted) {
                    fail(TR_NOOP("The printer requires an encrypted connection, but the "
                                 "encrypted connection to {}:{} failed"),
                         host, port);
                } else {
                    fail(TR_NOOP("Connection failed to {}:{}"), host, port);
                }
                return;
            }

            if (resp->status_code != 200) {
                if (encrypted) {
                    fail(TR_NOOP("The printer requires an encrypted connection, but it "
                                 "rejected the encrypted job: HTTP {} {}"),
                         static_cast<int>(resp->status_code), std::string(resp->status_message()));
                } else {
                    fail(TR_NOOP("HTTP error: {} {}"), static_cast<int>(resp->status_code),
                         std::string(resp->status_message()));
                }
                return;
            }

            // Parse IPP response
            auto ipp_resp = ipp::parse_response(reinterpret_cast<const uint8_t*>(resp->body.data()),
                                                resp->body.size());

            if (!ipp_resp.is_success()) {
                fail(TR_NOOP("IPP error: {}"), ipp_resp.status_message());
                return;
            }

            spdlog::warn("IPP Printer: print job submitted successfully");
            helix::ui::queue_update("IppPrinter::print", [callback]() { callback(true, ""); });

        } catch (const std::exception& e) {
            fail(TR_NOOP("Exception: {}"), std::string(e.what()));
        }
    });
}

} // namespace helix

#endif // HELIX_HAS_LABEL_PRINTER
