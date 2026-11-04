#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_imperative_ui.py. A widget's lookup and its mutation
# usually sit in different functions, so the gate has to judge them per file: a
# per-function view reports a panel that caches every pointer in setup() as clean.

load helpers

setup() {
  cd "$BATS_TEST_DIRNAME/../.." || return 1
  SRC="$BATS_TEST_TMPDIR/panel.cpp"
}

# Prints the gate's total for $SRC.
total() {
  python3 scripts/check_imperative_ui.py --summary "$SRC" | awk '$1 == "TOTAL" {print $2}'
}

expect_total() {
  local got
  got=$(total)
  [ "$got" = "$1" ] || fail "expected $1 site(s), the gate counted '$got' in: $(cat "$SRC")"
}

@test "a pointer cached in setup() and mutated in update_*() is counted" {
  cat > "$SRC" <<'CPP'
void Panel::setup(lv_obj_t* root) {
    title_ = lv_obj_find_by_name(root, "title");
    icon_ = helix::ui::find_required(root, "icon");
}

void Panel::update_title() {
    lv_label_set_text(title_, "x");
    lv_obj_add_flag(icon_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(icon_, 0, 0);
    lv_obj_add_event_cb(icon_, on_tap, LV_EVENT_CLICKED, this);
}
CPP
  expect_total 4
}

@test "a mutation written above the lookup in the file is counted" {
  cat > "$SRC" <<'CPP'
void Panel::update_title() {
    lv_label_set_text(title_, "x");
}

void Panel::setup(lv_obj_t* root) {
    title_ = lv_obj_find_by_name(root, "title");
}
CPP
  expect_total 1
}

@test "a pointer cached through find_widget() is counted" {
  cat > "$SRC" <<'CPP'
void Modal::on_show() {
    note_ = find_widget("note");
}

void Modal::update() {
    lv_label_set_text(note_, "x");
}
CPP
  expect_total 1
}

@test "a pointer cached into a struct member is counted" {
  cat > "$SRC" <<'CPP'
void bind(Row* row, lv_obj_t* root) {
    row->label = lv_obj_find_by_name(root, "label");
}

void refresh(Row* row) {
    lv_label_set_text(row->label, "x");
}
CPP
  expect_total 1
}

@test "a lookup and its mutation in one function stay counted" {
  cat > "$SRC" <<'CPP'
void Panel::setup(lv_obj_t* root) {
    lv_obj_t* title = lv_obj_find_by_name(root, "title");
    lv_label_set_text(title, "x");
}
CPP
  expect_total 1
}

@test "a parameter sharing a looked-up name is not that widget" {
  cat > "$SRC" <<'CPP'
void Panel::setup(lv_obj_t* root) {
    lv_obj_t* label = lv_obj_find_by_name(root, "label");
    use(label);
}

static void paint(lv_obj_t* label) {
    lv_label_set_text(label, "x");
}
CPP
  expect_total 0
}

@test "a local sharing a looked-up name is not that widget" {
  cat > "$SRC" <<'CPP'
void Panel::setup(lv_obj_t* root) {
    lv_obj_t* btn = lv_obj_find_by_name(root, "btn");
    use(btn);
}

void Panel::add_row(lv_obj_t* list) {
    auto* btn = make_row(list);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
}
CPP
  expect_total 0
}

@test "a widget created in C++ in the mutating function is not XML-owned" {
  cat > "$SRC" <<'CPP'
void Panel::setup(lv_obj_t* root) {
    badge_ = lv_obj_find_by_name(root, "badge");
}

void Panel::rebuild(lv_obj_t* root) {
    badge_ = lv_label_create(root);
    lv_label_set_text(badge_, "x");
}
CPP
  expect_total 0
}

@test "opt-outs and structural events stay silent per file" {
  cat > "$SRC" <<'CPP'
void Panel::setup(lv_obj_t* root) {
    title_ = lv_obj_find_by_name(root, "title");
}

void Panel::update() {
    lv_label_set_text(title_, "x");  // DECLARATIVE_OK: measured text
    lv_obj_add_event_cb(title_, on_gone, LV_EVENT_DELETE, this);
}
CPP
  expect_total 0
}

@test "a file registering a custom XML widget stays exempt" {
  cat > "$SRC" <<'CPP'
void init() { lv_xml_register_widget("w", create, apply); }

void Panel::setup(lv_obj_t* root) {
    title_ = lv_obj_find_by_name(root, "title");
}

void Panel::update() {
    lv_label_set_text(title_, "x");
}
CPP
  expect_total 0
}

@test "the gate fails above its baseline and passes at it" {
  cat > "$SRC" <<'CPP'
void Panel::setup(lv_obj_t* root) {
    title_ = lv_obj_find_by_name(root, "title");
}

void Panel::update() {
    lv_label_set_text(title_, "x");
}
CPP
  run python3 scripts/check_imperative_ui.py --max-allowed 0 "$SRC"
  [ "$status" -eq 1 ] || fail "$output"
  run python3 scripts/check_imperative_ui.py --max-allowed 1 "$SRC"
  [ "$status" -eq 0 ] || fail "$output"
}
