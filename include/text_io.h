// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// File and text helpers with no iostreams and no std::locale. On ESP32 a single
// std::locale reference links every libstdc++ facet (~184K), so code in the
// ESP32 cut reads, writes, splits and parses through these instead.

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace helix::text_io {

// ---------------------------------------------------------------------------
// Whole files
// ---------------------------------------------------------------------------

/// The whole file, byte for byte (embedded NULs and CRLF kept), or its first
/// @p max_bytes when it is longer. nullopt when the file cannot be opened or a
/// read fails; an empty file is "".
std::optional<std::string>
read_file(const std::string& path, std::size_t max_bytes = std::numeric_limits<std::size_t>::max());

/// The first line, as one std::getline would read it: the '\n' is removed, a
/// '\r' before it is kept. "" for an empty file; nullopt only when the file
/// cannot be opened.
std::optional<std::string> read_first_line(const std::string& path);

/// Size in bytes from stat(). nullopt when stat fails.
std::optional<std::uint64_t> file_size(const std::string& path);

/// Create or truncate `path` and write `data`. false on any open, write or
/// close failure, with errno from the failing call.
bool write_file(const std::string& path, std::string_view data);

enum class Durability {
    None,  ///< tmp + rename: readers see the old file or the new one, never a torn one
    Fsync, ///< additionally fsync the tmp before rename and the directory after it
};

/// Replace `path` atomically: write `path + ".tmp"`, close, rename over `path`.
/// On failure the tmp is removed, `path` is untouched, and errno holds the
/// failing call's error. `path` is used as given; resolve symlinks
/// (helix::paths::write_target) before calling if the caller needs that.
bool write_file_atomic(const std::string& path, std::string_view data,
                       Durability durability = Durability::None);

// ---------------------------------------------------------------------------
// Streaming files, for inputs too big to hold (G-code) and incremental writes
// ---------------------------------------------------------------------------

struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f) {
            std::fclose(f);
        }
    }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

/// fopen with ownership. `mode` is fopen's ("rb", "wb", "ab"). Empty on failure,
/// errno set. Opens files past 2GB on 32-bit targets too.
File open_file(const std::string& path, const char* mode);

/// fseek with a 64-bit offset on every target. `whence` is SEEK_SET/CUR/END.
bool seek(std::FILE* f, std::int64_t offset, int whence);

/// The current position as a 64-bit offset. nullopt on failure.
std::optional<std::int64_t> tell(std::FILE* f);

/// fwrite all of `data`. false on a short write.
bool write_all(std::FILE* f, std::string_view data);

/// Flush and close, reporting what ofstream's good() would have: false if any
/// buffered write or the close itself failed. `f` is empty afterwards.
bool close(File& f);

/// Reads a file one delimited record at a time with std::getline's contract:
/// the delimiter is removed, a final record with no delimiter is still
/// returned, and nothing is returned for an empty file.
class LineReader {
  public:
    explicit LineReader(const std::string& path, char delim = '\n');

    /// False when the file could not be opened.
    explicit operator bool() const {
        return file_ != nullptr;
    }

    /// Next record into `line`. False at end of file or on a read error.
    bool next(std::string& line);

    /// Whether the record `next()` last returned ended with the delimiter,
    /// i.e. false only for a final record the file did not terminate.
    bool last_had_delimiter() const {
        return last_had_delimiter_;
    }

  private:
    File file_;
    char delim_;
    bool last_had_delimiter_ = false;
};

// ---------------------------------------------------------------------------
// Splitting text already in memory
// ---------------------------------------------------------------------------

/// Records of `text` split on `delim`, with the same results a
/// `while (std::getline(stream, line, delim))` loop produces:
/// "" -> none, "a" -> {a}, "a\n" -> {a}, "a\n\nb" -> {a, "", b}.
/// Views point into `text`, which must outlive the loop.
class Lines {
  public:
    class iterator {
      public:
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;
        using pointer = const std::string_view*;
        using reference = const std::string_view&;

        iterator() = default;
        iterator(std::string_view text, char delim)
            : text_(text), delim_(delim), pos_(0), done_(false) {
            advance();
        }
        reference operator*() const {
            return cur_;
        }
        pointer operator->() const {
            return &cur_;
        }
        iterator& operator++() {
            advance();
            return *this;
        }
        iterator operator++(int) {
            iterator old = *this;
            advance();
            return old;
        }
        friend bool operator==(const iterator& a, const iterator& b) {
            return a.done_ == b.done_ && (a.done_ || a.pos_ == b.pos_);
        }
        friend bool operator!=(const iterator& a, const iterator& b) {
            return !(a == b);
        }

