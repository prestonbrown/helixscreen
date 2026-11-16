#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PreToolUse hook on Bash (wired in .claude/settings.json). Suggests where a
# heavy command should run; it never blocks and never decides permission. The
# answer is a context block the model reads before the command runs, or nothing.
#
# thelio is shared by several sessions at once; the test host (HELIX_TEST_HOST,
# driven by scripts/test-host-run.sh) is a bigger box that is usually idle.
# Mutation, sanitizers and symbolizers belong on the test host however quiet
# thelio looks, and so does a sweep: `make unit-sweep` and the C++ half of
# `make full-test-run` map to test-host-run.sh's sweep mode; bats stays on thelio,
# because the test container runs as root with no shellcheck. Container builds
# and test loops are only worth moving when thelio is tight. "Tight" is not
# decided here: `helix-claim jobs -v` reports MemAvailable, plus the jobpool's
# free tokens when one is live, and this applies one threshold to each. With no
# pool the -j is cores capped by memory, so memory is the whole signal. An
# explicit -j above that -j is worth a word only with no pool: with one, the
# make shim strips the -j and the pool decides.
#
# A command is judged segment by segment (split on && || ; | and newlines), by
# each segment's own first word, with quoted text masked. So a commit message,
# a grep pattern or a heredoc that merely mentions `make full-test-run` is
# silent.
#
# A runner that sizes itself to the machine (bats -j, GNU parallel, ninja,
# idf.py, a raw `docker run` of a build) started outside `helix-claim hold` or
# pool-docker.sh is outside the jobpool's budget; with a live pool that is
# always worth a word, tight or not.
#
# `helix-claim jobs` is make's -j: with a pool live it is the whole pool. A
# non-make runner sized from it (docker --cpus, ninja -j, parallel -j, xargs -P,
# idf.py, bats --jobs) runs that many jobs on top of every build, so that is
# always named, with or without a pool.
#
# Runs on EVERY Bash call: a command matching no heavy word returns before
# touching jq, /proc or helix-claim. Never ssh from here.
#
# Env:
#   HELIX_ADVISOR_HOSTS       hosts the advice is about (default: thelio); elsewhere it is silent
#   HELIX_ADVISOR_JOBS_CMD    command printing the `jobs -v` line (default: helix-claim jobs);
#                             tests stub the pool here, so they never read the real one
#   HELIX_ADVISOR_MIN_FREE    free pool tokens at or below which thelio is tight (default 8)
#   HELIX_ADVISOR_MIN_GB      availGB below which thelio is tight (default 16)
#   HELIX_ADVISOR_TEST_HOST_RUN    test-host-run.sh whose modes are offered (default: beside this script)

input=$(cat)

# Fast path: a plain substring test on the raw JSON, no parsing.
case "$input" in
    *addr2line*|*gdb*|*"helix-claim jobs"*|*llvm-symbolizer*|*full-test-run*|*unit-sweep*|*mutate*|*asan*|*SANITIZE*|*docker*|*-j*|*helix-tests*|*bats*|*parallel*|*ninja*|*idf.py*) ;;
    *) exit 0 ;;
esac

# The advice is about thelio and its test host; a cloud session or a Mac has neither.
host=$(hostname -s 2>/dev/null)
case " ${HELIX_ADVISOR_HOSTS:-thelio} " in
    *" $host "*) ;;
    *) exit 0 ;;
esac

cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // empty' 2>/dev/null) || exit 0
[ -n "$cmd" ] || exit 0
# bash regex matching goes quadratic on long input; a heavy command names itself early.
cmd=${cmd:0:8192}

case "$cmd" in
    # Already on the test host, or on some other box over ssh.
    *test-host-run.sh*|"ssh "*) exit 0 ;;
esac

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
host_run=${HELIX_ADVISOR_TEST_HOST_RUN:-"$here/test-host-run.sh"}
see_load="\`scripts/helix-claim resources\` shows what thelio and the test host are running now."

