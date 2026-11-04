// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_analysis.h"

#include "async_lifetime_guard.h"
#include "core/gcode_filename.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "moonraker_error.h"
#include "print_start_inputs.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

#include "hv/json.hpp"

namespace helix {

namespace {

using json = nlohmann::json;

/// Tool numbers past this are not a real printer's; the palette is slot-indexed.
constexpr int kMaxTools = 256;

std::optional<double> number_at(const json& j, const char* key) {
    const json* v = json_util::find_member(j, key);
    if (v == nullptr || !v->is_number()) {
        return std::nullopt;
    }
    const double d = json_util::as_double(*v, NAN);
    if (!std::isfinite(d)) {
        return std::nullopt;
    }
    return d;
}

std::optional<std::pair<double, double>> point_of(const json& p) {
    if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) {
        return std::nullopt;
    }
    return std::make_pair(json_util::as_double(p[0]), json_util::as_double(p[1]));
}

FileAnalysisTool tool_of(const json& t) {
    FileAnalysisTool out;
    out.tool = json_util::safe_int(t, "tool", -1);
    out.mm = number_at(t, "mm");
    out.grams = number_at(t, "grams");
    out.color = json_util::safe_string(t, "color");
    out.material = json_util::safe_string(t, "material");
    return out;
}

FileAnalysisObject object_of(const json& o) {
    FileAnalysisObject out;
    out.name = json_util::safe_string(o, "name");
    if (const json* c = json_util::find_member(o, "center")) {
        out.center = point_of(*c);
    }
    if (const json* poly = json_util::find_member(o, "polygon"); poly && poly->is_array()) {
        for (const auto& p : *poly) {
            if (auto pt = point_of(p)) {
                out.polygon.push_back(*pt);
            }
        }
    }
    return out;
}

std::optional<FileAnalysisRecord> record_of(const json& r) {
    if (!r.is_object() || json_util::safe_int(r, "schema", 0) != kFileAnalysisSchema) {
        return std::nullopt;
    }
    FileAnalysisRecord rec;
    rec.filename = json_util::safe_string(r, "filename");
    rec.file_size = json_util::safe_uint64(r, "file_size", 0);
    rec.file_mtime = json_util::safe_double(r, "file_mtime", 0.0);
    if (const json* tools = json_util::find_member(r, "tools"); tools && tools->is_array()) {
        for (const auto& t : *tools) {
            FileAnalysisTool tool = tool_of(t);
            if (tool.tool >= 0 && tool.tool < kMaxTools) {
                rec.tools.push_back(std::move(tool));
            }
        }
        std::sort(
            rec.tools.begin(), rec.tools.end(),
            [](const FileAnalysisTool& a, const FileAnalysisTool& b) { return a.tool < b.tool; });
    }
    if (const json* objects = json_util::find_member(r, "objects")) {
        rec.objects_source = json_util::safe_string(*objects, "source", "none");
        if (const json* list = json_util::find_member(*objects, "list"); list && list->is_array()) {
            for (const auto& o : *list) {
                FileAnalysisObject obj = object_of(o);
                if (!obj.name.empty()) {
                    rec.objects.push_back(std::move(obj));
                }
            }
        }
    }
    if (const json* layers = json_util::find_member(r, "layers")) {
        rec.layer_count = json_util::safe_int(*layers, "count", 0);
        rec.layer_index = json_util::safe_string(*layers, "index");
    }
    rec.pauses = json_util::safe_int(r, "pauses", 0);
    for (const auto& t : rec.tools) {
        rec.fit.tools.insert(t.tool);
    }
    if (const json* bounds = json_util::find_member(r, "bounds")) {
        if (const json* b = json_util::find_member(*bounds, "print")) {
            rec.fit.print = fit_box_from_json(*b);
        }
        if (const json* b = json_util::find_member(*bounds, "travel")) {
            rec.fit.travel = fit_box_from_json(*b);
        }
        rec.fit.max_hotend_temp = number_at(*bounds, "max_hotend_temp");
        rec.fit.max_bed_temp = number_at(*bounds, "max_bed_temp");
    }
    if (const json* header = json_util::find_member(r, "header")) {
        rec.fit.printer_model = json_util::safe_string(*header, "printer_model");
        if (const json* n = json_util::find_member(*header, "nozzle_diameters");
            n && n->is_array()) {
            for (const auto& d : *n) {
                if (d.is_number()) {
                    rec.fit.nozzle_diameters.push_back(json_util::as_double(d));
                }
            }
        }
        // The slicer's spelling; one this check does not know reads as unknown.
        if (const json* t = json_util::find_member(*header, "nozzle_volume_types");
            t && t->is_array()) {
            for (const auto& v : *t) {
                rec.fit.nozzle_volume_types.push_back(
                    v.is_string() ? fit_nozzle_volume_type(v.get_ref<const std::string&>()) : "");
            }
        }
    }
    return rec;
}

/// @p values grown to hold slot @p index.
template <typename T> void ensure_slot(std::vector<T>& values, size_t index) {
    if (values.size() <= index) {
        values.resize(index + 1);
    }
}

} // namespace

