# HTTP Content, JSON & Requests

## http_content.h — Content Parsing & Serialization

Functions for parsing HTTP request/response bodies based on Content-Type:

| Function | Signature | Description |
|----------|-----------|-------------|
| `parse_query_params` | `int parse_query_params(const std::string& str, hv::QueryParams& kv)` | Parse URL query string `?key=val&...` |
| `parse_json` | `int parse_json(const std::string& str, hv::Json& json)` | Parse JSON body using nlohmann/json |
| `parse_multipart` | `int parse_multipart(const std::string& str, hv::MultiPart& mp)` | Parse multipart/form-data body |
| `dump_query_params` | `std::string dump_query_params(const hv::QueryParams& kv)` | Serialize key-value pairs to query string |
| `dump_json` | `std::string dump_json(const hv::Json& json)` | Serialize JSON to string |
| `dump_multipart` | `std::string dump_multipart(const hv::MultiPart& mp)` | Serialize multipart data to string |

---

## Content Type Mapping

| Content-Type | Parsed Into | Type Alias |
|-------------|-------------|------------|
| `application/json` | `hv::Json` | `nlohmann::json` |
| `multipart/form-data` | `hv::MultiPart` | `std::vector<FormData>` |
| `application/x-www-form-urlencoded` | `hv::KeyValue` | `std::map<std::string, std::string>` |

---

## HttpMessage — Structured Content Access

`HttpRequest` and `HttpResponse` both extend `HttpMessage`, which provides:

### Direct Members

| Member | Type | Description |
|--------|------|-------------|
| `json` | `hv::Json` | Parsed JSON body (use after `ParseBody()`) |
| `form` | `hv::MultiPart` | Parsed multipart form data |
| `kv` | `hv::KeyValue` | Parsed URL-encoded form data |

### Template Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Get<T>` | `T Get(const std::string& key, T defvalue = T())` | Get typed value from parsed content |
| `Set<T>` | `void Set(const std::string& key, const T& value)` | Set typed value |

### Convenience Getters

| Method | Return Type | Description |
|--------|-------------|-------------|
| `GetString(key, defval)` | `std::string` | Get string value |
| `GetBool(key, defval)` | `bool` | Get boolean |
| `GetInt(key, defval)` | `int` | Get integer |
| `GetFloat(key, defval)` | `float` | Get float |

### Content Access Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetJson()` | `hv::Json&` | Get parsed JSON reference |
| `GetForm()` | `hv::MultiPart&` | Get parsed multipart form |
| `GetFormData(name)` | `FormData*` | Get specific form field by name |
| `GetUrlEncoded(key)` | `std::string` | Get URL-encoded field value |

### Content Setting Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetFormData(name, value)` | — | Set form field |
| `SetFormFile(name, filepath)` | — | Attach file to multipart form |
| `SetUrlEncoded(key, value)` | — | Set URL-encoded field |

### Body Parsing / Serialization

| Method | Signature | Description |
|--------|-----------|-------------|
| `ParseBody()` | `int ParseBody()` | Auto-detect Content-Type and parse body into structured members |
| `DumpBody()` | `int DumpBody()` | Serialize structured content back to body string |
| `Json(t)` | `void Json(const T& t)` | Serialize `t` as JSON response (sets Content-Type, calls `dump_json`) |

---

## HttpRequest

### Core Members

| Member | Type | Description |
|--------|------|-------------|
| `method` | `http_method` (enum) | HTTP method (GET, POST, PUT, DELETE, etc.) |
| `url` | `std::string` | Full URL |
| `scheme` | `std::string` | `http` or `https` |
| `host` | `std::string` | Target hostname |
| `port` | `int` | Target port |
| `path` | `std::string` | URL path |
| `query_params` | `hv::QueryParams` | Parsed query parameters |

### Key Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetParam(key)` | `std::string` | Get query parameter value |
| `SetParam(key, value)` | `void` | Set query parameter |
| `Path()` | `std::string` | Get URL path |
| `FullPath()` | `std::string` | Get path + query string |
| `SetTimeout(sec)` | `void` | Set total request timeout in seconds |
| `SetConnectTimeout(sec)` | `void` | Set connection timeout in seconds |
| `SetBasicAuth(user, pass)` | `void` | Set Basic authentication header |
| `SetBearerTokenAuth(token)` | `void` | Set Bearer token authorization header |
| `IsHttps()` | `bool` | Check if scheme is HTTPS |
| `AllowRedirect(bool)` | `void` | Enable/disable following redirects |
| `SetRange(from, to)` | `void` | Set Range header for partial content |
| `GetRange(from, to)` | `bool` | Parse Range header values |

---

## HttpResponse

### Core Members

| Member | Type | Description |
|--------|------|-------------|
| `status_code` | `http_status` (enum) | HTTP status code (200, 404, etc.) |
| `status_message()` | `const char*` | Status text ("OK", "Not Found", etc.) |

### Response Builder Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `String(str)` | `HttpResponse&` | Set text response body (returns 200) |
| `Data(data, len, nocopy)` | `HttpResponse&` | Set binary response body |
| `Json(t)` | `HttpResponse&` | Serialize as JSON response (sets Content-Type) |
| `File(filepath)` | `HttpResponse&` | Serve a file (auto-detects Content-Type) |
| `Redirect(location, status)` | `HttpResponse&` | Send redirect response (default 302) |
| `SetRange(from, to, total)` | `void` | Set Content-Range header |
| `GetRange(from, to, total)` | `bool` | Parse Range header values |

---

## http_headers

```cpp
using http_headers = std::map<std::string, std::string, hv::StringCaseLess>;
```

- Case-insensitive key comparison via `StringCaseLess`
- Use like a normal `std::map`

### Constants

| Constant | Description |
|----------|-------------|
| `DefaultHeaders` | Default HTTP headers map (Host, User-Agent, etc.) |
| `NoBody` | Sentinel for responses with no body |

---

## requests.h — Python-style HTTP Client

Synchronous and async HTTP client functions:

### Synchronous

| Function | Signature | Description |
|----------|-----------|-------------|
| `requests::get` | `HttpResponsePtr get(const std::string& url, const http_headers& headers = DefaultHeaders)` | HTTP GET |
| `requests::post` | `HttpResponsePtr post(const std::string& url, const std::string& body, const http_headers& headers = DefaultHeaders)` | HTTP POST |
| `requests::put` | `HttpResponsePtr put(url, body, headers)` | HTTP PUT |
| `requests::del` | `HttpResponsePtr del(url, headers)` | HTTP DELETE |
| `requests::request` | `HttpResponsePtr request(HttpRequestPtr req)` | Send custom request. Returns **NULL** on failure (connection error, timeout). |

### Async

| Function | Signature | Description |
|----------|-----------|-------------|
| `requests::async` | `void async(HttpRequestPtr req, std::function<void(HttpResponsePtr)> callback)` | Non-blocking request. Callback receives response (or NULL on failure). |

### Usage

```cpp
#include "hv/requests.h"

// Simple GET
auto resp = requests::get("http://moonraker:7121/api/printer");
if (resp && resp->status_code == 200) {
    auto& j = resp->GetJson();
    // use j["result"]
}

// POST with JSON body
auto resp = requests::post("http://moonraker:7121/api/printer/command",
    R"({"command": "G28"})", {{"Content-Type", "application/json"}});

// Custom request
auto req = HttpRequestPtr(new HttpRequest);
req->method = HTTP_POST;
req->url = "http://moonraker:7121/printer/print/start";
req->Json({{"filename", "test.gcode"}});
auto resp = requests::request(req);
```
