// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace helix::ipp {

/// IPP operation codes
enum class Operation : uint16_t {
    PRINT_JOB = 0x0002,
    VALIDATE_JOB = 0x0004,
    CREATE_JOB = 0x0005,
    SEND_DOCUMENT = 0x0006,
    CANCEL_JOB = 0x0008,
    GET_JOB_ATTRIBUTES = 0x0009,
    GET_PRINTER_ATTRIBUTES = 0x000B,
};

/// IPP status codes (high byte = class)
enum class StatusCode : uint16_t {
    OK = 0x0000,
    OK_IGNORED_ATTRIBUTES = 0x0001,
    OK_CONFLICTING = 0x0002,
    CLIENT_BAD_REQUEST = 0x0400,
    CLIENT_FORBIDDEN = 0x0401,
    CLIENT_NOT_AUTHENTICATED = 0x0402,
    CLIENT_NOT_AUTHORIZED = 0x0403,
    CLIENT_NOT_POSSIBLE = 0x0404,
    CLIENT_TIMEOUT = 0x0405,
    CLIENT_NOT_FOUND = 0x0406,
    CLIENT_GONE = 0x0407,
    CLIENT_DOCUMENT_FORMAT = 0x040A,
    SERVER_INTERNAL_ERROR = 0x0500,
    SERVER_NOT_ACCEPTING = 0x0502,
    SERVER_BUSY = 0x0503,
};

/// IPP value type tags
enum class ValueTag : uint8_t {
    // Delimiter tags
    OPERATION_ATTRIBUTES = 0x01,
    JOB_ATTRIBUTES = 0x02,
    END_OF_ATTRIBUTES = 0x03,
    PRINTER_ATTRIBUTES = 0x04,

    // Value tags
    INTEGER = 0x21,
    BOOLEAN = 0x22,
    ENUM = 0x23,
    OCTET_STRING = 0x30,
    DATE_TIME = 0x31,
    RESOLUTION = 0x32,
    RANGE_OF_INTEGER = 0x33,
    TEXT_WITHOUT_LANGUAGE = 0x41,
    NAME_WITHOUT_LANGUAGE = 0x42,
    KEYWORD = 0x44,
    URI = 0x45,
    URI_SCHEME = 0x46,
    CHARSET = 0x47,
    NATURAL_LANGUAGE = 0x48,
    MIME_MEDIA_TYPE = 0x49,
};

/// An IPP attribute value (simplified -- stores raw bytes + tag)
struct IppValue {
    ValueTag tag;
    std::string name;
    std::vector<uint8_t> data;

    /// Interpret data as a UTF-8 string
    std::string as_string() const;

    /// Interpret data as a big-endian int32_t
    int32_t as_integer() const;

    /// Interpret data as a boolean (first byte != 0)
    bool as_boolean() const;
};

/// Parsed IPP response
struct IppResponse {
    uint8_t version_major = 0;
    uint8_t version_minor = 0;
    StatusCode status = StatusCode::OK;
    uint32_t request_id = 0;

    /// Attributes grouped by delimiter tag
    std::vector<IppValue> operation_attributes;
    std::vector<IppValue> job_attributes;
    std::vector<IppValue> printer_attributes;

    /// Is the status a success code?
    bool is_success() const {
        return static_cast<uint16_t>(status) < 0x0400;
    }

    /// Find first attribute by name across all groups
    const IppValue* find_attribute(const std::string& name) const;

    /// Find all values of an attribute (for multi-valued attributes)
    std::vector<const IppValue*> find_all(const std::string& name) const;

    /// Get human-readable status message
    std::string status_message() const;
};

/// Builder for IPP request messages
class IppRequest {
  public:
    explicit IppRequest(Operation op, uint32_t request_id = 1);

    /// Add standard required operation attributes (charset, language, printer-uri)
    void set_printer_uri(const std::string& uri);

    /// Add requesting-user-name
    void set_user_name(const std::string& name);

    /// Add job-name
    void set_job_name(const std::string& name);

    /// Add document-format
    void set_document_format(const std::string& mime_type);

    /// Add copies
    void set_copies(int copies);

    /// Add media size (e.g., "na_letter_8.5x11in", "iso_a4_210x297mm")
    void set_media(const std::string& media);

    /// Add a custom string attribute to the current group
    void add_string(ValueTag tag, const std::string& name, const std::string& value);

    /// Add a custom integer attribute to the current group
    void add_integer(const std::string& name, int32_t value);

    /// Add a custom boolean attribute to the current group
    void add_boolean(const std::string& name, bool value);

    /// Add a custom enum attribute to the current group
    void add_enum(const std::string& name, int32_t value);

    /// Switch to job attributes group
    void begin_job_attributes();

    /// Encode the IPP request to binary (without document data).
    /// The caller appends document data after this for Print-Job.
    std::vector<uint8_t> encode() const;

    /// Encode the IPP request + append document data (convenience for Print-Job)
    std::vector<uint8_t> encode_with_data(const std::vector<uint8_t>& document) const;

  private:
    Operation operation_;
    uint32_t request_id_;

    struct Attribute {
        ValueTag tag;
        std::string name;
        std::vector<uint8_t> data;
    };

    std::vector<Attribute> operation_attrs_;
    std::vector<Attribute> job_attrs_;
    bool in_job_group_ = false;

    void add_raw_attribute(ValueTag tag, const std::string& name, const void* data, size_t len);
    static void encode_attribute(std::vector<uint8_t>& buf, const Attribute& attr);
};

/// Parse an IPP response from raw bytes
IppResponse parse_response(const uint8_t* data, size_t len);
IppResponse parse_response(const std::vector<uint8_t>& data);

/// Build a Get-Printer-Attributes request.
/// requested_attributes: if empty, requests all; otherwise specific attrs.
IppRequest build_get_printer_attributes(const std::string& printer_uri,
                                        const std::vector<std::string>& requested_attributes = {});

/// Build a Print-Job request (document data appended separately)
IppRequest build_print_job(const std::string& printer_uri, const std::string& job_name,
                           const std::string& document_format, const std::string& media = "");

/// Resource path of a printer that advertises none (IPP Everywhere's default).
inline constexpr const char* kDefaultResourcePath = "ipp/print";

/// The resource path without leading slashes, or kDefaultResourcePath when that
/// leaves nothing.
std::string normalize_resource_path(const std::string& resource_path);

/// How a job reaches the printer: plain IPP over HTTP, or IPPS (IPP over TLS, RFC 7472),
/// which shares IPP's default port 631.
enum class Transport { Plain, Tls };

/// Construct an IPP printer URI from discovered mDNS info: ipp:// for Plain, ipps:// for Tls.
/// host: hostname or IP, port: IPP port, resource_path: from mDNS TXT "rp" field.
std::string make_printer_uri(const std::string& host, uint16_t port,
                             const std::string& resource_path = kDefaultResourcePath,
                             Transport transport = Transport::Plain);

} // namespace helix::ipp
