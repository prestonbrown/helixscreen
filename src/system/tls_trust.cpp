// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/tls_trust.h"

#include "app_globals.h"
#include "hv/HttpClient.h"
#include "hv/hconfig.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

#ifdef WITH_OPENSSL
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#endif

namespace helix::tls {

namespace {

bool readable(const char* path) {
    return path && *path && access(path, R_OK) == 0;
}

#ifdef WITH_OPENSSL
/// The address the SSL's socket is connected to. libhv sends no SNI for an IP-literal
/// host, so for those this is the only record of what the caller asked for.
std::string peer_ip(SSL* ssl) {
    const int fd = ssl ? SSL_get_fd(ssl) : -1;
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (fd < 0 || getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return {};
    char buf[INET6_ADDRSTRLEN] = {};
    if (addr.ss_family == AF_INET)
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&addr)->sin_addr, buf, sizeof(buf));
    else if (addr.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&addr)->sin6_addr, buf, sizeof(buf));
    return buf;
}

/// Chain verification is OpenSSL's; this adds the name check at the leaf. libhv shares
/// one context across every connection, so the host comes from the SSL itself: the SNI
/// name libhv sets for a hostname, or the peer address for an IP literal.
int verify_cb(int ok, X509_STORE_CTX* store) {
    auto* ssl =
        static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    const char* name = ssl ? SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name) : nullptr;
    const std::string host = name ? std::string(name) : peer_ip(ssl);

    if (!ok) {
        spdlog::error("[TLS] Certificate for '{}' rejected: {}", host,
                      X509_verify_cert_error_string(X509_STORE_CTX_get_error(store)));
        return 0;
    }
    if (X509_STORE_CTX_get_error_depth(store) != 0)
        return 1;

    X509* leaf = X509_STORE_CTX_get_current_cert(store);
    const bool match =
        name ? X509_check_host(leaf, name, 0, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr) == 1
             : !host.empty() && X509_check_ip_asc(leaf, host.c_str(), 0) == 1;
    if (!match) {
        X509_STORE_CTX_set_error(store, X509_V_ERR_HOSTNAME_MISMATCH);
        spdlog::error("[TLS] Certificate does not match host '{}'", host);
        return 0;
    }
    spdlog::debug("[TLS] Verified certificate for '{}'", host);
    return 1;
}
#endif

} // namespace

CaStore find_ca_store(const std::vector<std::string>& system_files,
                      const std::string& bundled_file) {
    CaStore env;
    if (const char* f = std::getenv("SSL_CERT_FILE"); readable(f))
        env.file = f;
    // An empty directory trusts nothing, which would fail every request closed.
    std::error_code ec;
    if (const char* d = std::getenv("SSL_CERT_DIR");
        readable(d) && !std::filesystem::is_empty(d, ec) && !ec)
        env.dir = d;
    if (!env.empty())
        return env;
    for (const auto& f : system_files)
        if (readable(f.c_str()))
            return {f, {}};
    if (readable(bundled_file.c_str()))
        return {bundled_file, {}};
    return {};
}

CaStore find_ca_store() {
    const std::string root = app_get_install_root();
    return find_ca_store({"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                          "/etc/ssl/cert.pem"},
                         root.empty() ? std::string() : root + "/certs/ca-certificates.crt");
}

void* make_client_ctx(const CaStore& store) {
#ifdef WITH_OPENSSL
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx)
        return nullptr;
    if (SSL_CTX_load_verify_locations(ctx, store.file.empty() ? nullptr : store.file.c_str(),
                                      store.dir.empty() ? nullptr : store.dir.c_str()) != 1)
        spdlog::error("[TLS] Could not load CA store (file '{}', dir '{}'); HTTPS will fail",
                      store.file, store.dir);
    // Printers often boot with a 1970 clock until NTP syncs, and update checks run
    // early. Chain and hostname verification still defeat a MITM; validity dates
    // would only break updates on those devices.
    X509_VERIFY_PARAM_set_flags(SSL_CTX_get0_param(ctx), X509_V_FLAG_NO_CHECK_TIME);
    SSL_CTX_set_mode(ctx, SSL_CTX_get_mode(ctx) | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, verify_cb);
    return ctx;
#else
    (void)store;
    return nullptr;
#endif
}

