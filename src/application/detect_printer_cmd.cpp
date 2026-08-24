// SPDX-License-Identifier: GPL-3.0-or-later
#include "detect_printer_cmd.h"

#include "hv/requests.h"
#include "printer_detector.h"
#include "printer_discovery.h"

#include <spdlog/spdlog.h>

#include <cstdio>

#include "hv/json.hpp"

namespace helix::detect {

std::string format_detect_verdict(const PrinterDetectionResult& result,
                                  const std::string& runner_up_preset) {
    nlohmann::json j;
    j["model"] = result.type_name;
    j["preset"] = result.preset.empty() ? nlohmann::json(nullptr) : nlohmann::json(result.preset);
    j["confidence"] = result.confidence;
    j["runner_up_preset"] =
        runner_up_preset.empty() ? nlohmann::json(nullptr) : nlohmann::json(runner_up_preset);
    j["runner_up_confidence"] = result.runner_up_confidence;
    return j.dump();
}

} // namespace helix::detect

namespace {

nlohmann::json http_get_json(const std::string& url, int timeout_sec) {
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = url;
    req->timeout = timeout_sec;
    auto resp = requests::request(req);
    if (!resp || resp->status_code != 200)
        return nullptr;
    try {
        return nlohmann::json::parse(resp->body);
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace

namespace helix::detect {

void populate_discovery(helix::PrinterDiscovery& disc, const nlohmann::json& objects,
                        const nlohmann::json& info, const nlohmann::json& cfg) {
    disc.parse_objects(objects);

    // Hostname from /printer/info result object — guard against null values.
    // Looked up with find() rather than operator[]: on a const json the subscript
    // is only JSON_ASSERT-guarded, so a response that omits "hostname" aborts
    // where asserts are live and is undefined behaviour where they are not.
    if (info.is_object()) {
        const auto h = info.find("hostname");
        if (h != info.end() && h->is_string())
            disc.set_hostname(h->get<std::string>());
    }

    // configfile.settings block: kinematics + build volume.
    // parse_config_keys() already guards every field with .is_string()/.is_number(),
    // so null values in the configfile are silently skipped.
    if (cfg.is_object() && cfg.contains("configfile") && cfg["configfile"].contains("settings")) {
        const auto& s = cfg["configfile"]["settings"];
        disc.parse_config_keys(s);
        disc.parse_build_volume(s);
    }
}

int run_detect_printer(const std::string& host, int port) {
    const std::string base = "http://" + host + ":" + std::to_string(port);
    const int t = 3;

    nlohmann::json objs = http_get_json(base + "/printer/objects/list", t);
    if (!objs.is_object() || !objs.contains("result") || !objs["result"].contains("objects") ||
        !objs["result"]["objects"].is_array()) {
        spdlog::warn("[detect] Moonraker object list unavailable at {}", base);
        return 1;
    }

    nlohmann::json info_result;
    if (auto info = http_get_json(base + "/printer/info", t);
        info.is_object() && info.contains("result"))
        info_result = info["result"];

    nlohmann::json cfg_status;
    if (auto cfg = http_get_json(base + "/printer/objects/query?configfile=settings", t);
        cfg.is_object() && cfg.contains("result") && cfg["result"].contains("status"))
        cfg_status = cfg["result"]["status"];

    helix::PrinterDiscovery disc;
    try {
        populate_discovery(disc, objs["result"]["objects"], info_result, cfg_status);
    } catch (const std::exception& e) {
        spdlog::warn("[detect] JSON parsing failed, continuing with partial data: {}", e.what());
    }

    PrinterDetectionResult result = PrinterDetector::auto_detect(disc);
    std::string runner_up_preset = PrinterDetector::get_preset_for_name(result.runner_up_type_name);
    printf("%s\n", format_detect_verdict(result, runner_up_preset).c_str());
    return 0;
}

} // namespace helix::detect
