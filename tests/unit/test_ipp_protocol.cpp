// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipp_protocol.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ipp;

// Helper to read big-endian uint16 from a buffer
static uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

// Helper to read big-endian uint32 from a buffer
static uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// ---------------------------------------------------------------------------
// IppRequest encoding
// ---------------------------------------------------------------------------

TEST_CASE("IppRequest encoding - version and operation", "[label][ipp]") {
    IppRequest req(Operation::GET_PRINTER_ATTRIBUTES, 42);
    auto buf = req.encode();

    REQUIRE(buf.size() >= 8);
    // IPP version 2.0
    REQUIRE(buf[0] == 0x02);
    REQUIRE(buf[1] == 0x00);
    // Operation code: GET_PRINTER_ATTRIBUTES = 0x000B
    REQUIRE(read_be16(buf.data() + 2) == 0x000B);
    // Request ID = 42
    REQUIRE(read_be32(buf.data() + 4) == 42);
}

TEST_CASE("IppRequest encoding - required attributes", "[label][ipp]") {
    IppRequest req(Operation::PRINT_JOB);
    auto buf = req.encode();

    // After header (8 bytes) and operation-attributes-tag (1 byte = 0x01),
    // the first attribute should be attributes-charset = "utf-8"
    REQUIRE(buf.size() > 9);
    REQUIRE(buf[8] == 0x01); // operation-attributes-tag

    // First attribute: charset (tag 0x47)
    size_t pos = 9;
    REQUIRE(buf[pos] == 0x47);
    uint16_t name_len = read_be16(buf.data() + pos + 1);
    std::string name(buf.begin() + pos + 3, buf.begin() + pos + 3 + name_len);
    REQUIRE(name == "attributes-charset");
    uint16_t val_len = read_be16(buf.data() + pos + 3 + name_len);
    std::string val(buf.begin() + pos + 3 + name_len + 2,
                    buf.begin() + pos + 3 + name_len + 2 + val_len);
    REQUIRE(val == "utf-8");

    // Second attribute: natural-language (tag 0x48)
    pos = pos + 3 + name_len + 2 + val_len;
    REQUIRE(buf[pos] == 0x48);
    name_len = read_be16(buf.data() + pos + 1);
    name = std::string(buf.begin() + pos + 3, buf.begin() + pos + 3 + name_len);
    REQUIRE(name == "attributes-natural-language");
    val_len = read_be16(buf.data() + pos + 3 + name_len);
    val = std::string(buf.begin() + pos + 3 + name_len + 2,
                      buf.begin() + pos + 3 + name_len + 2 + val_len);
    REQUIRE(val == "en");
}

TEST_CASE("IppRequest encoding - printer URI", "[label][ipp]") {
    IppRequest req(Operation::GET_PRINTER_ATTRIBUTES);
    req.set_printer_uri("ipp://192.168.1.100/ipp/print");
    auto buf = req.encode();

    // Find the URI attribute (tag 0x45) with name "printer-uri"
    // Skip header(8) + op-attrs-tag(1)
    size_t pos = 9;
    bool found = false;
    while (pos < buf.size() - 1 && buf[pos] != 0x03) {
        uint8_t tag = buf[pos];
        if (tag <= 0x0F) {
            pos++;
            continue;
        } // delimiter
        uint16_t nlen = read_be16(buf.data() + pos + 1);
        uint16_t vlen = read_be16(buf.data() + pos + 3 + nlen);
        std::string attr_name(buf.begin() + pos + 3, buf.begin() + pos + 3 + nlen);
        if (attr_name == "printer-uri") {
            REQUIRE(tag == 0x45);
            std::string uri_val(buf.begin() + pos + 3 + nlen + 2,
                                buf.begin() + pos + 3 + nlen + 2 + vlen);
            REQUIRE(uri_val == "ipp://192.168.1.100/ipp/print");
            found = true;
            break;
        }
        pos = pos + 3 + nlen + 2 + vlen;
    }
    REQUIRE(found);
}

TEST_CASE("IppRequest encoding - string attributes", "[label][ipp]") {
    IppRequest req(Operation::PRINT_JOB);
    req.add_string(ValueTag::NAME_WITHOUT_LANGUAGE, "job-name", "test-label");
    auto buf = req.encode();

    // Search for the job-name attribute
    size_t pos = 9;
    bool found = false;
    while (pos < buf.size() - 1 && buf[pos] != 0x03) {
        uint8_t tag = buf[pos];
        if (tag <= 0x0F) {
            pos++;
            continue;
        }
        uint16_t nlen = read_be16(buf.data() + pos + 1);
        uint16_t vlen = read_be16(buf.data() + pos + 3 + nlen);
        std::string attr_name(buf.begin() + pos + 3, buf.begin() + pos + 3 + nlen);
        if (attr_name == "job-name") {
            REQUIRE(tag == static_cast<uint8_t>(ValueTag::NAME_WITHOUT_LANGUAGE));
            std::string val(buf.begin() + pos + 3 + nlen + 2,
                            buf.begin() + pos + 3 + nlen + 2 + vlen);
            REQUIRE(val == "test-label");
            found = true;
            break;
        }
        pos = pos + 3 + nlen + 2 + vlen;
    }
    REQUIRE(found);
}

