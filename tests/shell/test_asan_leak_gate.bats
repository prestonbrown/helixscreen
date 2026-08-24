#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_asan_leaks.py — the at-exit LeakSanitizer ratchet
# (#1279).
#
# The gate's hard problem is that a leak origin is spelled differently by every
# symbolizer, and only one spelling can be in the baseline. GCC and clang differ
# in FOUR ways on the same frame:
#
#   gcc:    void ns::func<Arg, {lambda(Arg*)#1}>(...)     /abs/or/rel/path.cpp:339
#   clang:  RetType ns::func<Arg, ns::site()::$_3>(...)   /abs/checkout/path.cpp:339:17
#
# (return types, lambda spellings, file:line:column, absolute paths). The nightly
# runs clang; a developer's `make test-asan` may run either. The 2026-08-16
# nightly failed on all 23 origins at once because the baseline held only one
# compiler's spelling — the gate could never match its own baseline in CI.
#
# These tests pin the property whose absence broke the nightly: the same leak
# site, symbolized by either compiler, is ONE key.

GATE="scripts/check_asan_leaks.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/asan_leaks"
    mkdir -p "$FIXTURE_DIR"
    LOG="$FIXTURE_DIR/run.log"
    BASE="$FIXTURE_DIR/baseline.txt"
    : > "$LOG"
}

# One complete leak report around the appended blocks.
finish_run() { # $1=bytes $2=allocations
    printf 'SUMMARY: AddressSanitizer: %s byte(s) leaked in %s allocation(s).\n' "$1" "$2" >> "$LOG"
    printf 'test cases: 100 | 100 passed\nassertions: 500 | 500 passed\n' >> "$LOG"
}

leak_header() {
    printf 'ERROR: LeakSanitizer: detected memory leaks\n' >> "$LOG"
}

frame() { # $1=frame# $2=rest
    printf '    #%s 0x000000000042 in %s\n' "$1" "$2" >> "$LOG"
}

run_gate() { run python3 "$GATE" --baseline "$BASE" "$LOG"; }

# A ui_button_create leak spelled the clang/CI way: absolute checkout path with
# tests/unit/../../ collapsed by normpath, file:line:COLUMN, (anonymous
# namespace):: prefix. The ui_button leak from the real 2026-08-18 nightly.
clang_ui_button_log() {
    leak_header
    printf 'Direct leak of 7168 byte(s) in 128 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) (/home/runner/work/helixscreen/helixscreen/build/bin/helix-tests-asan+0x351a24d) (BuildId: dead)'
    frame 1 '(anonymous namespace)::ui_button_create(_lv_xml_parser_state_t*, char const**) /home/runner/work/helixscreen/helixscreen/tests/unit/../../src/ui/ui_button.cpp:562:26'
    frame 2 'view_start_element_handler /home/runner/work/helixscreen/helixscreen/lib/helix-xml/src/xml/lv_xml.c:2323:23'
    finish_run 7168 128
}

# The same leak spelled the gcc way: return type only on templates, no column,
# {lambda(...)#n} template tails, absolute home path.
gcc_ui_button_log() {
    leak_header
    printf 'Direct leak of 7168 byte(s) in 128 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) ../../../../src/libsanitizer/asan/asan_new_delete.cpp:95'
    frame 1 'ui_button_create(_lv_xml_parser_state_t*, char const**) /home/pbrown/Code/Printing/helixscreen/src/ui/ui_button.cpp:568'
    frame 2 'view_start_element_handler /home/pbrown/Code/Printing/helixscreen/lib/helix-xml/src/xml/lv_xml.c:2401'
    finish_run 7168 128
}

# --- one site, two compilers, one key ---------------------------------------

@test "a baseline written from clang frames accepts the gcc spelling of the same leaks" {
    clang_ui_button_log
    run python3 "$GATE" --write-baseline "$BASE" "$LOG"
    [ "$status" -eq 0 ]
    grep -q '^src/ui/ui_button\.cpp::ui_button_create$' "$BASE"

    : > "$LOG"
    gcc_ui_button_log
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *'no new origins'* ]]
}

@test "a baseline written from gcc frames accepts the clang spelling of the same leaks" {
    gcc_ui_button_log
    run python3 "$GATE" --write-baseline "$BASE" "$LOG"

    : > "$LOG"
    clang_ui_button_log
    run_gate
    [ "$status" -eq 0 ]
}

