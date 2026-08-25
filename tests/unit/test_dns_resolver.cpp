// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: exercises libhv/base/dns_resolv.h, a file created by
// patches/libhv_dns_resolver.patch SPDX-License-Identifier: GPL-3.0-or-later

#include "libhv/base/dns_resolv.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

// ============================================================================
// dns_resolv_build_query() tests
// ============================================================================

TEST_CASE("dns_resolv_build_query() encodes valid hostnames", "[dns][query]") {
    uint8_t buf[512];

    SECTION("simple hostname") {
        int len = dns_resolv_build_query("example.com", buf, sizeof(buf));
        REQUIRE(len > 0);

        // Header: 12 bytes
        // Flags: RD=1 -> byte[2] = 0x01
        CHECK(buf[2] == 0x01);
        CHECK(buf[3] == 0x00);
        // Questions: 1
        CHECK(buf[4] == 0x00);
        CHECK(buf[5] == 0x01);
        // Answers/Authority/Additional: 0
        CHECK(buf[6] == 0x00);
        CHECK(buf[7] == 0x00);

        // Question: 7example3com0 + type(2) + class(2)
        // Offset 12: label length 7
        CHECK(buf[12] == 7);
        CHECK(memcmp(buf + 13, "example", 7) == 0);
        // Offset 20: label length 3
        CHECK(buf[20] == 3);
        CHECK(memcmp(buf + 21, "com", 3) == 0);
        // Offset 24: root label
        CHECK(buf[24] == 0);
        // Type A = 0x0001
        CHECK(buf[25] == 0x00);
        CHECK(buf[26] == 0x01);
        // Class IN = 0x0001
        CHECK(buf[27] == 0x00);
        CHECK(buf[28] == 0x01);
        // Total: 12 + 13 + 4 = 29
        CHECK(len == 29);
    }

    SECTION("subdomain") {
        int len = dns_resolv_build_query("api.github.com", buf, sizeof(buf));
        REQUIRE(len > 0);

        // 3api6github3com0
        CHECK(buf[12] == 3);
        CHECK(memcmp(buf + 13, "api", 3) == 0);
        CHECK(buf[16] == 6);
        CHECK(memcmp(buf + 17, "github", 6) == 0);
        CHECK(buf[23] == 3);
        CHECK(memcmp(buf + 24, "com", 3) == 0);
        CHECK(buf[27] == 0);
    }

    SECTION("single label") {
        int len = dns_resolv_build_query("localhost", buf, sizeof(buf));
        REQUIRE(len > 0);

        CHECK(buf[12] == 9);
        CHECK(memcmp(buf + 13, "localhost", 9) == 0);
        CHECK(buf[22] == 0);
    }

    SECTION("trailing dot is stripped") {
        int len = dns_resolv_build_query("example.com.", buf, sizeof(buf));
        REQUIRE(len > 0);

        // Should produce same encoding as "example.com"
        uint8_t buf2[512];
        int len2 = dns_resolv_build_query("example.com", buf2, sizeof(buf2));
        REQUIRE(len == len2);
        // Skip transaction ID (first 2 bytes may differ)
        CHECK(memcmp(buf + 2, buf2 + 2, len - 2) == 0);
    }
}

TEST_CASE("dns_resolv_build_query() rejects invalid input", "[dns][query]") {
    uint8_t buf[512];

    SECTION("null hostname") {
        CHECK(dns_resolv_build_query(nullptr, buf, sizeof(buf)) == -1);
    }

    SECTION("empty hostname") {
        CHECK(dns_resolv_build_query("", buf, sizeof(buf)) == -1);
    }

    SECTION("null buffer") {
        CHECK(dns_resolv_build_query("example.com", nullptr, sizeof(buf)) == -1);
    }

    SECTION("buffer too small") {
        CHECK(dns_resolv_build_query("example.com", buf, 10) == -1);
    }

    SECTION("label too long (>63 chars)") {
        std::string long_label(64, 'a');
        long_label += ".com";
        CHECK(dns_resolv_build_query(long_label.c_str(), buf, sizeof(buf)) == -1);
    }

    SECTION("empty label (consecutive dots)") {
        CHECK(dns_resolv_build_query("example..com", buf, sizeof(buf)) == -1);
    }

    SECTION("hostname too long (>253 chars)") {
        // Build a hostname with many short labels: a.b.c.d...
        std::string long_hostname;
        for (int i = 0; i < 128; i++) {
            if (i > 0)
                long_hostname += '.';
            long_hostname += 'a';
        }
        CHECK(long_hostname.size() > 253);
        CHECK(dns_resolv_build_query(long_hostname.c_str(), buf, sizeof(buf)) == -1);
    }
}

// ============================================================================
// dns_resolv_parse_response() tests
// ============================================================================