TEST_CASE("IppRequest encoding - integer attributes", "[label][ipp]") {
    IppRequest req(Operation::PRINT_JOB);
    req.add_integer("test-int", 0x01020304);
    auto buf = req.encode();

    // Find the test-int attribute
    size_t pos = 9;
    bool found = false;
    while (pos < buf.size() - 1 && buf[pos] != 0x03) {
        uint8_t tag = buf[pos];
        if (tag <= 0x0F) {
            pos++;
            continue;
        }
        uint16_t nlen = read_be16(buf.data() + pos + 1);
        uint16_t vlen = read_be16(buf.data() + pos + 3 + nlen);
        std::string attr_name(buf.begin() + pos + 3, buf.begin() + pos + 3 + nlen);
        if (attr_name == "test-int") {
            REQUIRE(tag == static_cast<uint8_t>(ValueTag::INTEGER));
            REQUIRE(vlen == 4);
            const uint8_t* vdata = buf.data() + pos + 3 + nlen + 2;
            REQUIRE(vdata[0] == 0x01);
            REQUIRE(vdata[1] == 0x02);
            REQUIRE(vdata[2] == 0x03);
            REQUIRE(vdata[3] == 0x04);
            found = true;
            break;
        }
        pos = pos + 3 + nlen + 2 + vlen;
    }
    REQUIRE(found);
}

TEST_CASE("IppRequest encoding - boolean attributes", "[label][ipp]") {
    IppRequest req(Operation::PRINT_JOB);

    SECTION("true value") {
        req.add_boolean("test-bool", true);
        auto buf = req.encode();

        size_t pos = 9;
        bool found = false;
        while (pos < buf.size() - 1 && buf[pos] != 0x03) {
            uint8_t tag = buf[pos];
            if (tag <= 0x0F) {
                pos++;
                continue;
            }
            uint16_t nlen = read_be16(buf.data() + pos + 1);
            uint16_t vlen = read_be16(buf.data() + pos + 3 + nlen);
            std::string attr_name(buf.begin() + pos + 3, buf.begin() + pos + 3 + nlen);
            if (attr_name == "test-bool") {
                REQUIRE(tag == static_cast<uint8_t>(ValueTag::BOOLEAN));
                REQUIRE(vlen == 1);
                REQUIRE(buf[pos + 3 + nlen + 2] == 1);
                found = true;
                break;
            }
            pos = pos + 3 + nlen + 2 + vlen;
        }
        REQUIRE(found);
    }

    SECTION("false value") {
        req.add_boolean("test-bool", false);
        auto buf = req.encode();

        size_t pos = 9;
        bool found = false;
        while (pos < buf.size() - 1 && buf[pos] != 0x03) {
            uint8_t tag = buf[pos];
            if (tag <= 0x0F) {
                pos++;
                continue;
            }
            uint16_t nlen = read_be16(buf.data() + pos + 1);
            uint16_t vlen = read_be16(buf.data() + pos + 3 + nlen);
            std::string attr_name(buf.begin() + pos + 3, buf.begin() + pos + 3 + nlen);
            if (attr_name == "test-bool") {
                REQUIRE(vlen == 1);
                REQUIRE(buf[pos + 3 + nlen + 2] == 0);
                found = true;
                break;
            }
            pos = pos + 3 + nlen + 2 + vlen;
        }
        REQUIRE(found);
    }
}

TEST_CASE("IppRequest encoding - end of attributes", "[label][ipp]") {
    IppRequest req(Operation::GET_PRINTER_ATTRIBUTES);
    auto buf = req.encode();

    // Last byte must be end-of-attributes tag (0x03)
    REQUIRE(!buf.empty());
    REQUIRE(buf.back() == 0x03);
}

