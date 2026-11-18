// SPDX-License-Identifier: GPL-3.0-or-later

#include "helix_regex.h"

#include <cctype>
#include <memory>

namespace helix {

namespace {

// Backtracking steps one search may take before it gives up as no match. Real
// patterns here finish in a few thousand; (a+)+b over a long line does not.
constexpr size_t kStepLimit = 1'000'000;

enum Kind : uint8_t {
    KDigit = 1,
    KNotDigit = 2,
    KSpace = 4,
    KNotSpace = 8,
    KWord = 16,
    KNotWord = 32
};

bool is_word(unsigned char c) {
    return std::isalnum(c) || c == '_';
}
bool is_space(unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

bool kinds_match(uint8_t kinds, unsigned char c) {
    const bool d = std::isdigit(c) != 0;
    const bool s = is_space(c);
    const bool w = is_word(c);
    return ((kinds & KDigit) && d) || ((kinds & KNotDigit) && !d) || ((kinds & KSpace) && s) ||
           ((kinds & KNotSpace) && !s) || ((kinds & KWord) && w) || ((kinds & KNotWord) && !w);
}

struct Node {
    enum Type { Seq, Alt, Char, Any, Class, Group, Look, Bol, Eol, WordB, NotWordB, Repeat } type;
    char c = 0;
    int index = -1; ///< Group: capture index (-1 = non-capturing); Class: class index
    bool neg = false;
    bool greedy = true;
    int min = 0;
    int max = -1; ///< -1 = unbounded
    std::vector<std::unique_ptr<Node>> kids;
};
using NodePtr = std::unique_ptr<Node>;

NodePtr make(Node::Type t) {
    auto n = std::make_unique<Node>();
    n->type = t;
    return n;
}

class Parser {
  public:
    Parser(std::string_view p, std::vector<Regex::CharClass>& classes) : p_(p), classes_(classes) {}

    NodePtr parse() {
        NodePtr n = alternation();
        if (err_.empty() && i_ < p_.size())
            fail(p_[i_] == ')' ? "unmatched ')'" : "unexpected character");
        return n;
    }
    const std::string& error() const {
        return err_;
    }
    size_t groups() const {
        return groups_;
    }

  private:
    std::string_view p_;
    size_t i_ = 0;
    size_t groups_ = 0;
    std::string err_;
    std::vector<Regex::CharClass>& classes_;

    void fail(const char* msg) {
        if (err_.empty())
            err_ = std::string(msg) + " at offset " + std::to_string(i_);
    }
    bool more() const {
        return err_.empty() && i_ < p_.size();
    }

    NodePtr alternation() {
        auto alt = make(Node::Alt);
        alt->kids.push_back(sequence());
        while (more() && p_[i_] == '|') {
            ++i_;
            alt->kids.push_back(sequence());
        }
        if (alt->kids.size() == 1)
            return std::move(alt->kids[0]);
        return alt;
    }

    NodePtr sequence() {
        auto seq = make(Node::Seq);
        while (more() && p_[i_] != '|' && p_[i_] != ')') {
            NodePtr a = atom();
            if (!err_.empty())
                break;
            a = quantified(std::move(a));
            seq->kids.push_back(std::move(a));
        }
        return seq;
    }

    bool parse_int(int& out) {
        size_t start = i_;
        long v = 0;
        while (i_ < p_.size() && std::isdigit(static_cast<unsigned char>(p_[i_]))) {
            v = v * 10 + (p_[i_] - '0');
            if (v > 100000)
                return false;
            ++i_;
        }
        out = static_cast<int>(v);
        return i_ > start;
    }

    NodePtr quantified(NodePtr a) {
        while (more()) {
            int mn, mx;
            const char q = p_[i_];
            if (q == '*') {
                mn = 0, mx = -1, ++i_;
            } else if (q == '+') {
                mn = 1, mx = -1, ++i_;
            } else if (q == '?') {
                mn = 0, mx = 1, ++i_;
            } else if (q == '{') {
                ++i_;
                if (!parse_int(mn)) {
                    fail("invalid range in '{}'");
                    return a;
                }
                mx = mn;
                if (i_ < p_.size() && p_[i_] == ',') {
                    ++i_;
                    if (!parse_int(mx))
                        mx = -1;
                }
                if (i_ >= p_.size() || p_[i_] != '}' || (mx != -1 && mx < mn)) {
                    fail("invalid range in '{}'");
                    return a;
                }
                ++i_;
            } else {
                break;
            }
            if (a->type == Node::Bol || a->type == Node::Eol || a->type == Node::WordB ||
                a->type == Node::NotWordB || a->type == Node::Look) {
                fail("nothing to repeat");
                return a;
            }
            auto r = make(Node::Repeat);
            r->min = mn;
            r->max = mx;
            if (i_ < p_.size() && p_[i_] == '?') {
                r->greedy = false;
                ++i_;
            }
            r->kids.push_back(std::move(a));
            a = std::move(r);
        }
        return a;
    }

    static int hex(char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
    }

    // Shared by atoms and classes. Returns true with `out` set for a single
    // character; sets `kind` for \d-style escapes instead.
    bool escape(char& out, uint8_t& kind, bool in_class) {
        if (i_ >= p_.size()) {
            fail("trailing backslash");
            return false;
        }
        const char e = p_[i_++];
        kind = 0;
        switch (e) {
        case 'd':
            kind = KDigit;
            return false;
        case 'D':
            kind = KNotDigit;
            return false;
        case 's':
            kind = KSpace;
            return false;
        case 'S':
            kind = KNotSpace;
            return false;
        case 'w':
            kind = KWord;
            return false;
        case 'W':
            kind = KNotWord;
            return false;
        case 'n':
            out = '\n';
            return true;
        case 'r':
            out = '\r';
            return true;
        case 't':
            out = '\t';
            return true;
        case 'f':
            out = '\f';
            return true;
        case 'v':
            out = '\v';
            return true;
        case '0':
            out = '\0';
            return true;
        case 'b':
            if (in_class) {
                out = '\b';
                return true;
            }
            break;
        case 'x':
            if (i_ + 1 < p_.size() && hex(p_[i_]) >= 0 && hex(p_[i_ + 1]) >= 0) {
                out = static_cast<char>(hex(p_[i_]) * 16 + hex(p_[i_ + 1]));
                i_ += 2;
                return true;
            }
            fail("invalid \\x escape");
            return false;
        default:
            break;
        }
        if (std::isdigit(static_cast<unsigned char>(e))) {
            fail("backreferences are not supported");
            return false;
        }
        if (std::isalnum(static_cast<unsigned char>(e))) {
            fail("unknown escape");
            return false;
        }
        out = e;
        return true;
    }

    NodePtr char_class() {
        Regex::CharClass cc;
        if (i_ < p_.size() && p_[i_] == '^') {
            cc.negate = true;
            ++i_;
        }
        while (true) {
            if (i_ >= p_.size()) {
                fail("unterminated character class");
                return nullptr;
            }
            char c = p_[i_];
            if (c == ']') {
                ++i_;
                break;
            }
            if (c == '[' && i_ + 1 < p_.size() &&
                (p_[i_ + 1] == ':' || p_[i_ + 1] == '.' || p_[i_ + 1] == '=')) {
                fail("POSIX class names are not supported");
                return nullptr;
            }
            ++i_;
            uint8_t kind = 0;
            if (c == '\\') {
                if (!escape(c, kind, true)) {
                    if (!err_.empty())
                        return nullptr;
                    cc.kinds |= kind;
                    continue;
                }
            }
            unsigned char lo = static_cast<unsigned char>(c);
            unsigned char hi = lo;
            if (i_ + 1 < p_.size() && p_[i_] == '-' && p_[i_ + 1] != ']') {
                ++i_;
                char h = p_[i_++];
                if (h == '\\') {
                    uint8_t k2 = 0;
                    if (!escape(h, k2, true)) {
                        fail("invalid range in character class");
                        return nullptr;
                    }
                }
                hi = static_cast<unsigned char>(h);
                if (hi < lo) {
                    fail("invalid range in character class");
                    return nullptr;
                }
            }
            cc.ranges.emplace_back(lo, hi);
        }
        auto n = make(Node::Class);
        n->index = static_cast<int>(classes_.size());
        classes_.push_back(std::move(cc));
        return n;
    }

    NodePtr atom() {
        const char c = p_[i_++];
        switch (c) {
        case '.':
            return make(Node::Any);
        case '^':
            return make(Node::Bol);
        case '$':
            return make(Node::Eol);
        case '[':
            return char_class();
        case '*':
        case '+':
        case '?':
        case '{':
            --i_;
            fail(c == '{' ? "invalid range in '{}'" : "nothing to repeat");
            return nullptr;
        case '(': {
            NodePtr g;
            if (i_ + 1 < p_.size() && p_[i_] == '?') {
                const char k = p_[i_ + 1];
                if (k == ':') {
                    g = make(Node::Group);
                } else if (k == '=' || k == '!') {
                    g = make(Node::Look);
                    g->neg = (k == '!');
                } else {
                    fail("unsupported group syntax");
                    return nullptr;
                }
                i_ += 2;
            } else {
                g = make(Node::Group);
                g->index = static_cast<int>(++groups_);
            }
            g->kids.push_back(alternation());
            if (!err_.empty())
                return nullptr;
            if (i_ >= p_.size() || p_[i_] != ')') {
                fail("unmatched '('");
                return nullptr;
            }
            ++i_;
            return g;
        }
        case '\\': {
            if (i_ < p_.size() && (p_[i_] == 'b' || p_[i_] == 'B')) {
                return make(p_[i_++] == 'b' ? Node::WordB : Node::NotWordB);
            }
            char out = 0;
            uint8_t kind = 0;
            if (escape(out, kind, false)) {
                auto n = make(Node::Char);
                n->c = out;
                return n;
            }
            if (!err_.empty())
                return nullptr;
            Regex::CharClass cc;
            cc.kinds = kind;
            auto n = make(Node::Class);
            n->index = static_cast<int>(classes_.size());
            classes_.push_back(std::move(cc));
            return n;
        }
        default: {
            auto n = make(Node::Char);
            n->c = c;
            return n;
        }
        }
    }
};

bool nullable(const Node& n) {
    switch (n.type) {
    case Node::Char:
    case Node::Any:
    case Node::Class:
        return false;
    case Node::Seq:
        for (const auto& k : n.kids)
            if (!nullable(*k))
                return false;
        return true;
    case Node::Alt:
        for (const auto& k : n.kids)
            if (nullable(*k))
                return true;
        return false;
    case Node::Group:
        return nullable(*n.kids[0]);
    case Node::Repeat:
        return n.min == 0 || nullable(*n.kids[0]);
    default:
        return true;
    }
}

class Emitter {
  public:
    std::vector<Regex::Inst>& prog;
    int marks = 0;

    int emit(Regex::Op op, int x = 0, int y = 0) {
        Regex::Inst in{op};
        in.x = x;
        in.y = y;
        prog.push_back(in);
        return static_cast<int>(prog.size()) - 1;
    }
    int pc() const {
        return static_cast<int>(prog.size());
    }

    void node(const Node& n) {
        using Op = Regex::Op;
        switch (n.type) {
        case Node::Seq:
            for (const auto& k : n.kids)
                node(*k);
            break;
        case Node::Alt: {
            std::vector<int> jumps;
            for (size_t i = 0; i < n.kids.size(); ++i) {
                if (i + 1 < n.kids.size()) {
                    int split = emit(Op::Split);
                    prog[split].x = pc();
                    node(*n.kids[i]);
                    jumps.push_back(emit(Op::Jmp));
                    prog[split].y = pc();
                } else {
                    node(*n.kids[i]);
                }
            }
            for (int j : jumps)
                prog[j].x = pc();
            break;
        }
        case Node::Char: {
            int at = emit(Op::Char);
            prog[at].c = n.c;
            break;
        }
        case Node::Any:
            emit(Op::Any);
            break;
        case Node::Class:
            emit(Op::Class, n.index);
            break;
        case Node::Bol:
            emit(Op::Bol);
            break;
        case Node::Eol:
            emit(Op::Eol);
            break;
        case Node::WordB:
            emit(Op::WordB);
            break;
        case Node::NotWordB:
            emit(Op::NotWordB);
            break;
        case Node::Group:
            if (n.index >= 0)
                emit(Op::Save, n.index * 2);
            node(*n.kids[0]);
            if (n.index >= 0)
                emit(Op::Save, n.index * 2 + 1);
            break;
        case Node::Look: {
            int look = emit(Op::Look);
            prog[look].neg = n.neg;
            node(*n.kids[0]);
            emit(Op::LookEnd);
            prog[look].x = pc();
            break;
        }
        case Node::Repeat:
            repeat(n);
            break;
        }
    }

    void repeat(const Node& n) {
        using Op = Regex::Op;
        const Node& body = *n.kids[0];
        for (int i = 0; i < n.min; ++i)
            node(body);
        if (n.max == -1) {
            // An iteration that consumed nothing is kept but ends the loop,
            // which is what libstdc++ does with (a*)* and what stops it
            // spinning forever.
            const bool empty_ok = nullable(body);
            const int mark = empty_ok ? marks++ : -1;
            int loop = emit(Op::Split);
            int body_pc = pc();
            if (empty_ok)
                emit(Op::Mark, mark);
            node(body);
            int check = -1;
            if (empty_ok)
                check = emit(Op::Check, mark);
            emit(Op::Jmp, loop);
            int exit = pc();
            prog[loop].x = n.greedy ? body_pc : exit;
            prog[loop].y = n.greedy ? exit : body_pc;
            if (check >= 0)
                prog[check].y = exit;
            return;
        }
        std::vector<int> splits;
        for (int i = n.min; i < n.max; ++i) {
            int split = emit(Op::Split);
            splits.push_back(split);
            prog[split].x = pc();
            node(body);
        }
        int exit = pc();
        for (int s : splits) {
            int body_pc = prog[s].x;
            prog[s].x = n.greedy ? body_pc : exit;
            prog[s].y = n.greedy ? exit : body_pc;
        }
    }
};

} // namespace

Regex::Regex(std::string_view pattern, unsigned flags) : icase_((flags & ICase) != 0) {
    Parser parser(pattern, classes_);
    NodePtr root = parser.parse();
    if (!parser.error().empty()) {
        error_ = parser.error();
        classes_.clear();
        return;
    }
    groups_ = parser.groups();
    Emitter em{prog_};
    em.emit(Op::Save, 0);
    em.node(*root);
    em.emit(Op::Save, 1);
    em.emit(Op::Match);
    marks_ = em.marks;
    anchored_ = prog_.size() > 1 && prog_[1].op == Op::Bol;
}

class RegexMatcher {
  public:
    RegexMatcher(const Regex& re, std::string_view s) : re_(re), s_(s) {}