      private:
        void advance() {
            if (pos_ >= text_.size()) {
                done_ = true;
                return;
            }
            size_t end = text_.find(delim_, pos_);
            if (end == std::string_view::npos) {
                cur_ = text_.substr(pos_);
                pos_ = text_.size();
            } else {
                cur_ = text_.substr(pos_, end - pos_);
                pos_ = end + 1;
            }
        }

        std::string_view text_;
        char delim_ = '\n';
        size_t pos_ = 0;
        std::string_view cur_;
        bool done_ = true;
    };

    Lines(std::string_view text, char delim) : text_(text), delim_(delim) {}
    iterator begin() const {
        return iterator(text_, delim_);
    }
    iterator end() const {
        return iterator();
    }

  private:
    std::string_view text_;
    char delim_;
};

inline Lines lines(std::string_view text, char delim = '\n') {
    return Lines(text, delim);
}

/// Whitespace-separated tokens, as successive `stream >> token` reads give
/// them (C-locale whitespace: space \t \n \v \f \r). Views point into `s`.
inline std::vector<std::string_view> split_ws(std::string_view s) {
    constexpr std::string_view ws = " \t\n\v\f\r";
    std::vector<std::string_view> out;
    size_t pos = s.find_first_not_of(ws);
    while (pos != std::string_view::npos) {
        size_t end = s.find_first_of(ws, pos);
        out.push_back(s.substr(pos, end == std::string_view::npos ? end : end - pos));
        pos = s.find_first_not_of(ws, end == std::string_view::npos ? s.size() : end);
    }
    return out;
}

/// `s` without leading and trailing C-locale whitespace (space \t \n \v \f \r).
/// The view points into `s`; "" when `s` is all whitespace.
inline std::string_view trim(std::string_view s) {
    constexpr std::string_view ws = " \t\n\v\f\r";
    const size_t first = s.find_first_not_of(ws);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

/// ASCII-only case mapping: bytes outside A-Z / a-z pass through untouched,
/// whatever the process locale.
inline std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

inline std::string to_upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Numbers. Strict: the whole view must be the number, no surrounding
// whitespace. One leading '+' is accepted, as std::stoi/stod and >> accept it.
// Always '.' as the decimal point, whatever the process locale.
// ---------------------------------------------------------------------------

/// An integer of type T in `base` (2-36, no "0x" prefix). nullopt on empty
/// input, trailing characters, a sign T cannot hold, or overflow of T.
template <typename T = long long> std::optional<T> parse_int(std::string_view s, int base = 10) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
    if (s.size() > 1 && s[0] == '+' && s[1] != '-' && s[1] != '+') {
        s.remove_prefix(1);
    }
    if (s.empty()) {
        return std::nullopt;
    }
    T value{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return value;
}

/// A decimal floating-point number ("1.5", "-2e3", "inf", "nan"). nullopt on
/// empty input, trailing characters or out-of-range magnitude.
std::optional<double> parse_double(std::string_view s);

// ---------------------------------------------------------------------------
// Numbers by std::sto* rules: leading whitespace skipped, the number read up to
// the first character that cannot continue it, the rest ignored ("12abc" is 12,
// "3.9" is 3 as an int). nullopt exactly where the std:: form throws: no number
// at all, or a value outside T. The non-throwing drop-in for std::stoi, stol,
// stoll, stoul, stoull, stof and stod; prefer parse_int/parse_double where a
// caller has no reason to accept trailing text.
// ---------------------------------------------------------------------------

template <typename T> std::optional<T> parse_leading(std::string_view s, int base = 10) {
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>);
    const std::string buf(s); // strto* reads up to a terminator
    const char* begin = buf.c_str();
    char* end = nullptr;
    errno = 0;
    if constexpr (std::is_floating_point_v<T>) {
        static_assert(!std::is_same_v<T, long double>);
        const T v = std::is_same_v<T, float> ? std::strtof(begin, &end) : std::strtod(begin, &end);
        if (end == begin || errno == ERANGE) {
            return std::nullopt;
        }
        return v;
    } else if constexpr (std::is_signed_v<T>) {
        const long long v = std::strtoll(begin, &end, base);
        if (end == begin || errno == ERANGE || v < std::numeric_limits<T>::min() ||
            v > std::numeric_limits<T>::max()) {
            return std::nullopt;
        }
        return static_cast<T>(v);
    } else {
        // strtoul/strtoull negate a leading '-' in the unsigned type, so "-1" is
        // the maximum rather than an error, as std::stoul and std::stoull return.
        using Wide = std::conditional_t<(sizeof(T) <= sizeof(unsigned long)), unsigned long,
                                        unsigned long long>;
        const Wide v = std::is_same_v<Wide, unsigned long> ? std::strtoul(begin, &end, base)
                                                           : std::strtoull(begin, &end, base);
        if (end == begin || errno == ERANGE || v > std::numeric_limits<T>::max()) {
            return std::nullopt;
        }
        return static_cast<T>(v);
    }
}

} // namespace helix::text_io