TEST_CASE("IppRequest encoding - encode_with_data", "[label][ipp]") {
    IppRequest req(Operation::PRINT_JOB);
    std::vector<uint8_t> doc_data = {0xDE, 0xAD, 0xBE, 0xEF};

    auto buf = req.encode_with_data(doc_data);
    auto header_only = req.encode();

    // encode_with_data should be encode() + document data
    REQUIRE(buf.size() == header_only.size() + doc_data.size());

    // The end-of-attributes tag is at header_only.size()-1
    REQUIRE(buf[header_only.size() - 1] == 0x03);

    // Document data follows immediately
    REQUIRE(buf[header_only.size()] == 0xDE);
    REQUIRE(buf[header_only.size() + 1] == 0xAD);
    REQUIRE(buf[header_only.size() + 2] == 0xBE);
    REQUIRE(buf[header_only.size() + 3] == 0xEF);
}

TEST_CASE("IppRequest encoding - job attributes group", "[label][ipp]") {
    IppRequest req(Operation::PRINT_JOB);
    req.set_printer_uri("ipp://localhost/ipp/print");
    req.begin_job_attributes();
    req.add_integer("copies", 3);

    auto buf = req.encode();

    // Parse through the encoded IPP message to find the job-attributes delimiter.
    // Walk the structure properly: header(8) then delimiter/attribute tags.
    size_t pos = 8;
    bool found_op_tag = false;
    bool found_job_tag = false;
    size_t job_tag_pos = 0;

    while (pos < buf.size()) {
        uint8_t tag = buf[pos];

        // Delimiter tags are 0x00-0x0F
        if (tag <= 0x0F) {
            if (tag == 0x01)
                found_op_tag = true;
            if (tag == 0x02) {
                found_job_tag = true;
                job_tag_pos = pos;
                break;
            }
            if (tag == 0x03)
                break; // end of attributes
            pos++;
            continue;
        }

        // Value tag: skip over the attribute (tag + name-len + name + value-len + value)
        if (pos + 3 > buf.size())
            break;
        uint16_t nlen = read_be16(buf.data() + pos + 1);
        if (pos + 3 + nlen + 2 > buf.size())
            break;
        uint16_t vlen = read_be16(buf.data() + pos + 3 + nlen);
        pos = pos + 3 + nlen + 2 + vlen;
    }

    REQUIRE(found_op_tag);
    REQUIRE(found_job_tag);

    // After the job-attributes-tag, the copies attribute should follow
    pos = job_tag_pos + 1;
    REQUIRE(pos < buf.size());
    REQUIRE(buf[pos] == static_cast<uint8_t>(ValueTag::INTEGER));
    uint16_t nlen = read_be16(buf.data() + pos + 1);
    std::string name(buf.begin() + pos + 3, buf.begin() + pos + 3 + nlen);
    REQUIRE(name == "copies");
}

// ---------------------------------------------------------------------------
// IppResponse parsing
// ---------------------------------------------------------------------------

// Helper to build a minimal IPP response with operation attributes
static std::vector<uint8_t> build_minimal_response(StatusCode status, uint32_t req_id) {
    std::vector<uint8_t> resp;
    // Version 2.0
    resp.push_back(0x02);
    resp.push_back(0x00);
    // Status code
    resp.push_back(static_cast<uint8_t>((static_cast<uint16_t>(status) >> 8) & 0xFF));
    resp.push_back(static_cast<uint8_t>(static_cast<uint16_t>(status) & 0xFF));
    // Request ID
    resp.push_back(static_cast<uint8_t>((req_id >> 24) & 0xFF));
    resp.push_back(static_cast<uint8_t>((req_id >> 16) & 0xFF));
    resp.push_back(static_cast<uint8_t>((req_id >> 8) & 0xFF));
    resp.push_back(static_cast<uint8_t>(req_id & 0xFF));
    // Operation attributes tag
    resp.push_back(0x01);
    // charset attribute: tag(0x47) + name-length(2) + "attributes-charset" + value-length(2) +
    // "utf-8"
    resp.push_back(0x47);
    std::string charset_name = "attributes-charset";
    resp.push_back(0x00);
    resp.push_back(static_cast<uint8_t>(charset_name.size()));
    resp.insert(resp.end(), charset_name.begin(), charset_name.end());
    std::string charset_val = "utf-8";
    resp.push_back(0x00);
    resp.push_back(static_cast<uint8_t>(charset_val.size()));
    resp.insert(resp.end(), charset_val.begin(), charset_val.end());
    // End of attributes
    resp.push_back(0x03);
    return resp;
}

TEST_CASE("IppResponse parsing - success", "[label][ipp]") {
    auto raw = build_minimal_response(StatusCode::OK, 1);
    auto resp = parse_response(raw);

    REQUIRE(resp.version_major == 2);
    REQUIRE(resp.version_minor == 0);
    REQUIRE(resp.status == StatusCode::OK);
    REQUIRE(resp.request_id == 1);
    REQUIRE(resp.is_success());
    REQUIRE(!resp.operation_attributes.empty());

    // Verify the charset attribute was parsed
    REQUIRE(resp.operation_attributes[0].name == "attributes-charset");
    REQUIRE(resp.operation_attributes[0].as_string() == "utf-8");
}