@test "template free functions drop the return type both compilers print" {
    # gcc:  void helix::ui::observe_int_sync<HomePanel, {lambda...#1}>(...)
    # clang: ObserverGuard helix::ui::observe_int_sync<HomePanel, ...::$_8>(...)
    # Both must key as include/observer_factory.h::observe_int_sync<HomePanel>:
    # return type dropped, and the function's namespace dropped too, because
    # gcc's libbacktrace reconstructs template names from DWARF WITHOUT the
    # enclosing namespaces while clang prints them qualified.
    leak_header
    printf 'Direct leak of 32 byte(s) in 1 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) ../../../../src/libsanitizer/asan/asan_new_delete.cpp:95'
    frame 1 'observe_int_sync<HomePanel, HomePanel::attach(int*)::<lambda(HomePanel*, int)#1> >(int*, HomePanel*, HomePanel::attach(int*)::<lambda(HomePanel*, int)#1>) include/observer_factory.h:339'
    finish_run 32 1
    run python3 "$GATE" --write-baseline "$BASE" "$LOG"
    grep -q '^include/observer_factory\.h::observe_int_sync<HomePanel>$' "$BASE"

    : > "$LOG"
    leak_header
    printf 'Direct leak of 32 byte(s) in 1 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) (/home/runner/work/helixscreen/helixscreen/build/bin/helix-tests-asan+0x351a24d) (BuildId: dead)'
    frame 1 'ObserverGuard helix::ui::observe_int_sync<HomePanel, HomePanel::attach(int*)::$_8>(_lv_subject_t*, HomePanel*, HomePanel::attach(int*)::$_8) include/observer_factory.h:339:17'
    finish_run 32 1
    run_gate
    [ "$status" -eq 0 ]
}

@test "CamelCase class qualifiers survive the namespace strip" {
    # MoonrakerClientMock is a class in the global namespace; the lowercase
    # namespace strip must not eat its qualifier or keys fork between the
    # compilers that print it (gcc, unqualified-by-DWARF is impossible here —
    # the class IS the outermost scope) and clang (prints it as-is).
    leak_header
    printf 'Direct leak of 80 byte(s) in 1 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) (helix-tests-asan+0x351a24d)'
    frame 1 'MoonrakerClientMock::dispatch_shaper_calibrate_response(char) src/api/moonraker_client_mock.cpp:4749:17'
    finish_run 80 1
    run python3 "$GATE" --list "$LOG"
    [[ "$output" == *'src/api/moonraker_client_mock.cpp::MoonrakerClientMock::dispatch_shaper_calibrate_response'* ]]

    : > "$LOG"
    leak_header
    printf 'Direct leak of 80 byte(s) in 1 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) (helix-tests-asan+0x351a24d)'
    frame 1 'helix::ui::observe_int_sync<helix::PrintStatusWidget, helix::PrintStatusWidget::attach(lv_obj_t*, lv_obj_t*)::<lambda(helix::PrintStatusWidget*, int)> >(_lv_subject_t*, helix::PrintStatusWidget*, helix::PrintStatusWidget::attach(lv_obj_t*, lv_obj_t*)::<lambda(helix::PrintStatusWidget*, int)>) include/observer_factory.h:339:17'
    finish_run 80 1
    run python3 "$GATE" --list "$LOG"
    [[ "$output" == *'include/observer_factory.h::observe_int_sync<helix::PrintStatusWidget>'* ]]
}

@test "lambda numbering and abi tags do not fork keys" {
    # Adding a lambda above the site renumbers $_11 -> $_12 (clang) without
    # moving the leak; clang also appends [abi:cxx11] to operator().
    leak_header
    printf 'Direct leak of 216 byte(s) in 3 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) (helix-tests-asan+0x351a24d)'
    frame 1 'CATCH2_INTERNAL_TEST_22()::$_11::operator()[abi:cxx11] tests/unit/test_panel_widget_manager.cpp:206:30'
    finish_run 216 3
    run python3 "$GATE" --write-baseline "$BASE" "$LOG"

    : > "$LOG"
    leak_header
    printf 'Direct leak of 216 byte(s) in 3 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) (helix-tests-asan+0x351a24d)'
    frame 1 'CATCH2_INTERNAL_TEST_23()::$_12::operator() tests/unit/test_panel_widget_manager.cpp:210:9'
    finish_run 216 3
    run_gate
    [ "$status" -eq 0 ]
}

