#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if the ESP32 app image exceeds its budget. Usage:
   check_esp32_size.py build/helixscreen_esp32.bin firmware/helixscreen-esp32/size_budget.json"""
import json
import os
import sys

def main() -> int:
    bin_path, budget_path = sys.argv[1], sys.argv[2]
    size = os.path.getsize(bin_path)
    budget = json.load(open(budget_path))["app_max_bytes"]
    pct = 100.0 * size / budget
    print(f"esp32 image: {size} bytes / budget {budget} ({pct:.1f}%)")
    if size > budget:
        print("FAIL: image exceeds budget", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