// Helper: build a minimal DNS response packet
static int build_test_response(uint8_t* buf, int buflen, uint16_t txid, uint8_t rcode,
                               uint16_t nanswer, const char* qname, uint32_t answer_ip) {
    if (buflen < 512)
        return -1;
    memset(buf, 0, buflen);

    // Header
    buf[0] = (uint8_t)(txid >> 8);
    buf[1] = (uint8_t)(txid & 0xFF);
    buf[2] = 0x81;         // QR=1, RD=1
    buf[3] = 0x80 | rcode; // RA=1, RCODE
    buf[4] = 0x00;
    buf[5] = 0x01; // 1 question
    buf[6] = (uint8_t)(nanswer >> 8);
    buf[7] = (uint8_t)(nanswer & 0xFF);

    int off = 12;

    // Question: encode qname
    const char* start = qname;
    const char* dot;
    while ((dot = strchr(start, '.')) != nullptr) {
        int label_len = (int)(dot - start);
        buf[off++] = (uint8_t)label_len;
        memcpy(buf + off, start, label_len);
        off += label_len;
        start = dot + 1;
    }
    int last_len = (int)strlen(start);
    if (last_len > 0) {
        buf[off++] = (uint8_t)last_len;
        memcpy(buf + off, start, last_len);
        off += last_len;
    }
    buf[off++] = 0; // Root label
    // Type A
    buf[off++] = 0x00;
    buf[off++] = 0x01;
    // Class IN
    buf[off++] = 0x00;
    buf[off++] = 0x01;

    // Answer section
    for (uint16_t i = 0; i < nanswer; i++) {
        // Name: compression pointer to offset 12 (question name)
        buf[off++] = 0xC0;
        buf[off++] = 0x0C;
        // Type A
        buf[off++] = 0x00;
        buf[off++] = 0x01;
        // Class IN
        buf[off++] = 0x00;
        buf[off++] = 0x01;
        // TTL: 60
        buf[off++] = 0x00;
        buf[off++] = 0x00;
        buf[off++] = 0x00;
        buf[off++] = 0x3C;
        // RDLENGTH: 4
        buf[off++] = 0x00;
        buf[off++] = 0x04;
        // RDATA: IPv4 address (network byte order)
        uint32_t ip = htonl(answer_ip + i);
        memcpy(buf + off, &ip, 4);
        off += 4;
    }

    return off;
}

TEST_CASE("dns_resolv_parse_response() extracts A records", "[dns][response]") {
    uint8_t pkt[512];
    struct in_addr addr;

    SECTION("single A record") {
        // 140.82.121.6 = 0x8C527906
        int pkt_len =
            build_test_response(pkt, sizeof(pkt), 0x1234, 0, 1, "api.github.com", 0x8C527906);
        REQUIRE(pkt_len > 0);

        int ret = dns_resolv_parse_response(pkt, pkt_len, &addr);
        REQUIRE(ret == 0);

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
        CHECK(std::string(ip_str) == "140.82.121.6");
    }

    SECTION("multiple A records returns first") {
        int pkt_len =
            build_test_response(pkt, sizeof(pkt), 0x5678, 0, 3, "example.com", 0x01020304);
        REQUIRE(pkt_len > 0);

        int ret = dns_resolv_parse_response(pkt, pkt_len, &addr);
        REQUIRE(ret == 0);

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
        CHECK(std::string(ip_str) == "1.2.3.4");
    }
}

TEST_CASE("dns_resolv_parse_response() rejects bad responses", "[dns][response]") {
    uint8_t pkt[512];
    struct in_addr addr;

    SECTION("null buffer") {
        CHECK(dns_resolv_parse_response(nullptr, 100, &addr) == -1);
    }

    SECTION("null addr") {
        uint8_t dummy[12] = {};
        CHECK(dns_resolv_parse_response(dummy, sizeof(dummy), nullptr) == -1);
    }

    SECTION("too short") {
        uint8_t dummy[8] = {};
        CHECK(dns_resolv_parse_response(dummy, sizeof(dummy), &addr) == -1);
    }

    SECTION("not a response (QR=0)") {
        int pkt_len =
            build_test_response(pkt, sizeof(pkt), 0x1234, 0, 1, "example.com", 0x01020304);
        // Clear QR bit
        pkt[2] &= ~0x80;
        CHECK(dns_resolv_parse_response(pkt, pkt_len, &addr) == -1);
    }

    SECTION("NXDOMAIN (rcode=3)") {
        int pkt_len =
            build_test_response(pkt, sizeof(pkt), 0x1234, 3, 0, "nonexistent.example.com", 0);
        CHECK(dns_resolv_parse_response(pkt, pkt_len, &addr) == -1);
    }

    SECTION("SERVFAIL (rcode=2)") {
        int pkt_len = build_test_response(pkt, sizeof(pkt), 0x1234, 2, 0, "example.com", 0);
        CHECK(dns_resolv_parse_response(pkt, pkt_len, &addr) == -1);
    }

    SECTION("zero answers") {
        int pkt_len = build_test_response(pkt, sizeof(pkt), 0x1234, 0, 0, "example.com", 0);
        CHECK(dns_resolv_parse_response(pkt, pkt_len, &addr) == -1);
    }

    SECTION("truncated answer record") {
        int pkt_len =
            build_test_response(pkt, sizeof(pkt), 0x1234, 0, 1, "example.com", 0x01020304);
        // Truncate in the middle of the answer
        CHECK(dns_resolv_parse_response(pkt, pkt_len - 6, &addr) == -1);
    }
}