emit() {
    jq -cn --arg c "[resource-advisor] $1" \
        '{hookSpecificOutput: {hookEventName: "PreToolUse", additionalContext: $c}}'
    exit 0
}

# A `$(helix-claim jobs)` substitution, bare or as the whole of a double-quoted
# word, becomes $CLAIMJOBS so the runner it sizes can be named. Other quoted
# text becomes Q, so `-j"$(nproc)"` reads as -jQ (a computed -j) and a quoted
# mention of a target, or of the substitution inside a sentence, reads as nothing.
# shellcheck disable=SC2016  # $CLAIMJOBS is literal marker text
masked=$(printf '%s' "$cmd" | sed -E \
    -e 's/"\$\([^()"]*helix-claim[[:space:]]+jobs[[:space:]]*\)"/$CLAIMJOBS/g' \
    -e 's/\$\([^()"]*helix-claim[[:space:]]+jobs[[:space:]]*\)/$CLAIMJOBS/g' \
    -e "s/'[^']*'/Q/g; s/\"[^\"]*\"/Q/g")
segments=$(printf '%s\n' "$masked" | sed -E 's/(&&|\|\||[;|])/\n/g')

# Patterns live in variables: `;|&(` inside a literal [[ =~ ]] break the parse.
re_word_sep='[[:space:]]'
re_sweep_target='(^|[[:space:]])(full-test-run|unit-sweep)([[:space:]]|$)'
re_asan='(^|[[:space:]])(test-asan|SANITIZE=address)([[:space:]]|$)'
re_remote='(-docker|deploy-|remote-)'
re_container_target='(^|[[:space:]])([A-Za-z0-9_.-]+-docker|docker-[A-Za-z0-9_.-]+)([[:space:]]|$)'
re_jobs='(^|[[:space:]])(-j[[:space:]]*|--jobs[=[:space:]]*)([0-9]+|\$\(nproc\)|Q|\$[A-Za-z(]|)([[:space:]]|$)'

want_symbolizer="" want_gdb="" want_mutate="" want_sweep="" want_asan=""
want_container="" want_loop="" jobs_asked="" in_loop="" unpooled="" claim_sized=""

