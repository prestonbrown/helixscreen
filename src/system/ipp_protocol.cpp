// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_LABEL_PRINTER

#include "ipp_protocol.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace helix::ipp {

// ---------------------------------------------------------------------------
// Byte-order helpers (big-endian / network byte order)
// ---------------------------------------------------------------------------

static void put_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

static void put_u16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void put_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void put_bytes(std::vector<uint8_t>& buf, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + len);
}

static uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static uint32_t read_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

static int32_t read_i32(const uint8_t* p) {
    uint32_t u = read_u32(p);
    int32_t result;
    std::memcpy(&result, &u, sizeof(result));
    return result;
}

// ---------------------------------------------------------------------------
// IppValue accessors
// ---------------------------------------------------------------------------

std::string IppValue::as_string() const {
    return std::string(data.begin(), data.end());
}

int32_t IppValue::as_integer() const {
    if (data.size() < 4) {
        spdlog::warn("ipp: as_integer() on value with {} bytes (expected 4)", data.size());
        return 0;
    }
    return read_i32(data.data());
}

bool IppValue::as_boolean() const {
    if (data.empty()) {
        spdlog::warn("ipp: as_boolean() on empty value");
        return false;
    }
    return data[0] != 0;
}

// ---------------------------------------------------------------------------
// IppResponse
// ---------------------------------------------------------------------------

const IppValue* IppResponse::find_attribute(const std::string& name) const {
    for (const auto& attr : operation_attributes) {
        if (attr.name == name)
            return &attr;
    }
    for (const auto& attr : job_attributes) {
        if (attr.name == name)
            return &attr;
    }
    for (const auto& attr : printer_attributes) {
        if (attr.name == name)
            return &attr;
    }
    return nullptr;
}

std::vector<const IppValue*> IppResponse::find_all(const std::string& name) const {
    std::vector<const IppValue*> results;
    auto search = [&](const std::vector<IppValue>& attrs) {
        for (const auto& attr : attrs) {
            if (attr.name == name)
                results.push_back(&attr);
        }
    };
    search(operation_attributes);
    search(job_attributes);
    search(printer_attributes);
    return results;
}

std::string IppResponse::status_message() const {
    switch (status) {
    case StatusCode::OK:
        return "successful-ok";
    case StatusCode::OK_IGNORED_ATTRIBUTES:
        return "successful-ok-ignored-or-substituted-attributes";
    case StatusCode::OK_CONFLICTING:
        return "successful-ok-conflicting-attributes";
    case StatusCode::CLIENT_BAD_REQUEST:
        return "client-error-bad-request";
    case StatusCode::CLIENT_FORBIDDEN:
        return "client-error-forbidden";
    case StatusCode::CLIENT_NOT_AUTHENTICATED:
        return "client-error-not-authenticated";
    case StatusCode::CLIENT_NOT_AUTHORIZED:
        return "client-error-not-authorized";
    case StatusCode::CLIENT_NOT_POSSIBLE:
        return "client-error-not-possible";
    case StatusCode::CLIENT_TIMEOUT:
        return "client-error-timeout";
    case StatusCode::CLIENT_NOT_FOUND:
        return "client-error-not-found";
    case StatusCode::CLIENT_GONE:
        return "client-error-gone";
    case StatusCode::CLIENT_DOCUMENT_FORMAT:
        return "client-error-document-format-not-supported";
    case StatusCode::SERVER_INTERNAL_ERROR:
        return "server-error-internal-error";
    case StatusCode::SERVER_NOT_ACCEPTING:
        return "server-error-not-accepting-jobs";
    case StatusCode::SERVER_BUSY:
        return "server-error-busy";
    default: {
        uint16_t code = static_cast<uint16_t>(status);
        if (code < 0x0400)
            return "successful-ok (0x" + fmt::format("{:04x}", code) + ")";
        if (code < 0x0500)
            return "client-error (0x" + fmt::format("{:04x}", code) + ")";
        return "server-error (0x" + fmt::format("{:04x}", code) + ")";
    }
    }
}

// ---------------------------------------------------------------------------
// IppRequest
// ---------------------------------------------------------------------------

IppRequest::IppRequest(Operation op, uint32_t request_id)
    : operation_(op), request_id_(request_id) {
    // Every IPP request must begin with charset and language
    add_string(ValueTag::CHARSET, "attributes-charset", "utf-8");
    add_string(ValueTag::NATURAL_LANGUAGE, "attributes-natural-language", "en");
}

void IppRequest::set_printer_uri(const std::string& uri) {
    add_string(ValueTag::URI, "printer-uri", uri);
}

void IppRequest::set_user_name(const std::string& name) {
    add_string(ValueTag::NAME_WITHOUT_LANGUAGE, "requesting-user-name", name);
}

void IppRequest::set_job_name(const std::string& name) {
    add_string(ValueTag::NAME_WITHOUT_LANGUAGE, "job-name", name);
}

