# Copyright Headers for HelixScreen

All source files in the HelixScreen project must include the appropriate GPL v3 copyright header.

## Company Information
- **Company:** 356C LLC
- **Author:** Preston Brown
- **Email:** pbrown@brown-house.net
- **License:** GNU General Public License v3.0 or later

## SPDX License Identifier

All C/C++ source and header files must include an SPDX license identifier comment at the very top of the file (before the full GPL header). This is required by the quality checks script.

```cpp
// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later
```

## For C++ Source Files (.cpp)

```cpp
// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */
```

## For Header Files (.h)

```cpp
// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */
```

## For XML Files (.xml)

```xml
<!--
  Copyright (C) 2025 356C LLC
  Author: Preston Brown <pbrown@brown-house.net>

  This file is part of HelixScreen.

  HelixScreen is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  HelixScreen is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
-->
```

## Notes

- **XML files with declarations:** For XML files that start with `<?xml version="1.0"?>`, place the copyright header **after** the XML declaration.
- **Copyright year:** Update the year in new files as appropriate (e.g., 2026, 2027, etc.).
- **Multiple contributors:** If multiple authors contribute to a file, add additional `Author:` lines.

## Applying Headers to Multiple Files

A Python script is available to batch-apply headers:

```bash
# Create a temporary script (see /tmp/add_headers.py for reference)
python3 scripts/add_copyright_headers.py .
```

## Verification

To check that all project files have copyright headers:

```bash
# Run the quality checks script (checks SPDX identifiers and more)
./scripts/quality-checks.sh --staged-only  # For pre-commit
./scripts/quality-checks.sh                # For all files

# Manual check for missing SPDX identifiers in C++ files
grep -L "SPDX-License-Identifier: GPL-3.0-or-later" src/*.cpp include/*.h

# Manual check for missing headers in XML files
grep -L "Copyright (C)" ui_xml/*.xml
```

All project source files should include the appropriate header. Third-party libraries (e.g., LVGL, submodules) retain their original licenses.
