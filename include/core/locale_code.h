// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace helix {

/// The language a locale code names, without its region: "en" for "en-GB", "pt" for
/// "pt-BR", and a plain code ("de") unchanged. Settings write the region after a hyphen;
/// an underscore (POSIX "pt_BR") is read the same way. A regional variant inherits
/// everything it does not define for itself from this language. The Python twin is
/// base_locale() in scripts/translations/locale_variants.py.
inline std::string_view locale_base_language(std::string_view code) {
    const auto cut = code.find_first_of("-_");
    return cut == std::string_view::npos ? code : code.substr(0, cut);
}

} // namespace helix