@test "an inlined std frame falls through to the next our-code frame" {
    # The TempGraphController leak: frame #1 is a <unique_ptr.h> inline (not our
    # code), frame #2 is the test method. Under a broken parser the unique_ptr
    # fallback key hid the real origin and the leak looked "new" in CI.
    leak_header
    printf 'Direct leak of 352 byte(s) in 1 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) (/usr/lib/.../libasan.so+0x351a24d)'
    frame 1 'std::__detail::_MakeUniq<helix::TempGraphController>::__single_object std::make_unique<helix::TempGraphController, _lv_obj_t*&, helix::TempGraphControllerConfig&>(_lv_obj_t*&, helix::TempGraphControllerConfig&) /usr/include/c++/12/bits/unique_ptr.h:1065:30'
    frame 2 '(anonymous namespace)::CATCH2_INTERNAL_TEST_22::test() tests/unit/test_temp_graph_controller.cpp:420:23'
    frame 3 'Catch::TestInvokerAsMethod<(anonymous namespace)::CATCH2_INTERNAL_TEST_22>::invoke() const tests/catch_amalgamated.hpp:5871:9'
    finish_run 352 1
    run python3 "$GATE" --write-baseline "$BASE" "$LOG"
    grep -q '^tests/unit/test_temp_graph_controller\.cpp::CATCH2_INTERNAL_TEST::test$' "$BASE"
}

# --- what must still fail ---------------------------------------------------

@test "a genuinely new origin fails" {
    gcc_ui_button_log
    run python3 "$GATE" --write-baseline "$BASE" "$LOG"

    : > "$LOG"
    leak_header
    printf 'Direct leak of 144 byte(s) in 2 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) (helix-tests-asan+0x351a24d)'
    frame 1 'ui_split_button_create(_lv_xml_parser_state_t*, char const**) src/ui/ui_split_button.cpp:435:18'
    finish_run 144 2
    run_gate
    [ "$status" -ne 0 ]
    [[ "$output" == *'ui_split_button_create'* ]]
}

@test "an already-baselined origin leaking more fails on the ceilings" {
    clang_ui_button_log
    run python3 "$GATE" --write-baseline "$BASE" "$LOG"

    : > "$LOG"
    leak_header
    printf 'Direct leak of 999999 byte(s) in 999 object(s) allocated from:\n' >> "$LOG"
    frame 0 'operator new(unsigned long) (helix-tests-asan+0x351a24d)'
    frame 1 '(anonymous namespace)::ui_button_create(_lv_xml_parser_state_t*, char const**) src/ui/ui_button.cpp:570:9'
    finish_run 999999 999
    run_gate
    [ "$status" -ne 0 ]
    [[ "$output" == *'exceeds the baseline ceiling'* ]]
}

# --- parser safety rails ----------------------------------------------------

@test "system paths that merely contain a tree-root segment stay foreign" {
    # /usr/src/... has a src/ segment; without the existence check the parser
    # would relativize it to repo code and key system leaks as ours.
    leak_header
    printf 'Direct leak of 64 byte(s) in 1 object(s) allocated from:\n' >> "$LOG"
    frame 0 'malloc ../../../../src/libsanitizer/sanitizer_common/sanitizer_allocator.cpp:1'
    frame 1 'weird_system_alloc(unsigned long) /usr/src/linux/mm/slab.c:100'
    finish_run 64 1
    run python3 "$GATE" --list "$LOG"
    [[ "$output" != *'block(s)  src/linux'* ]] || return 1
    [[ "$output" == *'  /usr/src/linux/mm/slab.c::weird_system_alloc'* ]]
}

@test "leaks with no usable stack are counted under <no-stack>" {
    # CI runs hit "Symbolizer buffer too small" on giant template frames; those
    # blocks must land somewhere visible, not vanish.
    leak_header
    printf 'Direct leak of 2224 byte(s) in 37 object(s) allocated from:\n' >> "$LOG"
    printf '    #0 0x000000000042 in <nested template beyond symbolizer buffer>\n' >> "$LOG"
    finish_run 2224 37
    run python3 "$GATE" --list "$LOG"
    [[ "$output" == *'<no-stack>'* ]]
}
