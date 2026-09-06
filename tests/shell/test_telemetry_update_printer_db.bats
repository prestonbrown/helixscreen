#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests for telemetry-update-printer-db.py

load helpers

setup() {
    TEST_DIR="$(mktemp -d)"

    # Minimal printer_database.json
    cat > "$TEST_DIR/printer_database.json" << 'DBEOF'
{
  "version": "2.0",
  "printers": [
    {
      "id": "test_printer_a",
      "name": "Test Printer A",
      "manufacturer": "TestCorp",
      "preset": "test_a",
      "heuristics": [
        {
          "type": "sensor_match",
          "field": "sensors",
          "pattern": "existing_sensor",
          "confidence": 80,
          "reason": "Already known"
        }
      ]
    }
  ]
}
DBEOF

    # Analysis JSON with candidates for existing + cluster for new
    cat > "$TEST_DIR/analysis.json" << 'ANEOF'
{
  "total_profiles": 100,
  "detected_count": 90,
  "undetected_count": 10,
  "model_distribution": {
    "Test Printer A": 50
  },
  "candidate_heuristics_detected": {
    "Test Printer A": [
      {
        "type": "sensor_match",
        "field": "sensors",
        "pattern": "existing_sensor",
        "confidence": 80,
        "reason": "Exact duplicate — should be skipped"
      },
      {
        "type": "fan_match",
        "field": "fans",
        "pattern": "new_fan",
        "confidence": 75,
        "reason": "Found in 90% of devices"
      }
    ]
  },
  "candidate_heuristics_clusters": [
    {
      "cluster_id": 1,
      "size": 10,
      "kinematics": "cartesian",
      "build_volume": {"x": 220, "y": 220, "z": 250},
      "mcu": "stm32f407",
      "candidate_heuristics": [
        {
          "type": "sensor_match",
          "field": "sensors",
          "pattern": "cluster_sensor",
          "confidence": 85,
          "reason": "Found in 100% of cluster"
        }
      ]
    }
  ],
  "validation_issues": [],
  "discriminating_names": {}
}
ANEOF

    mkdir -p "$TEST_DIR/presets"
}

teardown() {
    rm -rf "$TEST_DIR"
}

@test "help flag works" {
    run python3 scripts/telemetry-update-printer-db.py --help
    [ "$status" -eq 0 ]
    contains "analysis_json" "$output"
    [[ "$output" == *"--dry-run"* ]]
}

@test "fails on missing analysis file" {
    run python3 scripts/telemetry-update-printer-db.py /nonexistent.json \
        --db "$TEST_DIR/printer_database.json" --dry-run --skip-existing --skip-new
    [ "$status" -ne 0 ]
    [[ "$output" == *"Cannot load analysis file"* ]]
}

@test "fails on invalid printer database" {
    echo "not json" > "$TEST_DIR/bad_db.json"
    run python3 scripts/telemetry-update-printer-db.py "$TEST_DIR/analysis.json" \
        --db "$TEST_DIR/bad_db.json" --dry-run --skip-existing --skip-new
    [ "$status" -ne 0 ]
    [[ "$output" == *"Cannot load printer database"* ]]
}

@test "dry-run with skip-all shows no changes" {
    run python3 scripts/telemetry-update-printer-db.py "$TEST_DIR/analysis.json" \
        --db "$TEST_DIR/printer_database.json" --dry-run --skip-existing --skip-new
    [ "$status" -eq 0 ]
    contains "Modified entries: 0" "$output"
    [[ "$output" == *"New entries: 0"* ]]
}

@test "database file is not modified in dry-run" {
    original=$(cat "$TEST_DIR/printer_database.json")
    run python3 scripts/telemetry-update-printer-db.py "$TEST_DIR/analysis.json" \
        --db "$TEST_DIR/printer_database.json" --dry-run --skip-existing --skip-new \
        --presets-dir "$TEST_DIR/presets/"
    [ "$status" -eq 0 ]
    current=$(cat "$TEST_DIR/printer_database.json")
    [ "$original" = "$current" ]
}

@test "displays telemetry summary on startup" {
    run python3 scripts/telemetry-update-printer-db.py "$TEST_DIR/analysis.json" \
        --db "$TEST_DIR/printer_database.json" --dry-run --skip-existing --skip-new
    [ "$status" -eq 0 ]
    contains "Telemetry profiles: 100" "$output"
    contains "90 detected" "$output"
    contains "10 undetected" "$output"
    [[ "$output" == *"DRY RUN"* ]]
}
