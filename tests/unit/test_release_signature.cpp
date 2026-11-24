// SPDX-License-Identifier: GPL-3.0-or-later

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "system/release_signature.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::release_signature::Verdict;
using helix::release_signature::verify;
using helix::release_signature::verify_channel_manifest;

namespace {

std::string repo_root() {
    std::string src = __FILE__;
    const auto pos = src.rfind("/tests/unit/");
    return pos != std::string::npos ? src.substr(0, pos) + "/" : "";
}

std::string read_file(const std::string& rel) {
    std::ifstream f(repo_root() + rel, std::ios::binary);
    INFO("unreadable: " << repo_root() + rel);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string fixture(const std::string& name) {
    return read_file("tests/fixtures/release_signing/" + name);
}

// The base64 key line (line 2) of a minisign .pub file.
std::string key_line(const std::string& pub) {
    const auto first = pub.find('\n');
    const auto second = pub.find('\n', first + 1);
    return pub.substr(first + 1, second - first - 1);
}

std::string b64(const uint8_t* data, size_t len) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t n = (uint32_t{data[i]} << 16) |
                           (i + 1 < len ? uint32_t{data[i + 1]} << 8 : 0) |
                           (i + 2 < len ? uint32_t{data[i + 2]} : 0);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < len ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < len ? kAlphabet[n & 63] : '=';
    }
    return out;
}

// A throwaway minisign key made in the test from a fixed seed, signing as `minisign -S` does
// (prehashed, algorithm "ED").
class TestKey {
  public:
    explicit TestKey(uint8_t seed_byte) {
        std::array<uint8_t, 32> seed{};
        seed.fill(seed_byte);
        crypto_ed25519_key_pair(secret_.data(), public_.data(), seed.data());
        id_.fill(static_cast<uint8_t>(seed_byte ^ 0x5a));
    }

    // The base64 key line of the .pub file.
    std::string pub() const {
        std::vector<uint8_t> raw{'E', 'd'};
        raw.insert(raw.end(), id_.begin(), id_.end());
        raw.insert(raw.end(), public_.begin(), public_.end());
        return b64(raw.data(), raw.size());
    }

    // The text of the .minisig file over @p message with @p trusted as its trusted comment.
    std::string sign(const std::string& message, const std::string& trusted) const {
        std::array<uint8_t, 64> digest{};
        crypto_blake2b(digest.data(), digest.size(),
                       reinterpret_cast<const uint8_t*>(message.data()), message.size());
        std::array<uint8_t, 64> sig{};
        crypto_ed25519_sign(sig.data(), secret_.data(), digest.data(), digest.size());

        std::vector<uint8_t> sig_line{'E', 'D'};
        sig_line.insert(sig_line.end(), id_.begin(), id_.end());
        sig_line.insert(sig_line.end(), sig.begin(), sig.end());

        std::vector<uint8_t> signed_comment(sig.begin(), sig.end());
        signed_comment.insert(signed_comment.end(), trusted.begin(), trusted.end());
        std::array<uint8_t, 64> global{};
        crypto_ed25519_sign(global.data(), secret_.data(), signed_comment.data(),
                            signed_comment.size());

        return "untrusted comment: signature from a test key\n" +
               b64(sig_line.data(), sig_line.size()) + "\ntrusted comment: " + trusted + "\n" +
               b64(global.data(), global.size()) + "\n";
    }

  private:
    std::array<uint8_t, 64> secret_{};
    std::array<uint8_t, 32> public_{};
    std::array<uint8_t, 8> id_{};
};

std::string channel_comment(const std::string& channel) {
    return "file:manifest.json channel:" + channel;
}

} // namespace

TEST_CASE("release signature: a prehashed signature by the key verifies", "[release_signature]") {
    const std::string manifest = fixture("manifest.json");
    const std::string pub = fixture("test.pub");
    const std::string sig = fixture("good.minisig");

    CHECK(verify(manifest, sig, pub) == Verdict::Ok);
    CHECK(verify(manifest, sig, key_line(pub)) == Verdict::Ok);
}

TEST_CASE("release signature: a manifest changed after signing is refused", "[release_signature]") {
    std::string manifest = fixture("manifest.json");
    const auto pos = manifest.find("99.0.0");
    REQUIRE(pos != std::string::npos);
    manifest[pos] = '8';
    CHECK(verify(manifest, fixture("good.minisig"), fixture("test.pub")) == Verdict::BadSignature);

    CHECK(verify(fixture("manifest.json") + "\n", fixture("good.minisig"), fixture("test.pub")) ==
          Verdict::BadSignature);
}