namespace detail {

std::string resolve_location(const std::string& current_url, const std::string& location) {
    if (location.find("://") != std::string::npos)
        return location;
    const size_t scheme_end = current_url.find("://");
    if (scheme_end == std::string::npos)
        return location;
    if (location.rfind("//", 0) == 0)
        return current_url.substr(0, scheme_end + 1) + location;
    const size_t path_start = current_url.find_first_of("/?#", scheme_end + 3);
    const std::string origin = current_url.substr(0, path_start);
    if (location.rfind('/', 0) == 0)
        return origin + location;
    std::string path = path_start == std::string::npos ? "/" : current_url.substr(path_start);
    path = path.substr(0, path.find_first_of("?#"));
    return origin + path.substr(0, path.rfind('/') + 1) + location;
}

HttpResponsePtr trusted_request(const HttpRequestPtr& req, void* ssl_ctx) {
    // libhv follows a redirect on a fresh default client, which would drop the
    // verifying context, so redirects are followed here instead.
    constexpr int kMaxRedirects = 5;
    req->redirect = false;
    auto resp = std::make_shared<HttpResponse>();
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        hv::HttpClient cli;
        if (ssl_ctx)
            cli.setSslCtx(ssl_ctx);
        if (cli.send(req.get(), resp.get()) != 0)
            return nullptr;
        const std::string location = resp->GetHeader("Location");
        if (!HTTP_STATUS_IS_REDIRECT(resp->status_code) || location.empty())
            return resp;
        const bool was_https = req->IsHttps();
        const std::string old_origin =
            req->scheme + "://" + req->host + ":" + std::to_string(req->port);
        req->url = resolve_location(req->url, location);
        req->headers.erase("Host"); // ParseUrl() refills it, with any non-default port
        req->ParseUrl();
        if (was_https && !req->IsHttps()) {
            spdlog::warn("[TLS] Refusing redirect from https to plain http ({})", req->host);
            return nullptr;
        }
        // Credentials belong to the origin they were issued for, not to a redirect target.
        if (req->scheme + "://" + req->host + ":" + std::to_string(req->port) != old_origin) {
            for (const char* h : {"X-API-Key", "Authorization", "Cookie", "Proxy-Authorization"})
                req->headers.erase(h);
        }
        // The host only: a redirect URL can carry a signed, short-lived download token.
        spdlog::debug("[TLS] Following redirect to {}", req->host);
        resp->Reset();
    }
    spdlog::warn("[TLS] Too many redirects fetching {}", req->url);
    return nullptr;
}

} // namespace detail

HttpResponsePtr trusted_request(const HttpRequestPtr& req) {
    static void* const ctx = []() -> void* {
        const CaStore store = find_ca_store();
        if (store.empty()) {
            spdlog::warn("[TLS] No CA certificate store found; update and telemetry servers "
                         "will NOT be verified. Set SSL_CERT_FILE or install ca-certificates.");
            return nullptr;
        }
        spdlog::info("[TLS] Verifying our servers against {}{}", store.file,
                     store.dir.empty() ? "" : " + " + store.dir);
        return make_client_ctx(store);
    }();
    return detail::trusted_request(req, ctx);
}

size_t trusted_download(const std::string& url, const std::string& path,
                        const std::function<void(size_t received, size_t total)>& progress) {
    const std::string partial = path + ".download";
    FILE* file = std::fopen(partial.c_str(), "wb");
    if (!file)
        return 0;
    auto req = std::make_shared<HttpRequest>();
    req->method = HTTP_GET;
    req->url = url;
    req->timeout = 3600;
    size_t content_length = 0;
    size_t received = 0;
    bool write_failed = false;
    req->http_cb = [&](HttpMessage* resp, http_parser_state state, const char* data, size_t size) {
        if (!resp->GetHeader("Location").empty())
            return;
        if (state == HP_HEADERS_COMPLETE) {
            content_length = std::strtoull(resp->GetHeader("Content-Length").c_str(), nullptr, 10);
        } else if (state == HP_BODY && data && size) {
            write_failed |= std::fwrite(data, 1, size, file) != size;
            received += size;
            if (progress)
                progress(received, content_length);
        }
    };
    auto resp = trusted_request(req);
    std::fclose(file);
    if (!resp || resp->status_code != 200 || write_failed ||
        (content_length != 0 && received != content_length)) {
        std::remove(partial.c_str());
        return 0;
    }
    if (std::rename(partial.c_str(), path.c_str()) != 0)
        return 0;
    return received;
}

} // namespace helix::tls
