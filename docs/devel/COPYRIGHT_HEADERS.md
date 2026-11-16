# Copyright Headers for HelixScreen

All source files in the HelixScreen project must include an SPDX license identifier.

## SPDX License Identifier (Required)

All source files must include this header at the top of the file:

### C/C++ Files (.c, .cpp, .h, .hpp)

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
```

### Python Files (.py)

```python
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
```

### Bash Scripts (.sh)

```bash
#!/usr/bin/env bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
```

### XML Files (.xml)

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
```

## Why SPDX?

SPDX (Software Package Data Exchange) identifiers are:
- **Machine-readable**: Automated tools can instantly detect licenses
- **Industry standard**: Used by Linux kernel, LLVM, and major open source projects
- **Legally equivalent**: SPDX + LICENSE file = complete legal notice
- **Concise**: 2 lines instead of 20+ lines of boilerplate per file

The full copyright notice and GPL license text are in the `COPYRIGHT` and `LICENSE` files at the repository root.

## Adding Headers to New Files

When creating new source files:

1. Add the appropriate SPDX header at the top (see examples above)
2. Preserve special first lines (shebang `#!/usr/bin/env`, XML declaration `<?xml`)
3. Leave a blank line after the header before code begins

## Batch Application

To add SPDX headers to multiple files:

```bash
# Dry-run (shows what would be changed)
./scripts/add-spdx-headers.sh

# Apply to all source files
./scripts/add-spdx-headers.sh --apply

# Apply to specific files
./scripts/add-spdx-headers.sh --apply src/myfile.cpp include/myheader.h

# Verbose output with diffs
./scripts/add-spdx-headers.sh --verbose
```

The script automatically:
- Detects file type (C/C++, Python, Bash, XML)
- Preserves shebangs and XML declarations
- Removes old verbose copyright boilerplate
- Adds clean SPDX headers
- Skips files that already have SPDX identifiers

## Verification

To check that all files have SPDX identifiers:

```bash
# Run quality checks (includes SPDX verification)
./scripts/quality-checks.sh

# Manual check for missing SPDX in C/C++ files
grep -L "SPDX-License-Identifier: GPL-3.0-or-later" src/*.cpp include/*.h

# Check specific file
head -5 src/main.cpp | grep SPDX
```

## Pre-commit Hook

`core.hooksPath` is `.githooks` (tracked; `make setup` and `make install-deps` set it), so the pre-commit hook runs the staged-only quality checks, header check included, with no install step. The check reads the first three lines of every `.cpp`, `.c`, `.h` and `.mm` file under `src/` and `include/` for the `SPDX-License-Identifier: GPL-3.0-or-later` line (`scripts/qc/copyright.sh`).

## Full Copyright Notice

For the complete copyright and license information, see:
- `COPYRIGHT` - Comprehensive copyright notice
- `LICENSE` - Full GPL-3.0-or-later license text

## Third-Party Code

Third-party libraries in `lib/` retain their original licenses and copyright notices. See individual library directories for their license information.

## Notes

- **Year**: Update to current year for new files (e.g., 2026, 2027)
- **No verbose headers**: SPDX identifiers replace verbose GPL boilerplate
- **Machine-readable**: Tools like `reuse` can verify SPDX compliance
- **Standards compliant**: Follows REUSE 3.0 best practices
