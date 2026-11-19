// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_regex.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::Regex;
using helix::RegexIterator;
using helix::RegexMatch;

namespace {

struct Pattern {
    std::string text;
    bool icase;
};

// Every pattern literal the tree hands std::regex, with its icase flag.
const std::vector<Pattern> kSourcePatterns = {
    {R"re(Prob(?:ing (?:mesh )?point|e point) (\d+)[/\s]+(?:of\s+)?(\d+))re", false},
    {R"re(Adapted probe count:\s*\(?\s*(\d+)\s*,\s*(\d+))re", false},
    {R"re(probe at (?:x:\s*)?(-?\d+(?:\.\d+)?)[,\s]+(?:y:\s*)?(-?\d+(?:\.\d+)?)\s+is z=)re", false},
    {R"re("rotate"\s*:\s*(\d+))re", false},
    {R"re(sample:(\d+)\s+pwm:[\d.]+\s+asymmetry:[\d.]+\s+tolerance:(\S+))re", false},
    {R"re(pid_Kp=([\d.]+)\s+pid_Ki=([\d.]+)\s+pid_Kd=([\d.]+))re", false},
    {R"re(fan_ambient_transfer=([\d., ]+)\s*\[W/K\])re", false},
    {R"re(block_heat_capacity=([\d.]+))re", false},
    {R"re(sensor_responsiveness=([\d.]+))re", false},
    {R"re(ambient_transfer=([\d.]+)\s+\[W/K\])re", false},
    {R"re(Testing frequency ([\d.]+) Hz)re", false},
    {R"re(Fitted (?:shaper|smoother) '([\w]+)' frequency = ([\d.]+) Hz \(vibration score = ([\d.]+)%, smoothing ~= ([\d.]+))re",
     false},
    {R"re(Fitted shaper '(\w+)' frequency = ([\d.]+) Hz \(vibrations = ([\d.]+)%, smoothing ~= ([\d.]+)\))re",
     false},
    {R"re(suggested max_accel <= (\d+))re", false},
    {R"re(Recommended shaper_type_\w+ = (\w+), shaper_freq_\w+ = ([\d.]+) Hz)re", false},
    {R"re(Recommended smoother_type_\w+ = (\w+), smoother_freq_\w+ = ([\d.]+) Hz)re", false},
    {R"re(Recommended shaper is (\w+) @ ([\d.]+) Hz)re", false},
    {R"re(calibration data written to (\S+\.csv))re", false},
    {R"re(Axes noise.*:\s*([\d.]+)\s*\(x\),\s*([\d.]+)\s*\(y\),\s*([\d.]+)\s*\(z\))re", false},
    {R"re("brightness"\s*:\s*(\d+))re", false},
    {R"re("dark_mode"\s*:\s*(true|false))re", false},
    {R"re("auto_restart_sec"\s*:\s*(\d+))re", false},
    {R"re(\{%\s*if\s+.*)re", true},
    {R"re(\{%\s*set\s+\w+\s*=\s*params\.)re", true},
    {R"re(params\.([A-Z_][A-Z0-9_]*))re", true},
    {R"re(PRINT_START|START_PRINT|_PRINT_START)re", true},
    {R"re(\bprint\b\W+\b(start|started|starting)\b|\b(start|started|starting)\b\W+\bprint\b)re",
     true},
    {R"re(\s*\w+:-?\d+(\.\d+)? /-?\d+(\.\d+)?(\s+\w+:-?\d+(\.\d+)? /-?\d+(\.\d+)?)*\s*)re", false},
    {R"re(\{%\s*if\s)re", true},
    {R"re(\{%\s*endif\s*%\})re", true},
    {R"re(\{%\s*for\s)re", true},
    {R"re(\{%\s*endfor\s*%\})re", true},
    {R"re(^(.+)\.backup\.\d{8}_\d{6}$)re", false},
    {R"re(^\s*\[\s*([^\]\s]+)\s*\]\s*$)re", false},
    {R"re(^\s*filament_([A-Za-z0-9_+\-]+)\s*[:=].*$)re", false},
    {R"re(Heating the nozzle to\s+(\d+))re", false},
    {R"re(^\s*(?://\s*)?Extruder:\s*\d+\s*$)re", false},
    {R"re(SLOT=(\d+))re", false},
    {R"re(TYPE=([^\s|]+))re", false},
    {R"re(HEX=([0-9A-Fa-f]{6}))re", false},
    {R"re(action:prompt_button\s+(\d+)\s*:[^|]*\|\s*RUN_ZCOLOR\b)re", false},
    {R"re(^//\s*Extruder:\s*(.+?)\s*\|\s*IFS:\s*(True|False)\s*$)re", false},
    {R"re(^//\s*([1-9])\s*:\s*(.+?)\s*$)re", false},
    {R"re(^([1-9])\s*:)re", false},
    {R"re(\((\d+)\))re", false},
    {R"re(#\s*helix_macros\s+v(\d+\.\d+\.\d+))re", false},
    {R"re(^\[gcode_macro\s+(\w+)\])re", false},
    {R"re(td1_lane(\d+))re", false},
    {R"re(^(?:ghp_|gho_|glpat-|xoxb-|xoxp-)?[A-Za-z0-9+/=_-]{36,}$)re", false},
    {R"re(://[^@/\s]+:[^@/\s]+@)re", false},
    {R"re(\b[\w.+-]+@[\w-]+(\.[\w-]+)*\.[A-Za-z]{2,}\b)re", false},
    {R"re(\b([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}\b)re", false},
    {R"re(:\s+(\d+)\s+kB)re", false},
    {R"re(processor\s*:\s*\d+)re", false},
    {R"re([Bb]ogo[Mm][Ii][Pp][Ss]\s*:\s*([0-9.]+))re", false},
    {R"re(cpu MHz\s*:\s*([0-9.]+))re", false},
    {R"re(\s*:\s*([^\n]+))re", false},
    {R"re(lv_streq\s*\(\s*"([^"]+)")re", false},
    {R"re(lv_xml_get_value_of\s*\([^,]+,\s*"([^"]+)")re", false},
    {R"re(SET_STYLE_IF\s*\(\s*(\w+)\s*,)re", false},
    {R"re(strcmp\s*\(\s*\w+(?:\[\w+\])?\s*,\s*"([^"]+)"\s*\)\s*==\s*0)re", false},
    {R"re(lv_xml_register_widget\s*\(\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*(\w+)\s*\))re", false},
    {R"re(<component\b)re", false},
    {R"re(<view\s+extends\s*=\s*"([^"]+)")re", false},
    {R"re(<view\b)re", false},
    {R"re(<prop\s+name\s*=\s*"([^"]+)")re", false},
    {R"re(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)re", false},
    {R"re(params\.([A-Za-z_][A-Za-z0-9_]*)|params\['([A-Za-z_][A-Za-z0-9_]*)'\]|params\["([A-Za-z_][A-Za-z0-9_]*)"\])re",
     false},
    {R"re((?:'([A-Za-z_][A-Za-z0-9_]*)'|"([A-Za-z_][A-Za-z0-9_]*)")\s+(?:not\s+)?in\s+params)re",
     false},
    // Patterns assembled at runtime, in the shape their builders produce.
    {R"re(BED_TEMP=\S+)re", true},
    {R"re(model name\s*:\s*([^\n]+))re", false},
    {R"re(^// times:[0-9])re", false},
};

// Real inputs for the patterns above: Klipper responses, calibration output,
// AD5X IFS files, macro templates, gcode headers, config files.
const std::vector<std::string> kCorpus = {
    "// Probing point 3/25",
    "Probe point 12 of 49",
    "Probing mesh point 7 / 16",
    "Adapted probe count: 5, 5",
    "// Adapted probe count: (7,7)",
    "probe at 120.000,130.500 is z=0.012500",
    "probe at x: -5.5, y: 10 is z=1.2",
    R"({"rotate": 180, "brightness": 55, "dark_mode": true, "auto_restart_sec": 30})",
    R"({"brightness":100,"dark_mode":false})",
    "sample:3 pwm:0.500 asymmetry:0.012 tolerance:OK",
    "PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178",
    "fan_ambient_transfer=0.123, 0.140, 0.155 [W/K]",
    "block_heat_capacity=18.3721 [J/K]",
    "sensor_responsiveness=0.0891 [K/s/K]",
    "ambient_transfer=0.1325 [W/K]",
    "Testing frequency 62.5 Hz",
    "Fitted shaper 'mzv' frequency = 45.2 Hz (vibrations = 2.1%, smoothing ~= 0.094)",
    "Fitted shaper 'ei' frequency = 53.8 Hz (vibration score = 1.5%, smoothing ~= 0.120",
    "Fitted smoother 'smooth_zv' frequency = 40.0 Hz (vibration score = 0.8%, smoothing ~= 0.2",
    "To avoid too much smoothing with 'mzv', suggested max_accel <= 6200 mm/sec^2",
    "Recommended shaper_type_x = mzv, shaper_freq_x = 45.2 Hz",
    "Recommended smoother_type_y = smooth_ei, smoother_freq_y = 38.1 Hz",
    "Recommended shaper is mzv @ 45.2 Hz",
    "calibration data written to /tmp/calibration_data_x_20260927_121314.csv",
    "Axes noise for xy-axis accelerometer: 12.345 (x), 11.2 (y), 30.01 (z)",
    "{% if params.BED_TEMP|default(60)|float > 0 %}",
    "{%if params.X %}",
    "{% set bed_temp = params.BED|default(60) %}",
    "{% set chamber = params.CHAMBER_TEMP|int %}",
    "M190 S{params.BED_TEMP} ; heat bed",
    "{% endif %}",
    "{%endif%}",
    "{% for i in range(3) %}",
    "{% endfor %}",
    "PRINT_START BED=60 EXTRUDER=210",
    "start_print",
    "_PRINT_START",
    "Print started",
    "starting print now",
    "print, start!",
    "reprint started",
    "B:60.0 /60.0 T0:210.1 /210.0",
    "T0:25.5 /0.0",
    " B:-1 /0 ",
    "ok T0:210 /210",
    "printer.cfg.backup.20260927_121314",
    "a.b.backup.2026092_1213",
    "[filament]",
    "  [ zmod_ifs ]  ",
    "filament_pla = 1",
    "  filament_PETG-CF : 2",
    "filament_TPU+95=3",
    "Heating the nozzle to 215",
    "; Extruder: 1",
    "// Extruder: 3",
    "Extruder: 2 ",
    "SLOT=2 TYPE=PLA HEX=FF8800",
    "SLOT=4 TYPE=PETG|x HEX=00ff7a",
    "HEX=12345G",
    "// action:prompt_button 2: Load | RUN_ZCOLOR SLOT=2",
    "action:prompt_button 3 :x|RUN_ZCOLORS",
    "//Extruder: 1 | IFS: True",
    "// Extruder: Red PLA  |  IFS: False  ",
    "// 1: PLA",
    "//9 :  Silk Gold  ",
    "1: PLA",
    "0: none",
    "Lane (12)",
    "# helix_macros v1.2.3",
    "#helix_macros   v10.0.22 extra",
    "[gcode_macro PRINT_START]",
    "[gcode_macro _HELIX_PARK extra]",
    "td1_lane3",
    "td1_laneX",
    "ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghij",
    "short_token_123",
    "abcdefghijABCDEFGHIJ0123456789+/=_-xyz",
    "http://user:secret@host:7125/path",
    "https://host/path",
    "contact me at first.last+tag@mail.example.co.uk today",
    "bad@address",
    "mac aa:bb:cc:dd:ee:ff and AA-BB-CC-DD-EE-FF here",
    "aa:bb:cc:dd:ee",
    "MemTotal:        1024000 kB",
    "processor\t: 0\nprocessor\t: 1\nBogoMIPS\t: 57.14\ncpu MHz\t\t: 1008.000",
    "bogomips : 1200.5",
    "model name\t: ARMv7 Processor rev 5 (v7l)",
    R"(if (lv_streq(name, "bg_color")) {)",
    R"(lv_xml_get_value_of(attrs, "width"))",
    "SET_STYLE_IF(radius, x)",
    R"(if (strcmp(attrs[i], "text") == 0))",
    R"(lv_xml_register_widget("ui_button", create_cb, apply_cb))",
    R"(<component><view extends="lv_obj" name="x">)",
    R"(<prop name="value" type="int"/>)",
    "<components>",
    "12.5",
    "-.5e3",
    "+7.",
    "1e",
    R"(SET_HEATER_TEMPERATURE TARGET={params.T|default(200)} {params['BED']} {params["X_1"]})",
    R"({% if 'BED' in params %} {% if "Z" not in params %})",
    "SET_PRINT_STATS_INFO CURRENT_LAYER=1",
    "BED_TEMP=65 bed_temp=abc",
    "// times:3",
    "",
    "\r\n",
};

void require_same(const Pattern& p, const std::string& input) {
    std::regex sr(p.text, p.icase ? std::regex::icase : std::regex::ECMAScript);
    Regex hr(p.text, p.icase ? Regex::ICase : Regex::None);
    INFO("pattern: " << p.text << (p.icase ? " (icase)" : "") << "\ninput: " << input);
    REQUIRE(hr.ok());
    REQUIRE(hr.mark_count() == sr.mark_count());

    std::smatch sm;
    RegexMatch hm;
    const bool s_found = std::regex_search(input, sm, sr);
    REQUIRE(helix::regex_search(input, hm, hr) == s_found);
    if (s_found) {
        REQUIRE(hm.size() == sm.size());
        for (size_t g = 0; g < sm.size(); ++g) {
            INFO("group " << g);
            REQUIRE(hm[g].matched == sm[g].matched);
            if (sm[g].matched) {
                REQUIRE(hm.position(g) == static_cast<size_t>(sm.position(g)));
                REQUIRE(hm.str(g) == sm.str(g));
            }
        }
    }
    REQUIRE(helix::regex_match(input, hr) == std::regex_match(input, sr));

    std::vector<std::pair<size_t, std::string>> s_all, h_all;
    for (auto it = std::sregex_iterator(input.begin(), input.end(), sr);
         it != std::sregex_iterator(); ++it)
        s_all.emplace_back(static_cast<size_t>(it->position()), it->str());
    for (RegexIterator it(input, hr), end; it != end; ++it)
        h_all.emplace_back(it->position(), it->str());
    REQUIRE(h_all == s_all);
}

std::vector<Pattern> profile_patterns() {
    std::vector<Pattern> out;
    for (const auto& entry :
         std::filesystem::directory_iterator("assets/config/print_start_profiles")) {
        if (entry.path().extension() != ".json")
            continue;
        std::ifstream f(entry.path());
        auto j = nlohmann::json::parse(f, nullptr, false);
        for (const char* key : {"response_patterns", "state_patterns"}) {
            if (!j.contains(key))
                continue;
            for (const auto& rp : j[key])
                if (rp.contains("pattern") && rp["pattern"].is_string())
                    out.push_back({rp["pattern"].get<std::string>(), true});
        }
    }
    return out;
}

bool finds(const char* pattern, const std::string& s, unsigned flags = Regex::None) {
    return helix::regex_search(s, Regex(pattern, flags));
}

} // namespace

TEST_CASE("helix::Regex constructs", "[regex]") {
    SECTION("literals, dot and anchors") {
        CHECK(finds("abc", "xxabcxx"));
        CHECK_FALSE(finds("a.c", "a\nc"));
        CHECK_FALSE(finds("a.c", "a\rc"));
        CHECK(finds("^ab", "abc"));
        CHECK_FALSE(finds("^b", "ab"));
        CHECK_FALSE(finds("a$", "a\n"));
    }
    SECTION("classes and escapes") {
        CHECK(finds("[a-c]+", "zzb"));
        CHECK_FALSE(finds("[^a-z]", "abc"));
        CHECK(finds("[\\d.]+", "v1.2"));
        CHECK(finds("[^\\]\\s]+", "["));
        CHECK(finds("\\x41", "A"));
        CHECK(finds("\\t", "a\tb"));
        CHECK(finds("\\/\\-", "/-"));
    }
    SECTION("word boundaries") {
        CHECK(finds("\\bfoo\\b", "a foo."));
        CHECK_FALSE(finds("\\bfoo\\b", "afoo"));
        CHECK(finds("\\Boo", "foo"));
    }
    SECTION("quantifiers") {
        RegexMatch m;
        REQUIRE(helix::regex_search("aaaa", m, Regex("a{2,3}")));
        CHECK(m.str() == "aaa");
        REQUIRE(helix::regex_search("abcbc", m, Regex(".+?c")));
        CHECK(m.str() == "abc");
        REQUIRE(helix::regex_search("aaa", m, Regex("a{2}")));
        CHECK(m.str() == "aa");
        CHECK(helix::regex_match("aaaaa", Regex("a{5,}")));
        CHECK_FALSE(helix::regex_match("aaaa", Regex("a{5,}")));
    }
    SECTION("groups, alternation and lookahead") {
        RegexMatch m;
        REQUIRE(helix::regex_search("ab", m, Regex("a|ab")));
        CHECK(m.str() == "a");
        REQUIRE(helix::regex_search("x=12", m, Regex("(?:x)=(\\d+)")));
        CHECK(m.size() == 2);
        CHECK(m.str(1) == "12");
        CHECK(finds("BED_MESH_CALIBRATE(?!_START_PRINT)", "BED_MESH_CALIBRATE ADAPTIVE=1"));
        CHECK_FALSE(finds("BED_MESH_CALIBRATE(?!_START_PRINT)", "BED_MESH_CALIBRATE_START_PRINT"));
        CHECK(finds("(?=ab)a", "ab"));
    }
    SECTION("empty loop bodies terminate") {
        RegexMatch m;
        REQUIRE(helix::regex_search("aab", m, Regex("(a*)*b")));
        CHECK(m.str() == "aab");
    }
    SECTION("icase") {
        CHECK(finds("print_start", "PRINT_START", Regex::ICase));
        CHECK(finds("[a-z]+", "ABC", Regex::ICase));
        CHECK_FALSE(finds("[^a-z]", "ABC", Regex::ICase));
    }
    SECTION("unmatched groups report unmatched") {
        RegexMatch m;
        REQUIRE(helix::regex_search("z", m, Regex("(\\.\\d+)?z")));
        CHECK_FALSE(m[1].matched);
        CHECK(m.str(1).empty());
        CHECK(m[5].str().empty());
    }
    SECTION("replace") {
        CHECK(helix::regex_replace("a1b2", Regex("\\d"), "#") == "a#b#");
        CHECK(helix::regex_replace("ab", Regex("(a)"), "[$1$&$$]") == "[aa$]b");
        CHECK(helix::regex_replace("BED_TEMP=65 X", Regex("bed_temp=\\S+", Regex::ICase),
                                   "BED_TEMP=0") == "BED_TEMP=0 X");
    }
    SECTION("iterator counts empty matches like std::sregex_iterator") {
        Regex re("x*");
        CHECK(std::distance(RegexIterator("a1b2", re), RegexIterator()) == 5);
    }
}

TEST_CASE("helix::Regex rejects what it does not support", "[regex]") {
    for (const char* bad : {"a{", "a{,2}", "a{3,1}", "(ab", "ab)", "[abc", "*a", "a**b(?<x>y)",
                            "(a)\\1", "(?<=a)b", "[[:alpha:]]", "\\q", "a\\"}) {
        INFO(bad);
        Regex re(bad);
        CHECK_FALSE(re.ok());
        CHECK_FALSE(re.error().empty());
        CHECK_FALSE(helix::regex_search("anything a ab", re));
    }
    CHECK_FALSE(Regex().ok());
}

TEST_CASE("helix::Regex gives up on catastrophic backtracking", "[regex]") {
    Regex re("(a+)+b");
    REQUIRE(re.ok());
    CHECK_FALSE(helix::regex_search(std::string(40, 'a'), re));
    CHECK(helix::regex_search("aaab", re));
}

TEST_CASE("helix::regex_replace reports a search that gave up", "[regex]") {
    Regex re("(a+)+b");
    REQUIRE(re.ok());
    bool aborted = false;
    CHECK(helix::regex_replace("x aaab", re, "B", &aborted) == "x B");
    CHECK_FALSE(aborted);
    const std::string runaway = "aaab " + std::string(40, 'a');
    CHECK(helix::regex_replace(runaway, re, "B", &aborted) == "B " + std::string(40, 'a'));
    CHECK(aborted);
}

TEST_CASE("helix::Regex matches std::regex on every pattern in the tree", "[regex]") {
    for (const auto& p : kSourcePatterns)
        for (const auto& input : kCorpus)
            require_same(p, input);
}

TEST_CASE("helix::Regex matches std::regex on the print-start profile patterns", "[regex]") {
    auto patterns = profile_patterns();
    REQUIRE(patterns.size() > 50);
    std::vector<std::string> lines = kCorpus;
    for (const char* extra : {"BED_MESH_CALIBRATE_START_PRINT",
                              "BED_MESH_CALIBRATE ADAPTIVE=1",
                              "Heating bed",
                              "M104 S215",
                              "M104 S0",
                              "Soaking: 5 min",
                              "Heatsoak: 4.5m",
                              "Chamber: 45c",
                              "// Wait bed temperature to reach 60",
                              "M109 S210",
                              "can_break_flag = 3",
                              "can_break_flag is 3",
                              "Skew profile 'default' found",
                              "x_axes: xyz",
                              "flashforge_loadcell: Tare",
                              "Result is z=0.1",
                              "EXTRUDER_TEMP=210",
                              "PRINT_START BED_MESH_CALIBRATE_START_PRINT EXTRUDER_TEMP=210",
                              "Probing mesh",
                              "exist_points[3]",
                              "// trigger_mcu_pos: {x:1}",
                              "Chamber temperature 45C reached",
                              "HOMING",
                              "heat soak",
                              "bed-soak",
                              "Z_TILT_ADJUST",
                              "z tilt adjust",
                              "KAMP_ADAPTIVE_PURGE",
                              "prime nozzle",
                              "Rehoming Z",
                              "Smart Park location"})
        lines.emplace_back(extra);
    for (const auto& p : patterns)
        for (const auto& input : lines)
            require_same(p, input);
}