while IFS= read -r seg; do
    # Leading keywords, env assignments and wrappers do not name the program.
    seg=${seg#"${seg%%[![:space:]]*}"}
    while :; do
        case "$seg" in
            do\ *|then\ *|else\ *|\{\ *|\(*) seg=${seg#*[ (]} ;;
            [A-Za-z_]*=*\ *)
                if [[ "${seg%% *}" == *=* ]]; then seg=${seg#* }; else break; fi ;;
            nice\ -n\ *) seg=${seg#nice -n } ; seg=${seg#* } ;;
            nice\ *|time\ *|command\ *|exec\ *|sudo\ *) seg=${seg#* } ;;
            timeout\ *) seg=${seg#timeout }; seg=${seg#* } ;;
            *) break ;;
        esac
        seg=${seg#"${seg%%[![:space:]]*}"}
    done
    word=${seg%%[[:space:]]*}
    base=${word##*/}
    # shellcheck disable=SC2016  # literal marker text
    if [[ "$seg" == *'$CLAIMJOBS'* ]]; then
        case "$base" in
            docker) [[ "$seg" == *--cpus* ]] && claim_sized="docker --cpus" ;;
            ninja) claim_sized="ninja -j" ;;
            parallel) claim_sized="GNU parallel -j" ;;
            xargs) [[ "$seg" == *-P* ]] && claim_sized="xargs -P" ;;
            idf.py) claim_sized="idf.py" ;;
            bats) claim_sized="bats --jobs" ;;
        esac
    fi
    case "$base" in
        for|while|until) in_loop=1 ;;
        make)
            if [[ "$seg" =~ $re_sweep_target ]]; then
                [ "${BASH_REMATCH[2]}" = unit-sweep ] && want_sweep=sweep || want_sweep=full
            fi
            if [[ "$seg" =~ mutate-diff ]]; then want_mutate=1; fi
            if [[ "$seg" =~ $re_asan ]] && ! [[ "$seg" =~ $re_remote ]]; then want_asan=1; fi
            if [[ "$seg" =~ $re_container_target ]]; then want_container=1; fi
            if [[ "$seg" =~ $re_jobs ]]; then
                # '$(nproc)' below is the literal text a caller typed, not an expansion.
                # shellcheck disable=SC2016
                case "${BASH_REMATCH[3]}" in
                    '$(nproc)') jobs_asked=$(nproc 2>/dev/null || echo 32) ;;
                    # A bare -j takes the Makefile's own bound; a computed one is the caller's.
                    ''|Q|\$*) ;;
                    *) jobs_asked=${BASH_REMATCH[3]} ;;
                esac
            fi
            ;;
        mutate_diff.py) want_mutate=1 ;;
        gdb)
            [[ "$seg" == *helix-tests* || "$seg" == *helix-screen* ]] && want_gdb=1 ;;
        addr2line|eu-addr2line|llvm-symbolizer|llvm-addr2line)
            [[ "$seg" == *helix-* ]] && want_symbolizer=1 ;;
        docker)
            if [[ "$seg" =~ ${re_word_sep}run${re_word_sep} && "$seg" == *idf* ]]; then
                want_container=1 unpooled="a docker run of idf.py"
            fi ;;
        helix-tests|bats)
            [ -n "$in_loop" ] && want_loop=1
            if [ "$base" = bats ] && [[ "$seg" =~ $re_jobs ]]; then unpooled="bats --jobs"; fi ;;
        xargs|parallel)
            [[ "$seg" == *helix-tests* || "$seg" == *bats* ]] && want_loop=1
            [ "$base" = parallel ] && unpooled="GNU parallel" ;;
        ninja) unpooled=ninja ;;
        idf.py) [[ "$seg" == *build* ]] && unpooled="idf.py build" ;;
    esac
done <<< "$segments"

# --- Always -----------------------------------------------------------------

if [ -n "$claim_sized" ]; then
    # shellcheck disable=SC2016  # literal text for the reader to copy
    emit "\`helix-claim jobs\` is make's -j: with a jobpool live it is the whole pool, so ${claim_sized} sized from it runs that many jobs on top of every build. Size it from its own pool share: \`scripts/helix-claim hold [--min M] -- sh -c '<cmd> -j \"\$JOBPOOL_SLOTS\"'\`, or for a container \`scripts/pool-docker.sh docker run ...\`."
fi

# --- Always on the test host -----------------------------------------------

if [ -n "$want_gdb" ]; then
    emit "gdb against a helix binary takes ~35 min and tens of GB on thelio. \`coredumpctl info <pid>\` symbolizes a core in seconds; anything more goes to the test host: build and run gdb there in one go: \`scripts/test-host-run.sh exec --build -- gdb -batch ...\` (it mirrors this tree; \`exec --build -- true\` builds alone)."
fi
if [ -n "$want_symbolizer" ]; then
    emit "addr2line loads the whole DWARF of helix-tests per call (21GB+ RSS on thelio) and ignores SIGTERM. Symbolize on the test host: build there (\`scripts/test-host-run.sh exec --build -- llvm-symbolizer ...\` mirrors this tree) and use llvm-symbolizer or one \`gdb -batch -ex 'info symbol 0x..'\` there. For a function name alone, \`nm -C --defined-only\` plus a sorted lookup is seconds."
fi
if [ -n "$want_mutate" ]; then
    emit "Mutation rebuilds and reruns the suite per hunk. Run it on the test host: \`scripts/test-host-run.sh mutate --tests '[tag]'\` (mutate runs the pushed commit: push the branch, never main)."
fi
if [ -n "$want_asan" ]; then
    emit "ASAN produces no output on thelio (ld.so.preload loads its runtime second) and exits 0. Run it on the test host: \`scripts/test-host-run.sh asan '[tag]'\` (it mirrors this tree, uncommitted edits included)."
