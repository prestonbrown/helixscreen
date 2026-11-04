// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "three_mf.h"

#include "core/gcode_filename.h"
#include "core/text_io.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace helix::three_mf {

namespace {

constexpr std::string_view kPlatePrefix = "Metadata/plate_";
constexpr std::string_view kGcodeSuffix = ".gcode";

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// The value of attribute @p name inside one tag's text, or nullopt. Tolerates
/// space around '=' and either quote.
std::optional<std::string_view> tag_attribute(std::string_view tag, std::string_view name) {
    size_t at = 0;
    while ((at = tag.find(name, at)) != std::string_view::npos) {
        const bool starts_word = at > 0 && is_space(tag[at - 1]);
        size_t p = at + name.size();
        at = p;
        if (!starts_word) {
            continue;
        }
        while (p < tag.size() && is_space(tag[p])) {
            ++p;
        }
        if (p >= tag.size() || tag[p] != '=') {
            continue;
        }
        ++p;
        while (p < tag.size() && is_space(tag[p])) {
            ++p;
        }
        if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) {
            continue;
        }
        const char quote = tag[p++];
        const size_t close = tag.find(quote, p);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        return tag.substr(p, close - p);
    }
    return std::nullopt;
}

double attr_double(std::string_view tag, std::string_view name) {
    const auto v = tag_attribute(tag, name);
    return v ? text_io::parse_leading<double>(*v).value_or(0.0) : 0.0;
}

/// A whole number in [@p lo, @p hi], or 0 for anything else (absent,
/// fractional, out of range, not a number): the file is untrusted.
int attr_index(std::string_view tag, std::string_view name, int lo, int hi) {
    const double v = attr_double(tag, name);
    if (!(v >= lo && v <= hi) || v != static_cast<double>(static_cast<int>(v))) {
        return 0;
    }
    return static_cast<int>(v);
}

/// Calls @p fn with the text of each `<element ...>` tag in @p xml.
template <class Fn> void for_each_tag(std::string_view xml, std::string_view element, Fn&& fn) {
    const std::string open = "<" + std::string(element);
    size_t at = 0;
    while ((at = xml.find(open, at)) != std::string_view::npos) {
        const size_t after = at + open.size();
        if (after < xml.size() && !is_space(xml[after]) && xml[after] != '/' && xml[after] != '>') {
            at = after; // a longer element name sharing the prefix
            continue;
        }
        const size_t close = xml.find('>', after);
        if (close == std::string_view::npos) {
            return;
        }
        fn(xml.substr(at, close + 1 - at));
        at = close + 1;
    }
}

/// @p text with XML's five predefined entities decoded. Any other '&' stays as written.
std::string decode_entities(std::string_view text) {
    static constexpr std::pair<std::string_view, char> kEntities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
    };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        bool decoded = false;
        if (text[i] == '&') {
            for (const auto& [entity, c] : kEntities) {
                if (text.substr(i, entity.size()) == entity) {
                    out += c;
                    i += entity.size();
                    decoded = true;
                    break;
                }
            }
        }
        if (!decoded) {
            out += text[i++];
        }
    }
    return out;
}

/// The entry a model_settings pointer names, or nullptr. Pointers are archive
/// paths, sometimes written from the archive root with a leading '/'.
const zip::ZipEntry* pointed_entry(const std::vector<zip::ZipEntry>& entries,
                                   std::string_view pointer) {
    if (!pointer.empty() && pointer.front() == '/') {
        pointer.remove_prefix(1);
    }
    return pointer.empty() ? nullptr : find_archive_entry(entries, pointer);
}

