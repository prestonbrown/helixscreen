#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/esp32_package_release.sh against a fake build directory. A fake
# python stands in for esptool's merge_bin and writes an image of
# $FAKE_FACTORY_BYTES, so the cfg-overlap guard can be driven both ways
# without ESP-IDF.

load helpers

SCRIPT="scripts/esp32_package_release.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FW="$BATS_TEST_TMPDIR/fw"
    BUILD="$FW/build"
    OUT="$BATS_TEST_TMPDIR/out"
    mkdir -p "$BUILD/bootloader" "$BUILD/partition_table"
    printf -- '--flash_mode dout --flash_freq 80m --flash_size 16MB\n0x0 bootloader/bootloader.bin\n0x20000 app.bin\n0x8000 partition_table/partition-table.bin\n' \
        > "$BUILD/flash_args"
    echo boot > "$BUILD/bootloader/bootloader.bin"
    echo app > "$BUILD/app.bin"
    echo table > "$BUILD/partition_table/partition-table.bin"
    echo '{"format": 1, "tokens": {}}' > "$BUILD/helixscreen_esp32.logtok.json"
    printf 'nvs,      data, nvs,     0x9000,   0x6000,\ncfg,      data, spiffs,  0x1000,   0x20000,\n' \
        > "$FW/partitions.csv"

    FAKE_PY="$BATS_TEST_TMPDIR/fakepy"
    cat > "$FAKE_PY" <<'EOF'
#!/usr/bin/env bash
# Mimics `python -m esptool ... merge_bin -o FILE @flash_args`.
while [ $# -gt 0 ]; do
    if [ "$1" = "-o" ]; then head -c "$FAKE_FACTORY_BYTES" /dev/zero > "$2"; exit 0; fi
    shift
done
exit 1
EOF
    chmod +x "$FAKE_PY"
}

@test "the zip holds the factory image, flash_args, every image it names and a README" {
    PYTHON="$FAKE_PY" FAKE_FACTORY_BYTES=4096 run bash "$SCRIPT" v1.2.3 "$BUILD" "$OUT"
    [ "$status" -eq 0 ] || fail "$output"

    run unzip -Z1 "$OUT/helixscreen-esp32-ktouch-v1.2.3.zip"
    [ "$status" -eq 0 ] || fail "$output"
    local d=helixscreen-esp32-ktouch-v1.2.3 f
    for f in helixscreen-esp32-ktouch-factory.bin flash_args README.txt app.bin \
             bootloader/bootloader.bin partition_table/partition-table.bin \
             helixscreen-esp32-ktouch-logtok.json; do
        grep -qx "$d/$f" <<<"$output" || fail "missing $d/$f in:"$'\n'"$output"
    done

    run unzip -p "$OUT/helixscreen-esp32-ktouch-v1.2.3.zip" "$d/README.txt"
    [[ "$output" == *"write_flash 0x0 helixscreen-esp32-ktouch-factory.bin"* ]] || fail "$output"
    [[ "$output" == *"write_flash @flash_args"* ]] || fail "$output"
    [[ "$output" == *"detokenize_log.py helixscreen-esp32-ktouch-logtok.json"* ]] || fail "$output"
}

@test "a build without its token database is refused" {
    rm "$BUILD/helixscreen_esp32.logtok.json"
    PYTHON="$FAKE_PY" FAKE_FACTORY_BYTES=4096 run bash "$SCRIPT" v1.2.3 "$BUILD" "$OUT"
    [ "$status" -ne 0 ] || fail "packaged a build with no token database: $output"
    [[ "$output" == *"logtok.json"* ]] || fail "$output"
}

@test "a factory image reaching the cfg partition is refused" {
    # cfg sits at 0x1000 in the fixture table: one byte past it must fail.
    PYTHON="$FAKE_PY" FAKE_FACTORY_BYTES=4097 run bash "$SCRIPT" v1.2.3 "$BUILD" "$OUT"
    [ "$status" -ne 0 ] || fail "accepted an image that overwrites cfg: $output"
    [[ "$output" == *"would overwrite cfg"* ]] || fail "$output"
    [ ! -e "$OUT/helixscreen-esp32-ktouch-v1.2.3.zip" ] || fail "zip written anyway"
}