fi
# full-test-run sends its own sweep to the test host when one is configured and
# automatic offload is on (scripts/full-test-run.sh); advice to move it would
# only move it twice. A bare unit-sweep, or a gate kept local, still gets it.
if [ "$want_sweep" = full ] && ! [[ "$cmd" =~ TEST_HOST=0 ]]; then
    # shellcheck source-path=SCRIPTDIR source=lib/build_hosts.sh
    . "$here/lib/build_hosts.sh" 2>/dev/null
    [ -n "${HELIX_TEST_HOST:-}" ] && [ "${HELIX_TEST_HOST_AUTO:-1}" != 0 ] && exit 0
fi
if [ -n "$want_sweep" ]; then
    if grep -qE '^[[:space:]]*sweep\)' "$host_run" 2>/dev/null; then
        bats=""
        [ "$want_sweep" = full ] && bats=" Run the bats half on thelio with \`make test-shell\`: the test container runs as root with no shellcheck, so bats fails there for reasons that are not the code."
        emit "A full sweep starts dozens of shards at once. Run it on the test host: \`scripts/test-host-run.sh sweep\` (it mirrors this tree, uncommitted edits included).${bats} ${see_load}"
    fi
fi

# --- Needs the box ---------------------------------------------------------

# Reading the box costs a pgrep sweep; skip it when nothing below could fire.
[ -n "$jobs_asked$want_container$want_loop$unpooled" ] || exit 0

# jobs sweeps /proc; a hang here would stall the command until the hook times out.
if [ -n "${HELIX_ADVISOR_JOBS_CMD:-}" ]; then
    line=$(timeout 2 "$HELIX_ADVISOR_JOBS_CMD" -v 2>&1 >/dev/null) || exit 0
else
    line=$(timeout 2 "$here/helix-claim" jobs -v 2>&1 >/dev/null) || exit 0
fi
share=$(printf '%s' "$line" | sed -n 's/.*-> -j\([0-9][0-9]*\).*/\1/p')
avail=$(printf '%s' "$line" | sed -n 's/.*availGB=\([0-9][0-9]*\).*/\1/p')
[ -n "$share" ] && [ -n "$avail" ] || exit 0
# A live pool: `pool target=T available=F ...`.
free=$(printf '%s' "$line" | sed -n 's/^pool .*available=\([0-9][0-9]*\).*/\1/p')

if [ -z "$free" ] && [ -n "$jobs_asked" ] && [ "$jobs_asked" -gt "$share" ]; then
    emit "-j${jobs_asked} is above the -j${share} this box takes (${avail}GB available). Use plain \`make\`, or \`-j\$(scripts/helix-claim jobs)\`."
fi

if [ -n "$free" ] && [ -n "$unpooled" ]; then
    # shellcheck disable=SC2016  # literal text for the reader to copy
    emit "${unpooled} sizes itself to the machine and runs outside the jobpool (${free} of ${share} tokens free). Hold its share instead: \`scripts/helix-claim hold -- sh -c '<cmd> -j \"\$JOBPOOL_SLOTS\"'\`, or for a container \`scripts/pool-docker.sh docker run ...\` (it sets IDF_PY_BUILD_JOBS too)."
fi

min_free=${HELIX_ADVISOR_MIN_FREE:-8}
min_gb=${HELIX_ADVISOR_MIN_GB:-16}
if [ "$avail" -lt "$min_gb" ]; then
    tight="thelio has ${avail}GB available"
elif [ -n "$free" ] && [ "$free" -le "$min_free" ]; then
    tight="the build pool has ${free} of ${share} tokens free"
else
    exit 0
fi

if [ -n "$want_container" ]; then
    emit "Container builds are heavy, and ${tight}. Run it on the test host (it has the Docker images and the RAM), or wait for peers' builds to finish. ${see_load}"
fi
if [ -n "$want_loop" ]; then
    emit "A loop over the test binary multiplies its load, and ${tight}. Cut the count, or run the loop on the test host, in its container. ${see_load}"
fi

exit 0