FileAnalysisSource file_analysis_source(PluginFeatureState feature, const std::string& filename) {
    if (feature != PluginFeatureState::Ready || filename.empty() || gcode::is_3mf(filename)) {
        return FileAnalysisSource::Fallback;
    }
    return FileAnalysisSource::Plugin;
}

FileAnalysisReply parse_file_analysis_reply(const json& result) {
    FileAnalysisReply reply;
    const std::string status = json_util::safe_string(result, "status");
    if (status == "pending") {
        reply.status = FileAnalysisReply::Status::Pending;
    } else if (status == "disabled") {
        reply.status = FileAnalysisReply::Status::Disabled;
    } else if (status == "ready") {
        if (const json* r = json_util::find_member(result, "analysis")) {
            reply.record = record_of(*r);
        }
        reply.status =
            reply.record ? FileAnalysisReply::Status::Ready : FileAnalysisReply::Status::Invalid;
    }
    return reply;
}

void apply_file_analysis(FileStartFacts& facts, const FileAnalysisRecord& record) {
    facts.fit_record = record.fit;
    if (record.tools.empty()) {
        return;
    }
    std::set<int> used;
    bool all_grams = true;
    for (const auto& t : record.tools) {
        used.insert(t.tool);
        const auto slot = static_cast<size_t>(t.tool);
        if (!t.color.empty()) {
            ensure_slot(facts.palette.colors, slot);
            facts.palette.colors[slot] = t.color;
        }
        if (!t.material.empty()) {
            ensure_slot(facts.palette.materials, slot);
            facts.palette.materials[slot] = t.material;
        }
        if (!t.grams || *t.grams <= 0.0) {
            all_grams = false;
        }
    }
    facts.tools_used = std::move(used);
    if (all_grams) {
        std::vector<double> grams;
        for (const auto& t : record.tools) {
            ensure_slot(grams, static_cast<size_t>(t.tool));
            grams[static_cast<size_t>(t.tool)] = *t.grams;
        }
        facts.tool_grams = std::move(grams);
    }
}

void request_file_analysis(IMoonrakerAPI* api, const std::string& filename,
                           std::function<void(FileAnalysisReply)> on_reply) {
    api->get_client().send_jsonrpc(
        kFileAnalysisRpc, json{{"filename", filename}},
        [on_reply](const json& response) {
            const json* result = json_util::find_member(response, "result");
            on_reply(parse_file_analysis_reply(result != nullptr ? *result : json::object()));
        },
        [on_reply, filename](const MoonrakerError& error) {
            spdlog::debug("[FileAnalysis] No analysis for {}: {}", filename, error.message);
            on_reply(FileAnalysisReply{});
        },
        0, /*silent=*/true);
}

void with_layer_index_sidecar(IMoonrakerAPI* api, PluginFeatureState feature,
                              const std::string& filename, bool wanted, const LifetimeToken& tok,
                              std::function<void(std::string sidecar)> load) {
    // Callable from any thread: `load` runs only through the token's defer.
    auto answer = [tok, load](std::string sidecar) {
        tok.defer("FileAnalysis::layer_index_sidecar",
                  [load, sidecar = std::move(sidecar)]() mutable { load(std::move(sidecar)); });
    };
    if (!wanted || api == nullptr ||
        file_analysis_source(feature, filename) != FileAnalysisSource::Plugin) {
        load({});
        return;
    }
    request_file_analysis(api, filename, [api, answer, filename](FileAnalysisReply reply) {
        if (reply.status != FileAnalysisReply::Status::Ready || !reply.record ||
            reply.record->layer_index.empty()) {
            answer({});
            return;
        }
        api->transfers().download_file(
            "gcodes", reply.record->layer_index,
            [answer](const std::string& content) { answer(content); },
            [answer, filename](const MoonrakerError& error) {
                spdlog::debug("[FileAnalysis] No layer index for {}: {}", filename, error.message);
                answer({});
            });
    });
}

} // namespace helix
