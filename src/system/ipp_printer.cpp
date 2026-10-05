// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "ipp_printer.h"

#include "ui_update_queue.h"

#include "http_executor.h"
#include "hv/requests.h"
#include "ipp_protocol.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "pwg_raster.h"
#include "sheet_label_layout.h"
#include "translation_loader.h"

#include <spdlog/spdlog.h>

namespace helix {

IppPrinter::IppPrinter() = default;
IppPrinter::~IppPrinter() = default;

std::string IppPrinter::name() const {
    return "IPP Printer";
}

void IppPrinter::set_target(const std::string& host, uint16_t port,
                            const std::string& resource_path) {
    host_ = host;
    port_ = port;
    resource_path_ = resource_path;
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
                                              label_count, start_position, bitmap, size,
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

            auto pwg_data = helix::pwg::generate(page_bitmap, params);
            spdlog::info("IPP Printer: generated {} bytes of PWG Raster data", pwg_data.size());

            // Build IPP request
            auto printer_uri = ipp::make_printer_uri(host, port, resource_path);
            auto media_keyword = label::get_pwg_media_keyword(paper_size);

            auto request = ipp::build_print_job(printer_uri, "HelixScreen Label",
                                                "image/pwg-raster", media_keyword);

            // Add job attributes
            request.begin_job_attributes();
            request.set_copies(1);

            // Encode IPP request with PWG document data
            auto ipp_body = request.encode_with_data(pwg_data);
            spdlog::debug("IPP Printer: encoded IPP request: {} bytes total", ipp_body.size());

            // HTTP POST via libhv
            auto url = fmt::format("http://{}:{}/{}", host, port, resource_path);

            auto req = std::make_shared<HttpRequest>();
            req->method = HTTP_POST;
            req->url = url;
            req->timeout = 30;
            req->SetHeader("Content-Type", "application/ipp");
            req->body.assign(reinterpret_cast<const char*>(ipp_body.data()), ipp_body.size());

            auto resp = requests::request(req);

            if (resp == nullptr) {
                fail(TR_NOOP("Connection failed to {}:{}"), host, port);
                return;
            }

            if (resp->status_code != 200) {
                fail(TR_NOOP("HTTP error: {} {}"), static_cast<int>(resp->status_code),
                     std::string(resp->status_message()));
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