/// N from "Metadata/plate_<N><suffix>", or 0.
int plate_number(std::string_view name, std::string_view suffix) {
    if (name.size() <= kPlatePrefix.size() + suffix.size() ||
        name.substr(0, kPlatePrefix.size()) != kPlatePrefix ||
        name.substr(name.size() - suffix.size()) != suffix) {
        return 0;
    }
    const std::string_view digits =
        name.substr(kPlatePrefix.size(), name.size() - kPlatePrefix.size() - suffix.size());
    if (!std::all_of(digits.begin(), digits.end(),
                     [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        return 0;
    }
    const int plate = text_io::parse_int<int>(digits).value_or(0);
    return plate <= kMaxPlates ? plate : 0;
}

} // namespace

int plate_of_gcode_entry(std::string_view name) {
    return plate_number(name, kGcodeSuffix);
}

const zip::ZipEntry* find_archive_entry(const std::vector<zip::ZipEntry>& entries,
                                        std::string_view name) {
    for (const auto& e : entries) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

std::vector<ThreeMfPlate> sliced_plates(const std::vector<zip::ZipEntry>& entries,
                                        const std::vector<PlateSettings>& settings) {
    std::vector<ThreeMfPlate> plates;
    auto plate_numbered = [&plates](int n) -> ThreeMfPlate* {
        for (auto& p : plates) {
            if (p.index == n) {
                return &p;
            }
        }
        return nullptr;
    };
    auto add_plate = [&plates, &plate_numbered](int n) -> ThreeMfPlate& {
        if (auto* p = plate_numbered(n)) {
            return *p;
        }
        ThreeMfPlate plate;
        plate.index = n;
        plates.push_back(plate);
        return plates.back();
    };

    std::vector<const zip::ZipEntry*> pointed;
    for (const auto& s : settings) {
        if (const auto* e = pointed_entry(entries, s.gcode_file)) {
            add_plate(s.plate).gcode = e;
            pointed.push_back(e);
        }
    }
    for (const auto& e : entries) {
        const int n = plate_of_gcode_entry(e.name);
        if (n <= 0 || std::find(pointed.begin(), pointed.end(), &e) != pointed.end()) {
            continue;
        }
        if (const auto* p = plate_numbered(n); p && p->gcode) {
            continue; // its own pointer was followed
        }
        add_plate(n).gcode = &e;
    }
    std::sort(plates.begin(), plates.end(),
              [](const ThreeMfPlate& a, const ThreeMfPlate& b) { return a.index < b.index; });

    for (const auto& e : entries) {
        int n = 0;
        if ((n = plate_number(e.name, "_small.png")) > 0) {
            if (auto* p = plate_numbered(n)) {
                p->thumb_small = &e;
            }
        } else if ((n = plate_number(e.name, ".png")) > 0) {
            if (auto* p = plate_numbered(n)) {
                p->thumb = &e;
            }
        }
    }
    for (auto& p : plates) {
        // The checksum sits beside whichever entry the G-code was taken from.
        p.md5 = find_archive_entry(entries, p.gcode->name + ".md5");
        for (const auto& s : settings) {
            if (s.plate != p.index) {
                continue;
            }
            p.name = s.name;
            if (const auto* thumb = pointed_entry(entries, s.thumbnail_file)) {
                p.thumb = thumb;
            }
        }
    }
    return plates;
}

std::vector<PlateSettings> parse_model_settings(std::string_view xml) {
    std::vector<PlateSettings> out;
    size_t at = 0;
    while ((at = xml.find("<plate>", at)) != std::string_view::npos) {
        const size_t end = xml.find("</plate>", at);
        const std::string_view block =
            xml.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at += 7;

        PlateSettings plate;
        for_each_tag(block, "metadata", [&plate](std::string_view tag) {
            const auto key = tag_attribute(tag, "key");
            if (!key) {
                return;
            }
            if (*key == "plater_id") {
                plate.plate = attr_index(tag, "value", 1, kMaxPlates);
            } else if (*key == "plater_name") {
                const std::string name =
                    decode_entities(text_io::trim(tag_attribute(tag, "value").value_or("")));
                plate.name = std::string(text_io::utf8_prefix(name, kMaxPlateNameBytes));
            } else if (*key == "gcode_file") {
                plate.gcode_file = decode_entities(tag_attribute(tag, "value").value_or(""));
            } else if (*key == "thumbnail_file") {
                plate.thumbnail_file = decode_entities(tag_attribute(tag, "value").value_or(""));
            }
        });
        if (plate.plate > 0) {
            out.push_back(std::move(plate));
        }
    }
    return out;
}

bool parse_slice_info(std::string_view xml, int plate, SliceInfo& out) {
    if (plate < 1) {
        return false; // a block whose index is unreadable reads as plate 0
    }
    size_t at = 0;
    while ((at = xml.find("<plate>", at)) != std::string_view::npos) {
        const size_t end = xml.find("</plate>", at);
        const std::string_view block =
            xml.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at += 7;

        SliceInfo info;
        for_each_tag(block, "metadata", [&info](std::string_view tag) {
            const auto key = tag_attribute(tag, "key");
            if (!key) {
                return;
            }
            if (*key == "index") {
                info.plate = attr_index(tag, "value", 1, kMaxPlates);
            } else if (*key == "prediction") {
                info.prediction_s = attr_double(tag, "value");
            } else if (*key == "weight") {
                info.weight_g = attr_double(tag, "value");
            }
        });
        if (info.plate != plate) {
            continue;
        }
        for_each_tag(block, "filament", [&info](std::string_view tag) {
            SliceFilament f;
            f.id = attr_index(tag, "id", 1, kMaxFilamentSlots);
            if (f.id == 0) {
                return; // not a slot a print can have
            }
            f.type = std::string(tag_attribute(tag, "type").value_or(""));
            f.color = std::string(tag_attribute(tag, "color").value_or(""));
            f.used_m = attr_double(tag, "used_m");
            f.used_g = attr_double(tag, "used_g");
            info.filaments.push_back(std::move(f));
        });
        out = std::move(info);
        return true;
    }
    return false;
}

std::string extracted_gcode_name(std::string_view three_mf_path, int plate, bool multi) {
    std::string name(three_mf_path);
    if (gcode::is_3mf(name)) {
        name.resize(name.size() - 4);
    }
    if (name.size() >= kGcodeSuffix.size() &&
        name.compare(name.size() - kGcodeSuffix.size(), kGcodeSuffix.size(), kGcodeSuffix) == 0) {
        name.resize(name.size() - kGcodeSuffix.size());
    }
    if (multi) {
        name += "_plate" + std::to_string(plate);
    }
    name += kGcodeSuffix;
    return name;
}

} // namespace helix::three_mf
