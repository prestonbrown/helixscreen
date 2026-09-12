# Continue: #1585 — manifestation A is SOLVED, manifestation B is open

Overnight session reviewed GLM 5.3's fixes for #1575/#1586/#1587/#1588/#1589/#1400 and the
notes on #1585. Everything found is fixed and committed to main (11 commits, suite green).
One thing remains.

NOT PUSHED: local main is 7 ahead of origin, including other sessions' commits. A peer session
said local main running ahead of origin is by design in this tree, so I left it alone rather
than publishing their work for them. Check with them before pushing.

## What got solved overnight (no action needed, read for context)

The nightly's `[ISOLATION-LEAK] ... 8 -> 9` report is a FALSE POSITIVE, confirmed by
experiment and written up in the #1585 comments.

Mechanism: `lib/libhv/include/hv/hthreadpool.h#delThread` decrements `cur_thread_num` and
`idle_thread_num` for a worker retiring past `max_idle_time`, but defers its join to a
successor call that, at `min=1 max=2`, only happens if the pool grows and idles out again.
Accounting says one worker while two OS threads exist; the next `commit()` creates a
replacement that parks on the condvar, which is the `futex_do_wait` arrival the listener names.

Proof: `HThreadPool`'s ctor takes `max_idle_ms` (default 60000) and an unsharded run is ~2 min.
Raising it in `src/print/thumbnail_processor.cpp` alone:

    default  (60000)    7 runs, 7 leaks
    raised (3600000)    3 runs, 0 leaks

Bounded, not dangerous: `HThreadPool::stop()` joins everything still in `threads`, including
STOP-marked entries, so nothing is stranded at teardown and no std::thread dtor runs joinable.

THREE FIX OPTIONS, all laid out in the #1585 comment, NONE implemented — the choice has a
resource tradeoff on 64MB devices that is Preston's to make:
  1. patch libhv so delThread joins rather than deferring (most correct, goes via patches/)
  2. give the thumbnail pool min == max so no worker retires (one line, ours, one permanent thread)
  3. raise that pool's max_idle_ms (same cost as 2, more indirection)

## THE OPEN WORK: manifestation B

Nothing above explains it. From the issue's own bisect notes:

    ./build/bin/helix-tests "~[.] ~[slow]" --shard-count 4 --shard-index 1
    # 7044 passed, 1 skipped, then AFTER the Catch2 summary:
    #   pure virtual method called
    #   terminate called without an active exception
    #   exit 134 (core dumped)

Deterministic 3-for-3 when the issue was written. VERIFY IT STILL REPRODUCES FIRST — main has
moved a lot since, and shard index ranges shift with the test total.

Corrected shard arithmetic (`Catch::createShard`, `tests/catch_amalgamated.hpp`): leftovers go
one each to low-index shards, so with 14090 cases shard 2/8 is [3524,5285) NOT [3522,5283),
and "nesting when k doubles" is FALSE with non-zero leftovers. Recompute for the current total.

Best untested hypothesis, from `reference_http_lanes_at_exit_need_spdlog_first`: helix-tests
exits with both HttpExecutor lanes still running, safe only because the isolation listener
installs spdlog's null default logger first, constructing the registry before the lanes. A
worker logs on its way out of `~HttpExecutor`, and `spdlog::sinks::sink` is an abstract base
with pure virtuals — a worker logging into a partially-destroyed sink during
`__run_exit_handlers` is exactly `pure virtual method called`, at exactly the observed time.

Cheapest decisive step: one `gdb -batch -ex run -ex 'thread apply all bt' --args
./build/bin/helix-tests "~[.] ~[slow]" --shard-count 4 --shard-index 1`, breaking on
`__cxa_pure_virtual`. It names the object and the thread outright. A SINGLE gdb — do not fan
out parallel gdb on this binary, each loads multi-GB symbols (~6.6GB RSS) and 12 of them took
the box down once.

## Open decisions for Preston

- #1585 fix option 1/2/3 above.
- #1589's 256 KiB seed cap in `AfcMessageDedup::init`: kept deliberately, it is what makes the
  widened `catch (std::exception)` testable. Drop if you disagree.
- The `unshare E2E` bats case fails on unmodified main and is untracked. Every agent this
  session wrote "green except the pre-existing failure" around it. Worth an issue.
- #1593 (DisplayManager settle_display_rotation helper) and #1026 (per-zone GATES=, HUMIDITY=)
  are both Backlog, both need hardware.
- #1601 carries the two orphaned anomaly guards from #1400 (Backlog). #1400 stays closed.

## House rules that bit repeatedly

- NEVER conclude from an artifact without checking it is finished and which run produced it.
  Three wrong conclusions tonight: an agent read a source file mid-mutation and reported an
  abandoned mutant; an agent argued from run 1's output about run 2's anomaly; I counted leaks
  from a log still being written and called a deterministic bug intermittent.
- `mutate_diff` was fixed in `517101845` so a kill needs evidence, not a non-zero exit. Still
  ALWAYS pass `--base HEAD` for uncommitted work or it picks a fork point and sweeps in other
  sessions' commits.
- Never revert a file in this shared tree with git. Snapshot with `cp` first — a peer agent
  landed work mid-experiment tonight and only the cp snapshot saved it.