// ============================================================================
// CNAME handling tests
// ============================================================================

TEST_CASE("dns_resolv_parse_response() handles CNAME then A record", "[dns][response][cname]") {
    // Build a response with CNAME followed by A record
    uint8_t pkt[512];
    memset(pkt, 0, sizeof(pkt));
    struct in_addr addr;

    // Header
    pkt[0] = 0x12;
    pkt[1] = 0x34; // txid
    pkt[2] = 0x81;
    pkt[3] = 0x80; // QR=1, RD=1, RA=1
    pkt[4] = 0x00;
    pkt[5] = 0x01; // 1 question
    pkt[6] = 0x00;
    pkt[7] = 0x02; // 2 answers

    int off = 12;

    // Question: api.github.com
    pkt[off++] = 3;
    memcpy(pkt + off, "api", 3);
    off += 3;
    pkt[off++] = 6;
    memcpy(pkt + off, "github", 6);
    off += 6;
    pkt[off++] = 3;
    memcpy(pkt + off, "com", 3);
    off += 3;
    pkt[off++] = 0; // root
    pkt[off++] = 0x00;
    pkt[off++] = 0x01; // type A
    pkt[off++] = 0x00;
    pkt[off++] = 0x01; // class IN

    // Answer 1: CNAME record
    pkt[off++] = 0xC0;
    pkt[off++] = 0x0C; // name pointer
    pkt[off++] = 0x00;
    pkt[off++] = 0x05; // type CNAME
    pkt[off++] = 0x00;
    pkt[off++] = 0x01; // class IN
    pkt[off++] = 0x00;
    pkt[off++] = 0x00;
    pkt[off++] = 0x00;
    pkt[off++] = 0x3C; // TTL 60
    // CNAME data: github.github.io (encoded)
    int cname_start = off + 2; // skip rdlength
    int cname_off = cname_start;
    pkt[cname_off++] = 6;
    memcpy(pkt + cname_off, "github", 6);
    cname_off += 6;
    pkt[cname_off++] = 6;
    memcpy(pkt + cname_off, "github", 6);
    cname_off += 6;
    pkt[cname_off++] = 2;
    memcpy(pkt + cname_off, "io", 2);
    cname_off += 2;
    pkt[cname_off++] = 0; // root
    int cname_len = cname_off - cname_start;
    pkt[off++] = (uint8_t)(cname_len >> 8);
    pkt[off++] = (uint8_t)(cname_len & 0xFF);
    off = cname_off;

    // Answer 2: A record
    pkt[off++] = 0xC0;
    pkt[off++] = 0x0C; // name pointer
    pkt[off++] = 0x00;
    pkt[off++] = 0x01; // type A
    pkt[off++] = 0x00;
    pkt[off++] = 0x01; // class IN
    pkt[off++] = 0x00;
    pkt[off++] = 0x00;
    pkt[off++] = 0x00;
    pkt[off++] = 0x3C; // TTL 60
    pkt[off++] = 0x00;
    pkt[off++] = 0x04; // rdlength 4
    // 10.20.30.40
    pkt[off++] = 10;
    pkt[off++] = 20;
    pkt[off++] = 30;
    pkt[off++] = 40;

    int ret = dns_resolv_parse_response(pkt, off, &addr);
    REQUIRE(ret == 0);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
    CHECK(std::string(ip_str) == "10.20.30.40");
}

// ============================================================================
// dns_resolv_get_nameservers_from() tests
// ============================================================================