    // Tries one start position. `full` demands the match end at the subject's
    // end; `not_null` rejects an empty match.
    bool run_at(size_t start, bool full, bool not_null) {
        caps_.assign((re_.groups_ + 1) * 2, -1);
        marks_.assign(static_cast<size_t>(re_.marks_), -1);
        full_ = full;
        not_null_ = not_null;
        start_ = start;
        return run(0, static_cast<long>(start), /*look=*/false) && !aborted_;
    }

    bool search(size_t from, bool not_null_at_from, RegexMatch& m) {
        steps_ = 0;
        aborted_ = false;
        if (!re_.ok() || from > s_.size())
            return false;
        size_t first = from;
        if (not_null_at_from) {
            if (run_at(from, false, true))
                return fill(m);
            ++first;
        }
        for (size_t start = first; start <= s_.size() && !aborted_; ++start) {
            if (re_.anchored_ && start != 0)
                break;
            if (run_at(start, false, false))
                return fill(m);
        }
        return false;
    }

    bool aborted() const {
        return aborted_;
    }

    bool match_whole(RegexMatch& m) {
        steps_ = 0;
        aborted_ = false;
        return re_.ok() && run_at(0, true, false) && fill(m);
    }

  private:
    struct Frame {
        bool undo; ///< true: restore slot; false: resume at pc/pos
        bool mark; ///< undo targets marks_ rather than caps_
        int a;     ///< pc or slot
        long b;    ///< pos or old value
    };

