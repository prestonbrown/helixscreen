# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for scripts/rename_update_queue_drain.py: the rewrite is exact and idempotent."""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from rename_update_queue_drain import rewrite  # noqa: E402

SHIM_INC = '#include "../test_helpers/update_queue_test_access.h"\n'
QUEUE_INC = '#include "core/update_queue.h"\n'


def run(text, rel="tests/unit/test_x.cpp"):
    return rewrite(text, rel)


def test_drain_and_drain_all_move_onto_the_receiver():
    src = (
        "UpdateQueueTestAccess::drain(q);\n"
        "helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());\n"
    )
    assert run(src) == "q.drain();\nhelix::ui::UpdateQueue::instance().drain_until_empty();\n"


def test_a_pass_cap_survives_and_may_span_lines():
    src = "UpdateQueueTestAccess::drain_all(UpdateQueue::instance(),\n    limit(a, b) + 1);\n"
    assert run(src) == "UpdateQueue::instance().drain_until_empty(limit(a, b) + 1);\n"


def test_a_receiver_that_is_not_postfix_gets_parentheses():
    assert run("UpdateQueueTestAccess::drain(*queue);") == "(*queue).drain();"
    assert run("UpdateQueueTestAccess::drain(ctx.queue());") == "ctx.queue().drain();"


def test_other_accessors_and_lookalikes_are_untouched():
    src = (
        SHIM_INC + "using helix::ui::UpdateQueueTestAccess;\n"
        "UpdateQueueTestAccess::drain(q);\n"
        "CHECK(UpdateQueueTestAccess::queue_empty(q));\n"
        "MyTestAccess::drain_all(q);\n"
    )
    assert run(src) == (
        SHIM_INC + "using helix::ui::UpdateQueueTestAccess;\n"
        "q.drain();\n"
        "CHECK(UpdateQueueTestAccess::queue_empty(q));\n"
        "MyTestAccess::drain_all(q);\n"
    )


def test_the_last_use_takes_the_using_and_include_with_it():
    src = SHIM_INC + "using helix::ui::UpdateQueueTestAccess;\nUpdateQueueTestAccess::drain(q);\n"
    assert run(src) == QUEUE_INC + "q.drain();\n"
    already = QUEUE_INC + src
    assert run(already) == QUEUE_INC + "q.drain();\n"


def test_a_comment_naming_the_shim_does_not_keep_its_include():
    src = SHIM_INC + "// see UpdateQueueTestAccess\nUpdateQueueTestAccess::drain(q);\n"
    assert run(src) == QUEUE_INC + "// see UpdateQueueTestAccess\nq.drain();\n"


def test_a_digit_separator_does_not_hide_a_use_of_the_shim():
    src = SHIM_INC + "int n = 1'000;\nUpdateQueueTestAccess::queue_empty(q);\n"
    assert run(src) == src


def test_a_use_reached_through_another_header_gains_the_include():
    src = '#include "fixture.h"\n\nCHECK(UpdateQueueTestAccess::queue_empty(q));\n'
    assert run(src) == (
        '#include "fixture.h"\n#include "test_helpers/update_queue_test_access.h"\n\n'
        "CHECK(UpdateQueueTestAccess::queue_empty(q));\n"
    )
    assert '"update_queue_test_access.h"' in run(src, "tests/test_helpers/rig.h")


def test_rewrite_is_idempotent():
    for src in (
        SHIM_INC + "UpdateQueueTestAccess::drain_all(q, 6);\n",
        SHIM_INC + "UpdateQueueTestAccess::drain(q);\nUpdateQueueTestAccess::queue_empty(q);\n",
    ):
        once = run(src)
        assert run(once) == once