TEST_CASE("dns_resolv_get_nameservers_from() parses resolv.conf", "[dns][resolv]") {
    char nameservers[DNS_RESOLV_MAX_NAMESERVERS][DNS_RESOLV_NAMESERVER_LEN];

    SECTION("typical resolv.conf") {
        const char* tmpfile = "/tmp/test_resolv.conf";
        FILE* fp = fopen(tmpfile, "w");
        REQUIRE(fp != nullptr);
        fprintf(fp, "# Generated by NetworkManager\n");
        fprintf(fp, "nameserver 192.168.1.1\n");
        fprintf(fp, "nameserver 8.8.8.8\n");
        fprintf(fp, "search local\n");
        fclose(fp);

        int count =
            dns_resolv_get_nameservers_from(tmpfile, nameservers, DNS_RESOLV_MAX_NAMESERVERS);
        CHECK(count == 2);
        CHECK(std::string(nameservers[0]) == "192.168.1.1");
        CHECK(std::string(nameservers[1]) == "8.8.8.8");

        remove(tmpfile);
    }

    SECTION("comments and blank lines") {
        const char* tmpfile = "/tmp/test_resolv2.conf";
        FILE* fp = fopen(tmpfile, "w");
        REQUIRE(fp != nullptr);
        fprintf(fp, "# comment\n");
        fprintf(fp, "; another comment\n");
        fprintf(fp, "\n");
        fprintf(fp, "  \n");
        fprintf(fp, "nameserver 10.0.0.1\n");
        fclose(fp);

        int count =
            dns_resolv_get_nameservers_from(tmpfile, nameservers, DNS_RESOLV_MAX_NAMESERVERS);
        CHECK(count == 1);
        CHECK(std::string(nameservers[0]) == "10.0.0.1");

        remove(tmpfile);
    }

    SECTION("respects max count") {
        const char* tmpfile = "/tmp/test_resolv3.conf";
        FILE* fp = fopen(tmpfile, "w");
        REQUIRE(fp != nullptr);
        fprintf(fp, "nameserver 1.1.1.1\n");
        fprintf(fp, "nameserver 2.2.2.2\n");
        fprintf(fp, "nameserver 3.3.3.3\n");
        fprintf(fp, "nameserver 4.4.4.4\n");
        fclose(fp);

        int count = dns_resolv_get_nameservers_from(tmpfile, nameservers, 2);
        CHECK(count == 2);
        CHECK(std::string(nameservers[0]) == "1.1.1.1");
        CHECK(std::string(nameservers[1]) == "2.2.2.2");

        remove(tmpfile);
    }

    SECTION("nonexistent file returns 0") {
        int count = dns_resolv_get_nameservers_from("/tmp/nonexistent_resolv.conf", nameservers,
                                                    DNS_RESOLV_MAX_NAMESERVERS);
        CHECK(count == 0);
    }

    SECTION("null path returns 0") {
        int count =
            dns_resolv_get_nameservers_from(nullptr, nameservers, DNS_RESOLV_MAX_NAMESERVERS);
        CHECK(count == 0);
    }

    SECTION("leading whitespace before nameserver") {
        const char* tmpfile = "/tmp/test_resolv4.conf";
        FILE* fp = fopen(tmpfile, "w");
        REQUIRE(fp != nullptr);
        fprintf(fp, "  nameserver 10.0.0.1\n");
        fprintf(fp, "\tnameserver 10.0.0.2\n");
        fclose(fp);

        int count =
            dns_resolv_get_nameservers_from(tmpfile, nameservers, DNS_RESOLV_MAX_NAMESERVERS);
        // Lines with leading whitespace before "nameserver" - sscanf on trimmed line
        // The current impl trims leading whitespace then does sscanf
        CHECK(count == 2);

        remove(tmpfile);
    }

    SECTION("AD5M Forge-X format: inline '# eth0' comments after each address") {
        // Real /etc/resolv.conf seen on the Flashforge AD5M (Forge-X 1.4.0).
        // Each nameserver line has a trailing "# eth0" annotation, and the
        // file leads with a 'search' directive that is also annotated. The IP
        // must be parsed WITHOUT the trailing comment (sscanf %63s stops at the
        // first whitespace), otherwise inet_pton later rejects it and the
        // static-glibc device silently fails every HTTPS lookup.
        const char* tmpfile = "/tmp/test_resolv_ad5m.conf";
        FILE* fp = fopen(tmpfile, "w");
        REQUIRE(fp != nullptr);
        fprintf(fp, "search lan # eth0\n");
        fprintf(fp, "nameserver 192.168.1.1 # eth0\n");
        fprintf(fp, "nameserver 192.168.2.1 # eth0\n");
        fprintf(fp, "nameserver 9.9.9.9 # eth0\n");
        fclose(fp);

        int count =
            dns_resolv_get_nameservers_from(tmpfile, nameservers, DNS_RESOLV_MAX_NAMESERVERS);
        CHECK(count == 3);
        // Critically: the IP only, no trailing " # eth0".
        CHECK(std::string(nameservers[0]) == "192.168.1.1");
        CHECK(std::string(nameservers[1]) == "192.168.2.1");
        CHECK(std::string(nameservers[2]) == "9.9.9.9");

        // And each parsed value must be a valid IPv4 literal (what the resolver
        // feeds to inet_pton before sending the UDP query).
        struct in_addr probe;
        CHECK(inet_pton(AF_INET, nameservers[0], &probe) == 1);

        remove(tmpfile);
    }
}
