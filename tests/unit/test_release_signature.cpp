// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/release_signature.h"

#include <fstream>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::release_signature::Verdict;
using helix::release_signature::verify;

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
