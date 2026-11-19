// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

/// A compiled regular expression: the ECMAScript subset this codebase uses,
/// with std::regex's match semantics and no std::locale, iostreams or
/// exceptions behind it. std::regex pulls libstdc++'s whole locale machinery
/// into every image that links it, which the ESP32 cannot afford.
///
/// Supported: literals and identity escapes, `.` (not \n or \r), classes with
/// ranges and negation, \d \D \s \S \w \W (ASCII), \b \B, ^ $ (whole-subject,
/// no multiline), capturing and (?:) groups, (?=) and (?!) lookahead,
/// alternation, * + ? {n} {n,} {n,m} greedy and lazy, \n \r \t \f \v \0 \xHH.
/// Not supported (a compile error): backreferences, lookbehind, named groups,
/// POSIX [[:class:]] names.
///
/// A pattern that fails to compile never matches; ok() and error() say why.
/// A search that exceeds the step limit (catastrophic backtracking) reports
/// no match.
class Regex {
  public:
    enum Flags : unsigned {
        None = 0,
        ICase = 1u << 0,
    };

    Regex() = default;
    explicit Regex(std::string_view pattern, unsigned flags = None);

    bool ok() const {
        return error_.empty() && !prog_.empty();
    }
    const std::string& error() const {
        return error_;
    }
    /// Number of capturing groups, like std::regex::mark_count().
    size_t mark_count() const {
        return groups_;
    }

    enum class Op : uint8_t {
        Char,
        Any,
        Class,
        Split,
        Jmp,
        Save,
        Bol,
        Eol,
        WordB,
        NotWordB,
        Look,
        LookEnd,
        Mark,
        Check,
        Match,
    };
    struct Inst {
        Op op;
        bool neg = false; ///< Look: negative lookahead
        char c = 0;       ///< Char
        int x = 0;        ///< Split/Jmp target, Class index, Save slot, Mark/Check slot, Look end
        int y = 0;        ///< Split second target, Check exit target
    };
    struct CharClass {
        bool negate = false;
        uint8_t kinds = 0; ///< bitmask of \d \D \s \S \w \W inside the class
        std::vector<std::pair<unsigned char, unsigned char>> ranges;
    };

  private:
    friend class RegexMatcher;
    std::vector<Inst> prog_;
    std::vector<CharClass> classes_;
    std::string error_;
    size_t groups_ = 0;
    int marks_ = 0;
    bool icase_ = false;
    bool anchored_ = false;
};

/// Result of a search or match, like std::smatch. Positions are relative to
/// the subject that was searched, and the subject must outlive the match.
class RegexMatch {
  public:
    struct Sub {
        bool matched = false;
        std::string_view view;
        std::string str() const {
            return std::string(view);
        }
        size_t length() const {
            return view.size();
        }
        operator std::string() const {
            return str();
        }
    };

    /// Group count including group 0; 0 when nothing matched.
    size_t size() const {
        return subs_.size();
    }
    bool empty() const {
        return subs_.empty();
    }
    Sub operator[](size_t i) const {
        return i < subs_.size() ? subs_[i] : Sub{};
    }
    std::string str(size_t i = 0) const {
        return (*this)[i].str();
    }
    size_t position(size_t i = 0) const;
    size_t length(size_t i = 0) const {
        return (*this)[i].length();
    }
    /// Offset just past the whole match, for resuming a scan.
    size_t end() const {
        return position(0) + length(0);
    }

  private:
    friend class RegexMatcher;
    std::string_view subject_;
    std::vector<Sub> subs_;
};

/// First match anywhere in `subject`.
bool regex_search(std::string_view subject, RegexMatch& m, const Regex& re);
bool regex_search(std::string_view subject, const Regex& re);
/// Match of the whole of `subject`.
bool regex_match(std::string_view subject, RegexMatch& m, const Regex& re);
bool regex_match(std::string_view subject, const Regex& re);
/// Every non-overlapping match replaced by `fmt`, where $& is the match,
/// $1..$99 a group and $$ a dollar sign, as std::regex_replace does. A search
/// that gives up (the step limit) ends the scan as if nothing more matched;
/// @p aborted, when given, says whether that happened.
std::string regex_replace(std::string_view subject, const Regex& re, std::string_view fmt,
                          bool* aborted = nullptr);

/// Successive matches over one subject, like std::sregex_iterator. A
/// default-constructed iterator is the end.
class RegexIterator {
  public:
    using iterator_category = std::input_iterator_tag;
    using value_type = RegexMatch;
    using difference_type = std::ptrdiff_t;
    using pointer = const RegexMatch*;
    using reference = const RegexMatch&;

    RegexIterator() = default;
    RegexIterator(std::string_view subject, const Regex& re);

    reference operator*() const {
        return match_;
    }
    pointer operator->() const {
        return &match_;
    }
    RegexIterator& operator++();
    RegexIterator operator++(int) {
        RegexIterator tmp = *this;
        ++*this;
        return tmp;
    }
    bool operator==(const RegexIterator& o) const {
        return re_ == nullptr && o.re_ == nullptr;
    }
    bool operator!=(const RegexIterator& o) const {
        return !(*this == o);
    }

    /// The scan ended because a search gave up, not because nothing matched.
    bool aborted() const {
        return aborted_;
    }

  private:
    std::string_view subject_;
    const Regex* re_ = nullptr;
    RegexMatch match_;
    bool aborted_ = false;
};

} // namespace helix
