// SPDX-License-Identifier: GPL-3.0-or-later
//
// The record encoder behind tokenized logging. See log_token.h.

#include "log_token.h"

#include <cstring>
#include <type_traits>

namespace helix::logtok {

bool ByteWriter::put(std::uint8_t b) {
    return put_bytes(&b, 1);
}

bool ByteWriter::put_bytes(const void* p, std::size_t n) {
    if (full_ || n > cap_ - len_) {
        full_ = true;
        return false;
    }
    std::memcpy(out_ + len_, p, n);
    len_ += n;
    return true;
}

std::size_t uleb_size(std::uint64_t v) {
    std::size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

bool ByteWriter::put_uleb(std::uint64_t v) {
    std::uint8_t buf[10];
    std::size_t n = 0;
    do {
        std::uint8_t b = v & 0x7f;
        v >>= 7;
        if (v != 0) {
            b |= 0x80;
        }
        buf[n++] = b;
    } while (v != 0);
    return put_bytes(buf, n);
}

bool ByteWriter::put_sleb_zigzag(std::int64_t v) {
    const std::uint64_t u = static_cast<std::uint64_t>(v);
    return put_uleb((u << 1) ^ (v < 0 ? ~std::uint64_t{0} : 0));
}

namespace {

// Writes one argument as a tag + payload, all or nothing. Returns false when it
// did not fit, leaving the writer where it was.
class ArgEncoder {
  public:
    explicit ArgEncoder(ByteWriter& w) : w_(w) {}

    template <typename T> void operator()(T v) {
        if constexpr (std::is_same_v<T, fmt::monostate>) {
            ok_ = true;
        } else if constexpr (std::is_same_v<T, bool>) {
            fixed(ArgTag::kBool, static_cast<std::uint8_t>(v ? 1 : 0));
        } else if constexpr (std::is_same_v<T, char>) {
            fixed(ArgTag::kChar, static_cast<std::uint8_t>(v));
        } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            const auto x = static_cast<std::int64_t>(v);
            ok_ = room(1 + uleb_size((static_cast<std::uint64_t>(x) << 1) ^
                                     (x < 0 ? ~std::uint64_t{0} : 0))) &&
                  w_.put(static_cast<std::uint8_t>(ArgTag::kInt)) && w_.put_sleb_zigzag(x);
        } else if constexpr (std::is_integral_v<T>) {
            const auto x = static_cast<std::uint64_t>(v);
            ok_ = room(1 + uleb_size(x)) && w_.put(static_cast<std::uint8_t>(ArgTag::kUint)) &&
                  w_.put_uleb(x);
        } else if constexpr (std::is_same_v<T, float>) {
            ok_ = room(5) && w_.put(static_cast<std::uint8_t>(ArgTag::kFloat));
            if (ok_) {
                w_.put_bytes(&v, 4); // Xtensa and the test hosts are little-endian
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            const double d = static_cast<double>(v);
            ok_ = room(9) && w_.put(static_cast<std::uint8_t>(ArgTag::kDouble));
            if (ok_) {
                w_.put_bytes(&d, 8);
            }
        } else if constexpr (std::is_same_v<T, const char*>) {
            string(v != nullptr ? std::string_view(v) : std::string_view("(null)"));
        } else if constexpr (std::is_same_v<T, fmt::string_view>) {
            string(std::string_view(v.data(), v.size()));
        } else if constexpr (std::is_same_v<T, const void*>) {
            const auto x = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(v));
            ok_ = room(1 + uleb_size(x)) && w_.put(static_cast<std::uint8_t>(ArgTag::kPointer)) &&
                  w_.put_uleb(x);
        } else {
            ok_ = room(1) && w_.put(static_cast<std::uint8_t>(ArgTag::kUnsupported));
        }
    }

    bool ok() const {
        return ok_;
    }
    bool cut() const {
        return cut_;
    }

  private:
    bool room(std::size_t n) const {
        return n <= w_.remaining();
    }

    void fixed(ArgTag tag, std::uint8_t b) {
        ok_ = room(2) && w_.put(static_cast<std::uint8_t>(tag)) && w_.put(b);
    }

    // A string longer than the room left is cut to fit, so a long path or
    // response still shows its start; one with no room at all is dropped.
    void string(std::string_view s) {
        const std::size_t whole = 1 + uleb_size(s.size()) + s.size();
        if (room(whole)) {
            ok_ = w_.put(static_cast<std::uint8_t>(ArgTag::kString)) && w_.put_uleb(s.size()) &&
                  w_.put_bytes(s.data(), s.size());
            return;
        }
        if (w_.remaining() < 3) {
            ok_ = false;
            return;
        }
        std::size_t n = w_.remaining() - 1 - uleb_size(w_.remaining());
        ok_ = w_.put(static_cast<std::uint8_t>(ArgTag::kStringCut)) && w_.put_uleb(n) &&
              w_.put_bytes(s.data(), n);
        cut_ = true;
    }

    ByteWriter& w_;
    bool ok_ = false;
    bool cut_ = false;
};

} // namespace

std::size_t encode_record(int level, std::uint32_t token, fmt::format_args args, std::uint8_t* out,
                          std::size_t cap) {
    ByteWriter w(out, cap);
    if (!w.put(static_cast<std::uint8_t>(level & 0x7f)) || !w.put_uleb(token)) {
        return 0;
    }
    bool truncated = false;
    for (int i = 0;; ++i) {
        const auto arg = args.get(i);
        if (!arg) {
            break;
        }
        ArgEncoder enc(w);
        arg.visit(enc);
        if (!enc.ok() || enc.cut()) {
            truncated = true;
        }
        if (!enc.ok()) {
            break;
        }
    }
    if (truncated) {
        out[0] |= kTruncatedFlag;
    }
    return w.size();
}

std::size_t base64_encode(const std::uint8_t* in, std::size_t n, char* out, std::size_t cap) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::size_t need = (n * 4 + 2) / 3;
    if (cap < need + 1) {
        return 0;
    }
    std::size_t o = 0;
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const std::uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = kAlphabet[(v >> 18) & 63];
        out[o++] = kAlphabet[(v >> 12) & 63];
        out[o++] = kAlphabet[(v >> 6) & 63];
        out[o++] = kAlphabet[v & 63];
    }
    if (n - i == 1) {
        const std::uint32_t v = in[i] << 16;
        out[o++] = kAlphabet[(v >> 18) & 63];
        out[o++] = kAlphabet[(v >> 12) & 63];
    } else if (n - i == 2) {
        const std::uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
        out[o++] = kAlphabet[(v >> 18) & 63];
        out[o++] = kAlphabet[(v >> 12) & 63];
        out[o++] = kAlphabet[(v >> 6) & 63];
    }
    out[o] = '\0';
    return o;
}

} // namespace helix::logtok