TEST_CASE("IppResponse parsing - error status", "[label][ipp]") {
    auto raw = build_minimal_response(StatusCode::CLIENT_NOT_FOUND, 7);
    auto resp = parse_response(raw);

    REQUIRE(resp.version_major == 2);
    REQUIRE(resp.status == StatusCode::CLIENT_NOT_FOUND);
    REQUIRE(resp.request_id == 7);
    REQUIRE_FALSE(resp.is_success());
}

TEST_CASE("IppResponse parsing - find_attribute", "[label][ipp]") {
    // Build a response with attributes in different groups
    std::vector<uint8_t> raw;
    // Header
    raw.insert(raw.end(), {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01});
    // Operation attributes
    raw.push_back(0x01);
    // charset
    raw.push_back(0x47);
    std::string n1 = "attributes-charset";
    raw.push_back(0x00);
    raw.push_back(static_cast<uint8_t>(n1.size()));
    raw.insert(raw.end(), n1.begin(), n1.end());
    std::string v1 = "utf-8";
    raw.push_back(0x00);
    raw.push_back(static_cast<uint8_t>(v1.size()));
    raw.insert(raw.end(), v1.begin(), v1.end());
    // Printer attributes
    raw.push_back(0x04);
    // printer-state (integer, tag 0x21)
    raw.push_back(0x21);
    std::string n2 = "printer-state";
    raw.push_back(0x00);
    raw.push_back(static_cast<uint8_t>(n2.size()));
    raw.insert(raw.end(), n2.begin(), n2.end());
    raw.push_back(0x00);
    raw.push_back(0x04);                             // value-length = 4
    raw.insert(raw.end(), {0x00, 0x00, 0x00, 0x03}); // idle = 3
    // End
    raw.push_back(0x03);

    auto resp = parse_response(raw);

    // find_attribute searches across all groups
    auto* charset = resp.find_attribute("attributes-charset");
    REQUIRE(charset != nullptr);
    REQUIRE(charset->as_string() == "utf-8");

    auto* pstate = resp.find_attribute("printer-state");
    REQUIRE(pstate != nullptr);
    REQUIRE(pstate->as_integer() == 3);

    // Non-existent attribute
    REQUIRE(resp.find_attribute("nonexistent") == nullptr);
}

TEST_CASE("IppResponse parsing - multi-valued attributes", "[label][ipp]") {
    std::vector<uint8_t> raw;
    // Header
    raw.insert(raw.end(), {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01});
    // Operation attributes
    raw.push_back(0x01);

    // First value: keyword tag, name="document-format-supported", value="application/pdf"
    raw.push_back(0x44);
    std::string name = "document-format-supported";
    raw.push_back(0x00);
    raw.push_back(static_cast<uint8_t>(name.size()));
    raw.insert(raw.end(), name.begin(), name.end());
    std::string val1 = "application/pdf";
    raw.push_back(0x00);
    raw.push_back(static_cast<uint8_t>(val1.size()));
    raw.insert(raw.end(), val1.begin(), val1.end());

    // Second value: same attribute, name-length=0 (multi-valued)
    raw.push_back(0x44);
    raw.push_back(0x00);
    raw.push_back(0x00); // name-length = 0
    std::string val2 = "image/pwg-raster";
    raw.push_back(0x00);
    raw.push_back(static_cast<uint8_t>(val2.size()));
    raw.insert(raw.end(), val2.begin(), val2.end());

    // End
    raw.push_back(0x03);

    auto resp = parse_response(raw);
    auto all = resp.find_all("document-format-supported");
    REQUIRE(all.size() == 2);
    REQUIRE(all[0]->as_string() == "application/pdf");
    REQUIRE(all[1]->as_string() == "image/pwg-raster");
}

// ---------------------------------------------------------------------------
// Convenience builders
// ---------------------------------------------------------------------------

