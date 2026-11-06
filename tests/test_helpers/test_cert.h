// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>
#include <filesystem>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <string>

namespace helix::test {

/// A self-signed P-256 certificate with the given subjectAltName, valid from
/// `not_before_days` to `not_after_days` relative to now.
struct TestCert {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;

    explicit TestCert(const char* san, long not_before_days = -1, long not_after_days = 30) {
        EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        EVP_PKEY_keygen_init(kctx);
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1);
        EVP_PKEY_keygen(kctx, &key);
        EVP_PKEY_CTX_free(kctx);

        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), not_before_days * 86400);
        X509_gmtime_adj(X509_getm_notAfter(cert), not_after_days * 86400);
        X509_set_pubkey(cert, key);
        X509_NAME* subj = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(subj, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("helix-test"), -1, -1, 0);
        X509_set_issuer_name(cert, subj);
        X509V3_CTX v3;
        X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, san);
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
        X509_sign(cert, key, EVP_sha256());
    }
    ~TestCert() {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
    TestCert(const TestCert&) = delete;
    TestCert& operator=(const TestCert&) = delete;

    /// Writes the certificate as `ca.pem` in `dir` and returns its path.
    std::string write_pem(const std::filesystem::path& dir) const {
        const std::string p = (dir / "ca.pem").string();
        FILE* f = fopen(p.c_str(), "w");
        PEM_write_X509(f, cert);
        fclose(f);
        return p;
    }

    /// A server context presenting this certificate; the caller frees it.
    SSL_CTX* make_server_ctx() const {
        SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
        SSL_CTX_use_certificate(ctx, cert);
        SSL_CTX_use_PrivateKey(ctx, key);
        return ctx;
    }
};

} // namespace helix::test
