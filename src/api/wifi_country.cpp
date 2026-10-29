// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_country.h"

#include "text_io.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace helix::wifi::country {

namespace {

// ISO 3166-1 alpha-2, sorted by English short name so the dropdown reads in
// alphabetical order. A const table of pointers to literals lives in flash.
constexpr Entry kCountries[] = {
    {"AF", "Afghanistan"},
    {"AX", "Åland Islands"},
    {"AL", "Albania"},
    {"DZ", "Algeria"},
    {"AS", "American Samoa"},
    {"AD", "Andorra"},
    {"AO", "Angola"},
    {"AI", "Anguilla"},
    {"AQ", "Antarctica"},
    {"AG", "Antigua and Barbuda"},
    {"AR", "Argentina"},
    {"AM", "Armenia"},
    {"AW", "Aruba"},
    {"AU", "Australia"},
    {"AT", "Austria"},
    {"AZ", "Azerbaijan"},
    {"BS", "Bahamas"},
    {"BH", "Bahrain"},
    {"BD", "Bangladesh"},
    {"BB", "Barbados"},
    {"BY", "Belarus"},
    {"BE", "Belgium"},
    {"BZ", "Belize"},
    {"BJ", "Benin"},
    {"BM", "Bermuda"},
    {"BT", "Bhutan"},
    {"BO", "Bolivia"},
    {"BQ", "Bonaire, Sint Eustatius and Saba"},
    {"BA", "Bosnia and Herzegovina"},
    {"BW", "Botswana"},
    {"BV", "Bouvet Island"},
    {"BR", "Brazil"},
    {"IO", "British Indian Ocean Territory"},
    {"BN", "Brunei Darussalam"},
    {"BG", "Bulgaria"},
    {"BF", "Burkina Faso"},
    {"BI", "Burundi"},
    {"CV", "Cabo Verde"},
    {"KH", "Cambodia"},
    {"CM", "Cameroon"},
    {"CA", "Canada"},
    {"KY", "Cayman Islands"},
    {"CF", "Central African Republic"},
    {"TD", "Chad"},
    {"CL", "Chile"},
    {"CN", "China"},
    {"CX", "Christmas Island"},
    {"CC", "Cocos (Keeling) Islands"},
    {"CO", "Colombia"},
    {"KM", "Comoros"},
    {"CG", "Congo"},
    {"CD", "Congo, Democratic Republic of the"},
    {"CK", "Cook Islands"},
    {"CR", "Costa Rica"},
    {"CI", "Côte d'Ivoire"},
    {"HR", "Croatia"},
    {"CU", "Cuba"},
    {"CW", "Curaçao"},
    {"CY", "Cyprus"},
    {"CZ", "Czechia"},
    {"DK", "Denmark"},
    {"DJ", "Djibouti"},
    {"DM", "Dominica"},
    {"DO", "Dominican Republic"},
    {"EC", "Ecuador"},
    {"EG", "Egypt"},
    {"SV", "El Salvador"},
    {"GQ", "Equatorial Guinea"},
    {"ER", "Eritrea"},
    {"EE", "Estonia"},
    {"SZ", "Eswatini"},
    {"ET", "Ethiopia"},
    {"FK", "Falkland Islands"},
    {"FO", "Faroe Islands"},
    {"FJ", "Fiji"},
    {"FI", "Finland"},
    {"FR", "France"},
    {"GF", "French Guiana"},
    {"PF", "French Polynesia"},
    {"TF", "French Southern Territories"},
    {"GA", "Gabon"},
    {"GM", "Gambia"},
    {"GE", "Georgia"},
    {"DE", "Germany"},
    {"GH", "Ghana"},
    {"GI", "Gibraltar"},
    {"GR", "Greece"},
    {"GL", "Greenland"},
    {"GD", "Grenada"},
    {"GP", "Guadeloupe"},
    {"GU", "Guam"},
    {"GT", "Guatemala"},
    {"GG", "Guernsey"},
    {"GN", "Guinea"},
    {"GW", "Guinea-Bissau"},
    {"GY", "Guyana"},
    {"HT", "Haiti"},
    {"HM", "Heard Island and McDonald Islands"},
    {"VA", "Holy See"},
    {"HN", "Honduras"},
    {"HK", "Hong Kong"},
    {"HU", "Hungary"},
    {"IS", "Iceland"},
    {"IN", "India"},
    {"ID", "Indonesia"},
    {"IR", "Iran"},
    {"IQ", "Iraq"},
    {"IE", "Ireland"},
    {"IM", "Isle of Man"},
    {"IL", "Israel"},
    {"IT", "Italy"},
    {"JM", "Jamaica"},
    {"JP", "Japan"},
    {"JE", "Jersey"},
    {"JO", "Jordan"},
    {"KZ", "Kazakhstan"},
    {"KE", "Kenya"},
    {"KI", "Kiribati"},
    {"KP", "Korea, North"},
    {"KR", "Korea, South"},
    {"KW", "Kuwait"},
    {"KG", "Kyrgyzstan"},
    {"LA", "Laos"},
    {"LV", "Latvia"},
    {"LB", "Lebanon"},
    {"LS", "Lesotho"},
    {"LR", "Liberia"},
    {"LY", "Libya"},
    {"LI", "Liechtenstein"},
    {"LT", "Lithuania"},
    {"LU", "Luxembourg"},
    {"MO", "Macao"},
    {"MG", "Madagascar"},
    {"MW", "Malawi"},
    {"MY", "Malaysia"},
    {"MV", "Maldives"},
    {"ML", "Mali"},
    {"MT", "Malta"},
    {"MH", "Marshall Islands"},
    {"MQ", "Martinique"},
    {"MR", "Mauritania"},
    {"MU", "Mauritius"},
    {"YT", "Mayotte"},
    {"MX", "Mexico"},
    {"FM", "Micronesia"},
    {"MD", "Moldova"},
    {"MC", "Monaco"},
    {"MN", "Mongolia"},
    {"ME", "Montenegro"},
    {"MS", "Montserrat"},
    {"MA", "Morocco"},
    {"MZ", "Mozambique"},
    {"MM", "Myanmar"},
    {"NA", "Namibia"},
    {"NR", "Nauru"},
    {"NP", "Nepal"},
    {"NL", "Netherlands"},
    {"NC", "New Caledonia"},
    {"NZ", "New Zealand"},
    {"NI", "Nicaragua"},
    {"NE", "Niger"},
    {"NG", "Nigeria"},
    {"NU", "Niue"},
    {"NF", "Norfolk Island"},
    {"MK", "North Macedonia"},
    {"MP", "Northern Mariana Islands"},
    {"NO", "Norway"},
    {"OM", "Oman"},
    {"PK", "Pakistan"},
    {"PW", "Palau"},
    {"PS", "Palestine"},
    {"PA", "Panama"},
    {"PG", "Papua New Guinea"},
    {"PY", "Paraguay"},
    {"PE", "Peru"},
    {"PH", "Philippines"},
    {"PN", "Pitcairn"},
    {"PL", "Poland"},
    {"PT", "Portugal"},
    {"PR", "Puerto Rico"},
    {"QA", "Qatar"},
    {"RE", "Réunion"},
    {"RO", "Romania"},
    {"RU", "Russia"},
    {"RW", "Rwanda"},
    {"BL", "Saint Barthélemy"},
    {"SH", "Saint Helena, Ascension and Tristan da Cunha"},
    {"KN", "Saint Kitts and Nevis"},
    {"LC", "Saint Lucia"},
    {"MF", "Saint Martin (French part)"},
    {"PM", "Saint Pierre and Miquelon"},
    {"VC", "Saint Vincent and the Grenadines"},
    {"WS", "Samoa"},
    {"SM", "San Marino"},
    {"ST", "Sao Tome and Principe"},
    {"SA", "Saudi Arabia"},
    {"SN", "Senegal"},
    {"RS", "Serbia"},
    {"SC", "Seychelles"},
    {"SL", "Sierra Leone"},
    {"SG", "Singapore"},
    {"SX", "Sint Maarten (Dutch part)"},
    {"SK", "Slovakia"},
    {"SI", "Slovenia"},
    {"SB", "Solomon Islands"},
    {"SO", "Somalia"},
    {"ZA", "South Africa"},
    {"GS", "South Georgia and the South Sandwich Islands"},
    {"SS", "South Sudan"},
    {"ES", "Spain"},
    {"LK", "Sri Lanka"},
    {"SD", "Sudan"},
    {"SR", "Suriname"},
    {"SJ", "Svalbard and Jan Mayen"},
    {"SE", "Sweden"},
    {"CH", "Switzerland"},
    {"SY", "Syria"},
    {"TW", "Taiwan"},
    {"TJ", "Tajikistan"},
    {"TZ", "Tanzania"},
    {"TH", "Thailand"},
    {"TL", "Timor-Leste"},
    {"TG", "Togo"},
    {"TK", "Tokelau"},
    {"TO", "Tonga"},
    {"TT", "Trinidad and Tobago"},
    {"TN", "Tunisia"},
    {"TR", "Türkiye"},
    {"TM", "Turkmenistan"},
    {"TC", "Turks and Caicos Islands"},
    {"TV", "Tuvalu"},
    {"UG", "Uganda"},
    {"UA", "Ukraine"},
    {"AE", "United Arab Emirates"},
    {"GB", "United Kingdom"},
    {"US", "United States"},
    {"UM", "United States Minor Outlying Islands"},
    {"UY", "Uruguay"},
    {"UZ", "Uzbekistan"},
    {"VU", "Vanuatu"},
    {"VE", "Venezuela"},
    {"VN", "Viet Nam"},
    {"VG", "Virgin Islands (British)"},
    {"VI", "Virgin Islands (U.S.)"},
    {"WF", "Wallis and Futuna"},
    {"EH", "Western Sahara"},
    {"YE", "Yemen"},
    {"ZM", "Zambia"},
    {"ZW", "Zimbabwe"},
};

constexpr size_t kCountryCount = std::size(kCountries);

// Listed first in the dropdown, in this order, so the most common choices
// need no scrolling. Each must be in kCountries.
constexpr const char* kPinned[] = {"US", "GB", "DE", "CA", "AU", "FR"};

constexpr size_t kPinnedCount = std::size(kPinned);

std::string upper2(const std::string& code) {
    if (code.size() != 2) {
        return {};
    }
    std::string out;
    for (const char c : code) {
        if (!std::isalpha(static_cast<unsigned char>(c))) {
            return {};
        }
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

int table_index(const std::string& upper) {
    for (size_t i = 0; i < kCountryCount; ++i) {
        if (upper == kCountries[i].code) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int pinned_index(const char* code) {
    for (size_t i = 0; i < kPinnedCount; ++i) {
        if (std::strcmp(code, kPinned[i]) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// Calls @p visit(row, entry) for every country row of the dropdown in order
/// (row 1 is the first after the not-set row) until it returns true.
template <typename Visit> void for_each_row(Visit visit) {
    int row = 1;
    for (const char* code : kPinned) {
        if (visit(row++, kCountries[table_index(code)])) {
            return;
        }
    }
    for (const auto& entry : kCountries) {
        if (pinned_index(entry.code) < 0 && visit(row++, entry)) {
            return;
        }
    }
}

} // namespace

size_t table_size() {
    return kCountryCount;
}

const Entry& table_entry(size_t index) {
    return kCountries[index];
}

std::string normalize(const std::string& code) {
    const std::string upper = upper2(code);
    return table_index(upper) >= 0 ? upper : std::string();
}

bool is_world(const std::string& code) {
    return code.empty() || code == "00";
}

size_t pinned_count() {
    return kPinnedCount;
}

std::string dropdown_options(const std::string& not_set_label) {
    std::string out = not_set_label;
    for_each_row([&out](int, const Entry& entry) {
        out += '\n';
        out += entry.name;
        out += " (";
        out += entry.code;
        out += ')';
        return false;
    });
    return out;
}

int dropdown_index(const std::string& code) {
    const std::string upper = upper2(code);
    int found = 0;
    for_each_row([&](int row, const Entry& entry) {
        if (upper == entry.code) {
            found = row;
            return true;
        }
        return false;
    });
    return found;
}

std::string code_at_dropdown_index(int index) {
    std::string found;
    for_each_row([&](int row, const Entry& entry) {
        if (row == index) {
            found = entry.code;
            return true;
        }
        return false;
    });
    return found;
}

std::string parse_wpa_get_country(const std::string& reply) {
    return normalize(std::string(helix::text_io::trim(reply)));
}

std::string parse_iw_reg_get(const std::string& output) {
    const std::string key = "country ";
    size_t pos = 0;
    while ((pos = output.find(key, pos)) != std::string::npos) {
        const bool line_start = pos == 0 || output[pos - 1] == '\n';
        if (line_start && pos + key.size() + 3 <= output.size() &&
            output[pos + key.size() + 2] == ':') {
            return normalize(output.substr(pos + key.size(), 2));
        }
        pos += key.size();
    }
    return {};
}

std::string parse_regdom_change_alpha2(const std::string& event) {
    const std::string key = "alpha2=";
    const size_t pos = event.find(key);
    if (pos == std::string::npos) {
        return {};
    }
    size_t end = pos + key.size();
    while (end < event.size() && !std::isspace(static_cast<unsigned char>(event[end]))) {
        ++end;
    }
    return event.substr(pos + key.size(), end - pos - key.size());
}

Plan plan(const std::string& stored, const std::string& system, const std::string& last_seen) {
    const std::string want = normalize(stored);
    const std::string have = normalize(system);
    if (!want.empty()) {
        return {want == have ? std::string() : want, want};
    }
    if (have.empty()) {
        return {normalize(last_seen), have};
    }
    return {std::string(), have};
}

bool proc_status_has_capability(const std::string& proc_status, int bit) {
    if (bit < 0 || bit > 63) {
        return false;
    }
    const std::string key = "CapEff:";
    size_t pos = 0;
    while ((pos = proc_status.find(key, pos)) != std::string::npos) {
        if (pos == 0 || proc_status[pos - 1] == '\n') {
            break;
        }
        pos += key.size();
    }
    if (pos == std::string::npos) {
        return false;
    }
    const size_t eol = proc_status.find('\n', pos);
    const std::string_view value = helix::text_io::trim(
        std::string_view(proc_status)
            .substr(pos + key.size(),
                    eol == std::string::npos ? std::string::npos : eol - pos - key.size()));
    uint64_t mask = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), mask, 16);
    if (ec != std::errc() || end != value.data() + value.size() || value.empty()) {
        return false;
    }
    return ((mask >> bit) & 1U) != 0;
}

bool should_hint_5ghz(bool supports_5ghz, const std::string& effective_country) {
    return supports_5ghz && is_world(normalize(effective_country));
}

} // namespace helix::wifi::country