TEST_CASE("build_get_printer_attributes - basic", "[label][ipp]") {
    auto req = build_get_printer_attributes("ipp://printer.local/ipp/print",
                                            {"printer-state", "printer-state-reasons"});
    auto buf = req.encode();

    // Verify operation code = GET_PRINTER_ATTRIBUTES (0x000B)
    REQUIRE(read_be16(buf.data() + 2) == 0x000B);

    // Parse our own encoding to verify attributes are present
    auto resp_data = buf;
    // The encoded request has the same binary format as a response (minus document)
    // but we can just search for the keyword values in the buffer
    std::string kw1 = "printer-state";
    std::string kw2 = "printer-state-reasons";
    std::string buf_str(buf.begin(), buf.end());
    REQUIRE(buf_str.find(kw1) != std::string::npos);
    REQUIRE(buf_str.find(kw2) != std::string::npos);
    REQUIRE(buf_str.find("requested-attributes") != std::string::npos);
}

TEST_CASE("build_print_job - basic", "[label][ipp]") {
    auto req = build_print_job("ipp://printer.local/ipp/print", "my-label", "image/pwg-raster",
                               "na_letter_8.5x11in");
    auto buf = req.encode();

    // Verify operation code = PRINT_JOB (0x0002)
    REQUIRE(read_be16(buf.data() + 2) == 0x0002);

    // Verify document-format and media are in the encoded data
    std::string buf_str(buf.begin(), buf.end());
    REQUIRE(buf_str.find("document-format") != std::string::npos);
    REQUIRE(buf_str.find("image/pwg-raster") != std::string::npos);
    REQUIRE(buf_str.find("na_letter_8.5x11in") != std::string::npos);
}

// ---------------------------------------------------------------------------
// make_printer_uri
// ---------------------------------------------------------------------------

TEST_CASE("make_printer_uri - basic", "[label][ipp]") {
    auto uri = make_printer_uri("192.168.1.100", 9100, "ipp/print");
    REQUIRE(uri == "ipp://192.168.1.100:9100/ipp/print");
}

TEST_CASE("make_printer_uri - IPv6", "[label][ipp]") {
    auto uri = make_printer_uri("fe80::1", 631, "ipp/print");
    // IPv6 should be wrapped in brackets; port 631 is default, omitted
    REQUIRE(uri == "ipp://[fe80::1]/ipp/print");
}

TEST_CASE("make_printer_uri - default port", "[label][ipp]") {
    auto uri = make_printer_uri("printer.local", 631, "ipp/print");
    // Port 631 is default, should be omitted
    REQUIRE(uri == "ipp://printer.local/ipp/print");
    REQUIRE(uri.find(":631") == std::string::npos);
}

TEST_CASE("make_printer_uri - CUPS queue path and empty path", "[label][ipp][ipp_rp]") {
    REQUIRE(make_printer_uri("192.168.2.3", 631, "/printers/Canon_TS8350") ==
            "ipp://192.168.2.3/printers/Canon_TS8350");
    REQUIRE(make_printer_uri("192.168.2.3", 631, "") == "ipp://192.168.2.3/ipp/print");
}

TEST_CASE("make_printer_uri - TLS uses the ipps scheme", "[label][ipp][2034]") {
    REQUIRE(make_printer_uri("192.168.2.3", 8631, "ipp/print", Transport::Tls) ==
            "ipps://192.168.2.3:8631/ipp/print");
}

// ---------------------------------------------------------------------------
// normalize_resource_path
// ---------------------------------------------------------------------------

TEST_CASE("normalize_resource_path strips leading slashes and falls back to ipp/print",
          "[label][ipp][ipp_rp]") {
    CHECK(normalize_resource_path("ipp/print") == "ipp/print");
    CHECK(normalize_resource_path("printers/Canon_TS8350") == "printers/Canon_TS8350");
    CHECK(normalize_resource_path("/printers/Canon_TS8350") == "printers/Canon_TS8350");
    CHECK(normalize_resource_path("//ipp/print") == "ipp/print");
    CHECK(normalize_resource_path("") == "ipp/print");
    CHECK(normalize_resource_path("/") == "ipp/print");
}

// ---------------------------------------------------------------------------
// StatusCode is_success
// ---------------------------------------------------------------------------

TEST_CASE("StatusCode is_success", "[label][ipp]") {
    IppResponse ok_resp;
    ok_resp.status = StatusCode::OK;
    REQUIRE(ok_resp.is_success());

    IppResponse ok_ignored;
    ok_ignored.status = StatusCode::OK_IGNORED_ATTRIBUTES;
    REQUIRE(ok_ignored.is_success());

    IppResponse ok_conflict;
    ok_conflict.status = StatusCode::OK_CONFLICTING;
    REQUIRE(ok_conflict.is_success());

    IppResponse client_err;
    client_err.status = StatusCode::CLIENT_BAD_REQUEST;
    REQUIRE_FALSE(client_err.is_success());

    IppResponse server_err;
    server_err.status = StatusCode::SERVER_INTERNAL_ERROR;
    REQUIRE_FALSE(server_err.is_success());
}
