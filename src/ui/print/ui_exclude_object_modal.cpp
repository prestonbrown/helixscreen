// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_object_modal.h"

void ExcludeObjectModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");
}