    const Regex& re_;
    std::string_view s_;
    std::vector<long> caps_;
    std::vector<long> marks_;
    std::vector<Frame> stack_;
    size_t steps_ = 0;
    bool aborted_ = false;
    bool full_ = false;
    bool not_null_ = false;
    size_t start_ = 0;

    bool fill(RegexMatch& m) {
        m.subject_ = s_;
        m.subs_.assign(re_.groups_ + 1, RegexMatch::Sub{});
        for (size_t g = 0; g <= re_.groups_; ++g) {
            long b = caps_[g * 2], e = caps_[g * 2 + 1];
            if (b >= 0 && e >= b) {
                m.subs_[g].matched = true;
                m.subs_[g].view = s_.substr(static_cast<size_t>(b), static_cast<size_t>(e - b));
            }
        }
        return true;
    }

    bool char_eq(char pat, unsigned char c) const {
        if (static_cast<unsigned char>(pat) == c)
            return true;
        return re_.icase_ && std::tolower(static_cast<unsigned char>(pat)) == std::tolower(c);
    }

    bool class_has(const Regex::CharClass& cc, unsigned char c) const {
        auto in = [&](unsigned char ch) {
            if (cc.kinds && kinds_match(cc.kinds, ch))
                return true;
            for (const auto& r : cc.ranges)
                if (ch >= r.first && ch <= r.second)
                    return true;
            return false;
        };
        bool hit = in(c);
        if (!hit && re_.icase_) {
            hit = in(static_cast<unsigned char>(std::tolower(c))) ||
                  in(static_cast<unsigned char>(std::toupper(c)));
        }
        return hit != cc.negate;
    }

