# ESP32 tokenized logging (#1915)

Preston ruled 2026-11-11: build tokenized logging and keep INFO. The firmware stops storing
info/warn/error/critical format strings in flash; it prints a compact token record per log
call, and a host tool turns the serial log back into text with a token database produced
by the same build. Desktop and Linux builds keep real spdlog, unchanged.

## How the token is made (gnu++17, no call-site rewrite)

Call sites stay `spdlog::info("temp: {}", t)`. A C++17 function cannot see its format
literal as a constant, so the interception is a function-like macro in the firmware's
spdlog shim (`components/helixnet/shim/spdlog_shim.h`), one per level name:

```cpp
#define info(...) info(HELIX_LOGTOK_ARGS(__VA_ARGS__))   // same for warn, error, critical
```

`HELIX_LOGTOK_ARGS` leaves an empty argument list alone (`x.error()` still compiles) and
wraps the first argument:

- `helix::logtok::is_literal_run(#x)` is a constexpr scan of the stringized argument: true
  only when it is one or more adjacent plain `"..."` literals.
- Literal: the argument becomes `helix::logtok::Site<N>{x, token}`, where `token` is the
  address of a function-local `static const char[] = #x` placed in its own section
  `.helix_logfmt.<__COUNTER__>` (one name per site, so inline and template functions do not
  hit GCC's section type conflict). The linker script fragment `helix_logfmt.ld` collects
  those into one `(INFO)` output section at address 0: it stays in the ELF, never reaches
  the image, and each token is that string's offset (a small integer, 3 varint bytes).
  The shim's `info(Site<N>, args...)` overload is `always_inline` and reads only the token,
  so the original literal is never referenced and the compiler drops it.
- Anything else (a `std::string`, `fmt::runtime`, a macro-built literal) is forwarded
  unchanged and takes today's text path.

A function named `info`/`warn`/`error`/`critical` that takes arguments therefore collides
with the macro in a firmware translation unit. The four in firmware sources
(`board::info`, `HardwareIssue::info`/`critical`, `DeviceAction::info`,
`ErrorContext::error`/`critical`) are handled by `Site`'s implicit conversions for calls
(`const char*`, `std::string`, `std::string_view`) and by parenthesising the declarator
(`static HardwareIssue (info)(...)`) at the declarations. A new collision fails the
firmware compile (`qc_esp32_compile` at commit), never silently.

Measured alternatives, rejected: hashing the literal at compile time needs it as a
constant expression, which C++17 cannot get through a function parameter; `__builtin_FILE`/
`__builtin_LINE` tokens need the strings parsed back out of the source; renaming the call
sites to a macro touches ~7,100 lines.

## Record format

One line per call: `$` + base64 (no padding) of

| field | encoding |
|---|---|
| level | 1 byte, spdlog numbering |
| token | unsigned LEB128 |
| each argument | 1 tag byte, then: signed int zigzag LEB128, unsigned LEB128, double 8 bytes LE, float 4 bytes LE, bool/char 1 byte, string LEB128 length + bytes, pointer unsigned LEB128 |

Records are capped (`kMaxRecordBytes`, 192); a string argument is cut to fit and flagged.
Arguments come from `fmt::make_format_args`, so the per-call code is one call to the
non-inline `helix::logtok::emit(level, token, fmt::format_args)`. A call whose argument
list holds a type with a custom `fmt::formatter` keeps the text path (its format spec is
only known to the device's formatter), and so keeps its literal in flash.

Header line `$HLT1:<first 9 hex digits of the ELF SHA-256>` before the first record and every `kHeaderEvery` records,
so a capture that starts mid-session still names its build. The ELF SHA-256 is the one the
image already carries (`esp_app_get_elf_sha256`, printed at boot as `ELF file SHA256`).

## Host side

- `scripts/esp32_logtok_db.py <elf> <out.json>`: reads `.helix_logfmt` from the ELF (stdlib
  ELF32 reader), resolves each stringized literal with
  `scripts/translations/extractor.resolve_cpp_literal_run`, writes
  `{"format":1,"elf_sha256":..., "tokens":{"<offset>": "<fmt>"}}`. Runs as a post-build step
  of the app ELF (`main/CMakeLists.txt`), output `build/helixscreen_esp32.logtok.json`.
- `scripts/detokenize_log.py <db.json> [< serial.log]`: rewrites `$...` records to text,
  passes every other line through, warns once when the header's build id does not match the
  database. fmt formatting in Python: `{}`, `{N}`, and the spec mini-language (fill, align,
  sign, `#`, `0`, width, precision, type), with fmt's `{}` rules for float (shortest, no
  trailing `.0`) and bool (`true`); an argument it cannot format prints as `{?value}`.
- Release: `scripts/esp32_package_release.sh` puts the database in the zip folder next to the
  factory image as `helixscreen-esp32-ktouch-logtok.json`.

The firmware shows no log on the device itself (the debug bundle collector and log sinks are
not in the firmware), so the serial console is the only reader.

`CONFIG_HELIX_LOG_TOKENIZE` (default y) switches it off for a plain-text build.

## Size

Census (2026-10-28): 3,831 unique info/warn/error strings, about 217 KB. Expected saving:
most of the string bytes plus the per-call `fmt::format` + `std::string` destructor code.
Measured before/after: `idf.py build` image size, `build/helixscreen_esp32.bin`.

## Tests

- `[tokenlog]` (host, `tests/unit/test_esp32_logtok.cpp`): LEB128/zigzag edges (0, 127,
  128, INT64_MIN/MAX), each argument tag, string truncation at the cap, base64, the
  `is_literal_run` classifier (adjacent literals, escaped quotes, identifiers, prefixed and
  raw literals), and a corpus of real format strings from the tree encoded and compared with
  the checked-in fixture `tests/fixtures/logtok/corpus.json` (which also holds the
  `fmt::format` text of each).
- `tests/python/test_detokenize_log.py`: decodes every fixture record and compares with the
  `fmt::format` text; header mismatch warning; passthrough of non-record lines; the ELF
  reader and literal resolution against a hand-built ELF32.
