// SPDX-License-Identifier: GPL-3.0-or-later
#include "gcode_wait_line.h"

#include "core/text_io.h"

#include <cctype>
#include <vector>

namespace helix {

namespace {

bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/// The whitespace-separated words of a gcode line, a double-quoted run kept
/// inside its word (SENSOR="heater_generic chamber").
std::vector<std::string_view> split_words(std::string_view line) {
    std::vector<std::string_view> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && is_blank(line[i])) {
            ++i;
        }
        const size_t start = i;
        bool quoted = false;
        while (i < line.size() && (quoted || !is_blank(line[i]))) {
            if (line[i] == '"') {
                quoted = !quoted;
            }
            ++i;
        }
        if (i > start) {
            words.push_back(line.substr(start, i - start));
        }
    }
    return words;
}

std::string_view unquote(std::string_view value) {
    if (!value.empty() && value.front() == '"') {
        value.remove_prefix(1);
        if (const size_t end = value.find('"'); end != std::string_view::npos) {
            value = value.substr(0, end);
        }
    }
    return value;
}

/// The extruder index a heater name denotes: extruder = 0, extruderN = N.
std::optional<int> extruder_index(std::string_view sensor) {
    constexpr std::string_view kExtruder = "extruder";
    if (sensor.substr(0, kExtruder.size()) != kExtruder) {
        return std::nullopt;
    }
    const std::string_view digits = sensor.substr(kExtruder.size());
    if (digits.empty()) {
        return 0;
    }
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }
    return text_io::parse_leading<int>(digits);
}

/// M109/M190/M191: letter-prefixed parameters (T1, S250).
void read_heater_params(const std::vector<std::string_view>& words, WaitLine& out) {
    for (size_t i = 1; i < words.size(); ++i) {
        const std::string_view word = words[i];
        const std::string_view value = word.substr(1);
        switch (std::toupper(static_cast<unsigned char>(word[0]))) {
        case 'S':
            out.target = text_io::parse_leading<double>(value);
            break;
        case 'T':
            out.tool = text_io::parse_leading<int>(value);
            break;
        default:
            break;
        }
    }
}

/// TEMPERATURE_WAIT: KEY=VALUE parameters.
void read_temperature_wait_params(const std::vector<std::string_view>& words, WaitLine& out) {
    for (size_t i = 1; i < words.size(); ++i) {
        const std::string_view word = words[i];
        const size_t eq = word.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string key = text_io::to_upper(word.substr(0, eq));
        const std::string_view value = unquote(word.substr(eq + 1));
        if (key == "SENSOR") {
            out.sensor = text_io::to_lower(value);
            out.tool = extruder_index(out.sensor);
        } else if (key == "MINIMUM") {
            out.minimum = text_io::parse_leading<double>(value);
        } else if (key == "MAXIMUM") {
            out.maximum = text_io::parse_leading<double>(value);
        }
    }
}

} // namespace

WaitLine classify_wait_line(std::string_view line) {
    if (const size_t comment = line.find(';'); comment != std::string_view::npos) {
        line = line.substr(0, comment);
    }
    const std::vector<std::string_view> words = split_words(line);
    WaitLine out;
    if (words.empty()) {
        return out;
    }
    const std::string command = text_io::to_upper(words[0]);
    if (command == "M109") {
        out.kind = WaitKind::M109;
    } else if (command == "M190") {
        out.kind = WaitKind::M190;
    } else if (command == "M191") {
        out.kind = WaitKind::M191;
    } else if (command == "TEMPERATURE_WAIT") {
        out.kind = WaitKind::TemperatureWait;
        read_temperature_wait_params(words, out);
        return out;
    } else {
        return out;
    }
    read_heater_params(words, out);
    return out;
}

HeaterTargetLine heater_target_line(std::string_view line) {
    if (const size_t comment = line.find(';'); comment != std::string_view::npos) {
        line = line.substr(0, comment);
    }
    const std::vector<std::string_view> words = split_words(line);
    HeaterTargetLine out;
    if (words.empty()) {
        return out;
    }
    const std::string command = text_io::to_upper(words[0]);
    if (command == "M104" || command == "M109" || command == "M140" || command == "M190") {
        WaitLine params;
        read_heater_params(words, params);
        out.target = params.target;
        if (command == "M140" || command == "M190") {
            out.heater = "heater_bed";
        } else {
            out.heater = params.tool && *params.tool > 0 ? "extruder" + std::to_string(*params.tool)
                                                         : std::string("extruder");
        }
        return out;
    }
    if (command != "SET_HEATER_TEMPERATURE") {
        return out;
    }
    for (size_t i = 1; i < words.size(); ++i) {
        const size_t eq = words[i].find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string key = text_io::to_upper(words[i].substr(0, eq));
        const std::string_view value = unquote(words[i].substr(eq + 1));
        if (key == "HEATER") {
            out.heater = text_io::to_lower(value);
        } else if (key == "TARGET") {
            out.target = text_io::parse_leading<double>(value);
        }
    }
    return out;
}

std::optional<std::string_view> first_line_at_position(std::string_view bytes, size_t requested) {
    if (bytes.empty()) {
        return std::nullopt;
    }
    if (const size_t newline = bytes.find('\n'); newline != std::string_view::npos) {
        return bytes.substr(0, newline);
    }
    // The file ended inside the window, or a comment began in it: either way
    // every byte of the command is here.
    if (bytes.size() < requested || bytes.find(';') != std::string_view::npos) {
        return bytes;
    }
    return std::nullopt;
}

} // namespace helix