TEST_CASE("release signature: a signature by another key is refused", "[release_signature]") {
    const std::string manifest = fixture("manifest.json");
    // Signed by the other key, checked against the test key.
    CHECK(verify(manifest, fixture("other.minisig"), fixture("test.pub")) == Verdict::WrongKey);
    // Signed by the test key, checked against the other key.
    CHECK(verify(manifest, fixture("good.minisig"), fixture("other.pub")) == Verdict::WrongKey);
}

TEST_CASE("release signature: a legacy (not prehashed) signature is refused",
          "[release_signature]") {
    CHECK(verify(fixture("manifest.json"), fixture("legacy.minisig"), fixture("test.pub")) ==
          Verdict::LegacySignature);
}

TEST_CASE("release signature: an edited trusted comment is refused", "[release_signature]") {
    std::string sig = fixture("good.minisig");
    const auto pos = sig.find("channel:dev");
    REQUIRE(pos != std::string::npos);
    sig.replace(pos, 11, "channel:stb");
    CHECK(verify(fixture("manifest.json"), sig, fixture("test.pub")) == Verdict::BadSignature);
}

TEST_CASE("release signature: malformed inputs are refused, never accepted",
          "[release_signature]") {
    const std::string manifest = fixture("manifest.json");
    const std::string pub = fixture("test.pub");
    const std::string sig = fixture("good.minisig");

    CHECK(verify(manifest, "", pub) == Verdict::MalformedSignature);
    CHECK(verify(manifest, "<html>404</html>", pub) == Verdict::MalformedSignature);
    // Three of the four lines.
    CHECK(verify(manifest, sig.substr(0, sig.rfind("trusted comment:")), pub) ==
          Verdict::MalformedSignature);
    // A character outside base64 in the signature line.
    std::string bad_b64 = sig;
    bad_b64[bad_b64.find('\n') + 5] = '*';
    CHECK(verify(manifest, bad_b64, pub) == Verdict::MalformedSignature);

    CHECK(verify(manifest, sig, "") == Verdict::MalformedKey);
    CHECK(verify(manifest, sig, "RWQ=") == Verdict::MalformedKey);
}

TEST_CASE("release signature: the compiled-in key is the one release.yml verifies with",
          "[release_signature]") {
    // sign-release-manifest.sh checks every signature against .github/release-minisign.pub;
    // a client compiled with any other key would refuse every release.
    CHECK(key_line(read_file(".github/release-minisign.pub")) ==
          std::string(helix::release_signature::kReleasePublicKey));
}

TEST_CASE("release signature: a manifest signed for one channel is refused on another",
          "[release_signature]") {
    const TestKey release(1);
    const std::string pub = release.pub();
    const std::string manifest = R"({"version":"9.9.9","assets":{}})";

    for (const char* channel : {"stable", "beta", "dev"}) {
        INFO(channel);
        const std::string sig = release.sign(manifest, channel_comment(channel));
        CHECK(verify_channel_manifest(manifest, sig, channel, pub) == Verdict::Ok);
        for (const char* other : {"stable", "beta", "dev"}) {
            if (std::string(other) != channel) {
                CHECK(verify_channel_manifest(manifest, sig, other, pub) == Verdict::WrongChannel);
            }
        }
        // A signature check still comes first.
        CHECK(verify_channel_manifest(manifest + " ", sig, channel, pub) == Verdict::BadSignature);
    }

    // The channel is a whole token, and a comment naming none matches none.
    CHECK(verify_channel_manifest(manifest, release.sign(manifest, channel_comment("devel")), "dev",
                                  pub) == Verdict::WrongChannel);
    CHECK(verify_channel_manifest(manifest, release.sign(manifest, "file:manifest.json"), "dev",
                                  pub) == Verdict::WrongChannel);
    CHECK(verify_channel_manifest(manifest,
                                  release.sign(manifest, "channel:dev file:manifest.json"), "dev",
                                  pub) == Verdict::Ok);

    // The fixtures are signed for dev.
    CHECK(verify_channel_manifest(fixture("manifest.json"), fixture("good.minisig"), "dev",
                                  fixture("test.pub")) == Verdict::Ok);
    CHECK(verify_channel_manifest(fixture("manifest.json"), fixture("good.minisig"), "stable",
                                  fixture("test.pub")) == Verdict::WrongChannel);
}
