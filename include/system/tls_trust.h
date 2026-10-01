// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "hv/HttpMessage.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

/// Certificate verification for the app's own internet endpoints.
///
/// Only requests to our servers verify: the update manifest, release downloads and
/// changelog, telemetry, crash reports and debug bundle uploads. They go through
/// trusted_request() / trusted_download(). Every other libhv request (Moonraker,
/// cameras, Spoolman, IPP, plugins, anything the user typed) keeps libhv's unverified
/// default, because self-signed certificates are normal on printer LANs.
namespace helix::tls {

/// Where the CA certificates live. Both empty means no store was found.
struct CaStore {
    std::string file;
    std::string dir;
    bool empty() const {
        return file.empty() && dir.empty();
    }
};

/// Resolves the CA store: a readable $SSL_CERT_FILE / $SSL_CERT_DIR first, then the
/// non-empty `bundled_file`, then the first readable entry of `system_files`.
CaStore find_ca_store(const std::vector<std::string>& system_files,
                      const std::string& bundled_file);

/// find_ca_store() over the certs/ca-certificates.crt shipped next to the install,
/// then the usual system bundle paths.
CaStore find_ca_store();

/// A client SSL_CTX (as libhv's hssl_ctx_t) that verifies the server's chain against
/// `store` and its name against the host being connected to. Never null when OpenSSL
/// is built in: a store that fails to load leaves the context with no trust anchors,
/// so every handshake fails instead of passing unverified. Null without OpenSSL.
void* make_client_ctx(const CaStore& store);

/// requests::request() for one of our own endpoints: the server certificate is
/// verified, and redirects are followed on the same verifying context. Null on any
/// transport or verification failure. With no CA store on the device it logs one
/// warning and sends unverified.
HttpResponsePtr trusted_request(const HttpRequestPtr& req);

/// requests::downloadFile() over trusted_request().
size_t trusted_download(const std::string& url, const std::string& path,
                        const std::function<void(size_t received, size_t total)>& progress);

namespace detail {
/// `location` from a redirect response, resolved against the URL that returned it.
std::string resolve_location(const std::string& current_url, const std::string& location);

/// trusted_request() with an explicit context, for tests.
HttpResponsePtr trusted_request(const HttpRequestPtr& req, void* ssl_ctx);
} // namespace detail

} // namespace helix::tls