void IppRequest::set_document_format(const std::string& mime_type) {
    add_string(ValueTag::MIME_MEDIA_TYPE, "document-format", mime_type);
}

void IppRequest::set_copies(int copies) {
    begin_job_attributes();
    add_integer("copies", copies);
}

void IppRequest::set_media(const std::string& media) {
    begin_job_attributes();
    add_string(ValueTag::KEYWORD, "media", media);
}

void IppRequest::add_string(ValueTag tag, const std::string& name, const std::string& value) {
    add_raw_attribute(tag, name, value.data(), value.size());
}

void IppRequest::add_integer(const std::string& name, int32_t value) {
    uint8_t buf[4];
    buf[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    buf[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buf[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(value & 0xFF);
    add_raw_attribute(ValueTag::INTEGER, name, buf, 4);
}

void IppRequest::add_boolean(const std::string& name, bool value) {
    uint8_t v = value ? 1 : 0;
    add_raw_attribute(ValueTag::BOOLEAN, name, &v, 1);
}

void IppRequest::add_enum(const std::string& name, int32_t value) {
    uint8_t buf[4];
    buf[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    buf[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buf[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(value & 0xFF);
    add_raw_attribute(ValueTag::ENUM, name, buf, 4);
}

void IppRequest::begin_job_attributes() {
    in_job_group_ = true;
}

void IppRequest::add_raw_attribute(ValueTag tag, const std::string& name, const void* data,
                                   size_t len) {
    Attribute attr;
    attr.tag = tag;
    attr.name = name;
    attr.data.resize(len);
    if (len > 0) {
        std::memcpy(attr.data.data(), data, len);
    }

    if (in_job_group_) {
        job_attrs_.push_back(std::move(attr));
    } else {
        operation_attrs_.push_back(std::move(attr));
    }
}

void IppRequest::encode_attribute(std::vector<uint8_t>& buf, const Attribute& attr) {
    // value-tag: 1 byte
    put_u8(buf, static_cast<uint8_t>(attr.tag));
    // name-length: 2 bytes BE
    put_u16(buf, static_cast<uint16_t>(attr.name.size()));
    // name
    if (!attr.name.empty()) {
        put_bytes(buf, attr.name.data(), attr.name.size());
    }
    // value-length: 2 bytes BE
    put_u16(buf, static_cast<uint16_t>(attr.data.size()));
    // value
    if (!attr.data.empty()) {
        put_bytes(buf, attr.data.data(), attr.data.size());
    }
}

std::vector<uint8_t> IppRequest::encode() const {
    std::vector<uint8_t> buf;
    // Estimate: header(8) + attrs ~64 bytes each + end tag
    buf.reserve(8 + (operation_attrs_.size() + job_attrs_.size()) * 64 + 1);

    // Version: IPP 2.0
    put_u8(buf, 0x02);
    put_u8(buf, 0x00);

    // Operation code (big-endian)
    put_u16(buf, static_cast<uint16_t>(operation_));

    // Request ID (big-endian)
    put_u32(buf, request_id_);

    // Operation attributes group
    if (!operation_attrs_.empty()) {
        put_u8(buf, static_cast<uint8_t>(ValueTag::OPERATION_ATTRIBUTES));
        for (const auto& attr : operation_attrs_) {
            encode_attribute(buf, attr);
        }
    }

    // Job attributes group
    if (!job_attrs_.empty()) {
        put_u8(buf, static_cast<uint8_t>(ValueTag::JOB_ATTRIBUTES));
        for (const auto& attr : job_attrs_) {
            encode_attribute(buf, attr);
        }
    }

    // End of attributes
    put_u8(buf, static_cast<uint8_t>(ValueTag::END_OF_ATTRIBUTES));

    return buf;
}

std::vector<uint8_t> IppRequest::encode_with_data(const std::vector<uint8_t>& document) const {
    auto buf = encode();
    buf.insert(buf.end(), document.begin(), document.end());
    return buf;
}

// ---------------------------------------------------------------------------
// Response parser
// ---------------------------------------------------------------------------

IppResponse parse_response(const uint8_t* data, size_t len) {
    IppResponse resp;

    if (len < 8) {
        spdlog::error("ipp: response too short ({} bytes, need at least 8)", len);
        resp.status = StatusCode::CLIENT_BAD_REQUEST;
        return resp;
    }

    resp.version_major = data[0];
    resp.version_minor = data[1];
    resp.status = static_cast<StatusCode>(read_u16(data + 2));
    resp.request_id = read_u32(data + 4);

    spdlog::debug("ipp: response v{}.{} status=0x{:04x} request_id={}", resp.version_major,
                  resp.version_minor, static_cast<uint16_t>(resp.status), resp.request_id);

    size_t pos = 8;

    // Track which attribute group we're currently in
    std::vector<IppValue>* current_group = nullptr;
    std::string last_attr_name;

    while (pos < len) {
        uint8_t tag = data[pos];

        // Delimiter tags (0x00-0x0F)
        if (tag <= 0x0F) {
            if (tag == static_cast<uint8_t>(ValueTag::END_OF_ATTRIBUTES)) {
                break;
            }
            if (tag == static_cast<uint8_t>(ValueTag::OPERATION_ATTRIBUTES)) {
                current_group = &resp.operation_attributes;
            } else if (tag == static_cast<uint8_t>(ValueTag::JOB_ATTRIBUTES)) {
                current_group = &resp.job_attributes;
            } else if (tag == static_cast<uint8_t>(ValueTag::PRINTER_ATTRIBUTES)) {
                current_group = &resp.printer_attributes;
            } else {
                spdlog::debug("ipp: unknown delimiter tag 0x{:02x}, treating as generic group",
                              tag);
                // Unknown group -- put into printer_attributes as fallback
                current_group = &resp.printer_attributes;
            }
            last_attr_name.clear();
            pos++;
            continue;
        }

        // Value tag -- parse attribute
        // Need at least: tag(1) + name-length(2) + value-length(2) = 5 bytes
        if (pos + 4 >= len) {
            spdlog::warn("ipp: truncated attribute at offset {}", pos);
            break;
        }

        uint16_t name_len = read_u16(data + pos + 1);
        size_t name_start = pos + 3;

        if (name_start + name_len + 2 > len) {
            spdlog::warn("ipp: truncated attribute name at offset {}", pos);
            break;
        }

        std::string attr_name;
        if (name_len > 0) {
            attr_name.assign(reinterpret_cast<const char*>(data + name_start), name_len);
            last_attr_name = attr_name;
        } else {
            // Multi-valued: name-length=0 means "additional value for previous attribute"
            attr_name = last_attr_name;
        }

        uint16_t value_len = read_u16(data + name_start + name_len);
        size_t value_start = name_start + name_len + 2;

        if (value_start + value_len > len) {
            spdlog::warn("ipp: truncated attribute value at offset {}", pos);
            break;
        }

        IppValue val;
        val.tag = static_cast<ValueTag>(tag);
        val.name = attr_name;
        if (value_len > 0) {
            val.data.assign(data + value_start, data + value_start + value_len);
        }

        if (current_group) {
            current_group->push_back(std::move(val));
        } else {
            // Attribute before any group delimiter -- shouldn't happen but handle gracefully
            spdlog::debug("ipp: attribute '{}' before any group delimiter", attr_name);
            resp.operation_attributes.push_back(std::move(val));
        }

        pos = value_start + value_len;
    }

    return resp;
}

IppResponse parse_response(const std::vector<uint8_t>& data) {
    return parse_response(data.data(), data.size());
}

// ---------------------------------------------------------------------------
// Convenience builders
// ---------------------------------------------------------------------------

IppRequest build_get_printer_attributes(const std::string& printer_uri,
                                        const std::vector<std::string>& requested_attributes) {
    IppRequest req(Operation::GET_PRINTER_ATTRIBUTES);
    req.set_printer_uri(printer_uri);
    req.set_user_name("helixscreen");

    if (!requested_attributes.empty()) {
        // First value has the attribute name
        req.add_string(ValueTag::KEYWORD, "requested-attributes", requested_attributes[0]);
        // Subsequent values use empty name (multi-valued encoding)
        for (size_t i = 1; i < requested_attributes.size(); i++) {
            req.add_string(ValueTag::KEYWORD, "", requested_attributes[i]);
        }
    }

    return req;
}

IppRequest build_print_job(const std::string& printer_uri, const std::string& job_name,
                           const std::string& document_format, const std::string& media) {
    IppRequest req(Operation::PRINT_JOB);
    req.set_printer_uri(printer_uri);
    req.set_user_name("helixscreen");
    req.set_job_name(job_name);
    req.set_document_format(document_format);

    if (!media.empty()) {
        req.set_media(media);
    }

    return req;
}

std::string normalize_resource_path(const std::string& resource_path) {
    const auto start = resource_path.find_first_not_of('/');
    if (start == std::string::npos) {
        return kDefaultResourcePath;
    }
    return resource_path.substr(start);
}

std::string make_printer_uri(const std::string& host, uint16_t port,
                             const std::string& resource_path, Transport transport) {
    std::string uri = transport == Transport::Tls ? "ipps://" : "ipp://";

    // Wrap IPv6 addresses in brackets
    if (!host.empty() && host.find(':') != std::string::npos && host.front() != '[') {
        uri += "[" + host + "]";
    } else {
        uri += host;
    }

    // Only include port if non-default
    if (port != 631) {
        uri += ":" + std::to_string(port);
    }

    uri += "/" + normalize_resource_path(resource_path);

    return uri;
}

} // namespace helix::ipp

#endif // HELIX_HAS_LABEL_PRINTER