    bool word_at(long pos) const {
        return pos >= 0 && pos < static_cast<long>(s_.size()) &&
               is_word(static_cast<unsigned char>(s_[static_cast<size_t>(pos)]));
    }

    // Runs from pc at pos. In a lookahead body, LookEnd is the success state.
    bool run(int pc, long pos, bool look) {
        using Op = Regex::Op;
        // Frames below `base` belong to an enclosing run (a lookahead's caller).
        const size_t base = stack_.size();
        struct Unwind {
            std::vector<Frame>& s;
            size_t base;
            ~Unwind() {
                s.resize(base);
            }
        } unwind{stack_, base};
        const long n = static_cast<long>(s_.size());
        const auto& prog = re_.prog_;
        while (true) {
            if (++steps_ > kStepLimit) {
                aborted_ = true;
                return false;
            }
            const Regex::Inst& in = prog[static_cast<size_t>(pc)];
            bool ok = true;
            switch (in.op) {
            case Op::Char:
                ok = pos < n &&
                     char_eq(in.c, static_cast<unsigned char>(s_[static_cast<size_t>(pos)]));
                if (ok)
                    ++pos, ++pc;
                break;
            case Op::Any:
                ok = pos < n && s_[static_cast<size_t>(pos)] != '\n' &&
                     s_[static_cast<size_t>(pos)] != '\r';
                if (ok)
                    ++pos, ++pc;
                break;
            case Op::Class:
                ok = pos < n && class_has(re_.classes_[static_cast<size_t>(in.x)],
                                          static_cast<unsigned char>(s_[static_cast<size_t>(pos)]));
                if (ok)
                    ++pos, ++pc;
                break;
            case Op::Split:
                stack_.push_back({false, false, in.y, pos});
                pc = in.x;
                break;
            case Op::Jmp:
                pc = in.x;
                break;
            case Op::Save:
                stack_.push_back({true, false, in.x, caps_[static_cast<size_t>(in.x)]});
                caps_[static_cast<size_t>(in.x)] = pos;
                ++pc;
                break;
            case Op::Mark:
                stack_.push_back({true, true, in.x, marks_[static_cast<size_t>(in.x)]});
                marks_[static_cast<size_t>(in.x)] = pos;
                ++pc;
                break;
            case Op::Check:
                pc = (marks_[static_cast<size_t>(in.x)] == pos) ? in.y : pc + 1;
                break;
            case Op::Bol:
                ok = pos == 0;
                ++pc;
                break;
            case Op::Eol:
                ok = pos == n;
                ++pc;
                break;
            case Op::WordB:
            case Op::NotWordB: {
                const bool edge = word_at(pos - 1) != word_at(pos);
                ok = (in.op == Op::WordB) == edge;
                ++pc;
                break;
            }
            case Op::Look: {
                std::vector<long> saved = caps_;
                const bool hit = run(pc + 1, pos, true);
                if (aborted_)
                    return false;
                if (in.neg || !hit) {
                    caps_ = std::move(saved);
                    ok = hit != in.neg;
                } else {
                    for (size_t i = 0; i < caps_.size(); ++i)
                        if (caps_[i] != saved[i])
                            stack_.push_back({true, false, static_cast<int>(i), saved[i]});
                }
                pc = in.x;
                break;
            }
            case Op::LookEnd:
                if (look)
                    return true;
                ++pc;
                break;
            case Op::Match:
                if ((full_ && pos != n) || (not_null_ && pos == static_cast<long>(start_))) {
                    ok = false;
                    break;
                }
                return true;
            }
            if (ok)
                continue;
            // Backtrack: unwind undo records to the most recent choice point.
            while (true) {
                if (stack_.size() == base)
                    return false;
                Frame f = stack_.back();
                stack_.pop_back();
                if (f.undo) {
                    (f.mark ? marks_ : caps_)[static_cast<size_t>(f.a)] = f.b;
                    continue;
                }
                pc = f.a;
                pos = f.b;
                break;
            }
        }
    }
};

size_t RegexMatch::position(size_t i) const {
    if (i >= subs_.size() || !subs_[i].matched)
        return std::string_view::npos;
    return static_cast<size_t>(subs_[i].view.data() - subject_.data());
}

bool regex_search(std::string_view subject, RegexMatch& m, const Regex& re) {
    m = RegexMatch{};
    return RegexMatcher(re, subject).search(0, false, m);
}

bool regex_search(std::string_view subject, const Regex& re) {
    RegexMatch m;
    return regex_search(subject, m, re);
}

bool regex_match(std::string_view subject, RegexMatch& m, const Regex& re) {
    m = RegexMatch{};
    return RegexMatcher(re, subject).match_whole(m);
}

bool regex_match(std::string_view subject, const Regex& re) {
    RegexMatch m;
    return regex_match(subject, m, re);
}

std::string regex_replace(std::string_view subject, const Regex& re, std::string_view fmt,
                          bool* aborted) {
    std::string out;
    size_t last = 0;
    RegexIterator it(subject, re);
    for (const RegexIterator end; it != end; ++it) {
        const RegexMatch& m = *it;
        out.append(subject.substr(last, m.position(0) - last));
        for (size_t i = 0; i < fmt.size(); ++i) {
            const char c = fmt[i];
            if (c != '$' || i + 1 >= fmt.size()) {
                out.push_back(c);
                continue;
            }
            const char k = fmt[i + 1];
            if (k == '$') {
                out.push_back('$');
                ++i;
            } else if (k == '&') {
                out.append(m[0].view);
                ++i;
            } else if (k == '`') {
                out.append(subject.substr(0, m.position(0)));
                ++i;
            } else if (k == '\'') {
                out.append(subject.substr(m.end()));
                ++i;
            } else if (std::isdigit(static_cast<unsigned char>(k))) {
                size_t g = static_cast<size_t>(k - '0');
                size_t used = 1;
                if (i + 2 < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i + 2]))) {
                    size_t two = g * 10 + static_cast<size_t>(fmt[i + 2] - '0');
                    if (two >= 1 && two < m.size()) {
                        g = two;
                        used = 2;
                    }
                }
                if (g >= 1 && g < m.size()) {
                    out.append(m[g].view);
                    i += used;
                } else {
                    out.push_back(c);
                }
            } else {
                out.push_back(c);
            }
        }
        last = m.end();
    }
    out.append(subject.substr(last));
    if (aborted != nullptr)
        *aborted = it.aborted();
    return out;
}

RegexIterator::RegexIterator(std::string_view subject, const Regex& re)
    : subject_(subject), re_(&re) {
    RegexMatcher matcher(re, subject);
    if (!matcher.search(0, false, match_)) {
        re_ = nullptr;
        aborted_ = matcher.aborted();
    }
}

RegexIterator& RegexIterator::operator++() {
    if (re_ == nullptr)
        return *this;
    const size_t from = match_.end();
    const bool was_empty = match_.length(0) == 0;
    if (was_empty && from >= subject_.size()) {
        re_ = nullptr;
        return *this;
    }
    RegexMatch next;
    RegexMatcher matcher(*re_, subject_);
    if (!matcher.search(from, was_empty, next)) {
        re_ = nullptr;
        aborted_ = matcher.aborted();
        return *this;
    }
    match_ = std::move(next);
    return *this;
}

} // namespace helix
