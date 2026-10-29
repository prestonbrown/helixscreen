#!/usr/bin/env python3
# Pressure advance from a nozzle force log.
#
# Input: <dir>/<name>_{pressure,extruder,marks}.csv (or .csv.gz), as
# helix_pa_gauge writes them. Marks: 'dry' (the nozzle at rest), then per
# tested PA value 'k<K>' ... 'end<K>'.
#
# Nozzle force is mapped to outflow through the run's own steady levels (rest,
# slow, fast), which absorbs the melt's non-linear force/flow curve. For each
# PA value every slow->fast (up) and fast->slow (down) flow step is overlaid by
# time since the step. The outflow error integrated after up steps minus that
# after down steps rises with PA and crosses zero where outflow follows the
# commanded flow.
#
# Runs in a subprocess of Klipper (pure stdlib, Python 3.7+), so a long fit
# never stalls the reactor. Method after CNC Kitchen's PATuner and
# grant0013/K2-OpenKlipper (GPLv3).
#
# This file may be distributed under the terms of the GNU GPLv3 license.
import bisect
import csv
import gzip
import json
import os
import statistics
import sys

STEP_WIN = 0.45  # seconds scored after each flow step
BIN = 0.005  # seconds per bin of the overlaid step response
PRE_WIN = 0.25  # seconds of steady flow before a step
MIN_STEPS = 8
MIN_SNR = 8.0
MIN_STEP = 20000  # counts between the slow and fast levels
MIN_FILL = 0.3  # fraction of step-response bins that must hold data
MIN_R2 = 0.75
# Decode glitches read as zero or several million counts off; a real flow
# step is a few hundred thousand at most, and its edge must survive.
OUTLIER = 400000
MEDIAN_N = 31


def _rows(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt") as f:
        return list(csv.reader(f))[1:]


def _find(directory, name, part):
    base = os.path.join(directory, "%s_%s.csv" % (name, part))
    return base if os.path.exists(base) else base + ".gz"


def load(directory, name):
    pressure = sorted((float(t), float(v)) for t, v in _rows(_find(directory, name, "pressure")))
    moves = sorted(tuple(float(x) for x in r) for r in _rows(_find(directory, name, "extruder")))
    marks = [(float(t), label) for t, label in _rows(_find(directory, name, "marks"))]
    return pressure, moves, marks


def clean(samples):
    """Samples further than OUTLIER from the running median of their
    neighbours dropped."""
    vals = [v for _, v in samples]
    n = len(vals)
    if n < MEDIAN_N:
        return list(samples)
    half = MEDIAN_N // 2
    window = sorted(vals[:MEDIAN_N])
    kept = []
    for i, (t, v) in enumerate(samples):
        lo, hi = i - half, i + half
        if lo > 0 and hi < n:
            window.pop(bisect.bisect_left(window, vals[lo - 1]))
            bisect.insort(window, vals[hi])
        if abs(v - window[len(window) // 2]) <= OUTLIER:
            kept.append((t, v))
    return kept


def segments(moves):
    """One (start, end, peak rate) per extrusion run: trapq pieces joined
    while the flow never drops to zero between them."""
    out = []
    cur = None
    for pt, mt, sv, ac, _ in moves:
        peak = max(sv, sv + ac * mt)
        if cur is not None and sv > 1e-6 and abs(pt - cur[1]) < 1e-4:
            cur[1] = pt + mt
            cur[2] = max(cur[2], peak)
            continue
        if cur is not None:
            out.append(tuple(cur))
        cur = [pt, pt + mt, peak]
    if cur is not None:
        out.append(tuple(cur))
    return out


def _window(samples, times, t0, t1):
    return [v for _, v in samples[bisect.bisect_left(times, t0):bisect.bisect_left(times, t1)]]


def _flow_map(points):
    """Piecewise-linear force -> flow through (force, flow) points."""
    pts = sorted(points)
    xs = [p[0] for p in pts]

    def f(x):
        j = min(max(bisect.bisect_right(xs, x), 1), len(pts) - 1)
        (x0, y0), (x1, y1) = pts[j - 1], pts[j]
        return y0 + (y1 - y0) * (x - x0) / (x1 - x0) if x1 != x0 else y0
    return f


def _fit_line(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if not sxx:
        return 0., my, 0.
    a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    b = my - a * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (a * x + b)) ** 2 for x, y in zip(xs, ys))
    return a, b, (1 - ss_res / ss_tot) if ss_tot else 0.


def _no_result(problem, **extra):
    out = {"fitted": None, "reliable": False, "problems": [problem], "table": []}
    out.update(extra)
    return out


def analyse(pressure, moves, marks):
    raw_count = len(pressure)
    samples = clean(pressure)
    glitches = raw_count - len(samples)
    times = [t for t, _ in samples]
    segs = [s for s in segments(moves) if s[2] > 0.05]
    mark_t = {label: t for t, label in marks}
    ks = sorted(float(label[1:]) for label in mark_t if label.startswith("k"))
    if len(segs) < 2 or len(ks) < 2 or "dry" not in mark_t:
        return _no_result("no flow steps in the log", glitches=glitches)

    rates = sorted(s[2] for s in segs)
    slow = statistics.median(rates[:len(rates) // 2])
    fast = statistics.median(rates[len(rates) // 2:])
    mid = (slow + fast) / 2.
    steps = []
    for prev, cur in zip(segs, segs[1:]):
        if prev[2] < mid <= cur[2]:
            steps.append((cur[0], "up"))
        elif prev[2] >= mid > cur[2]:
            steps.append((cur[0], "down"))
    if len(steps) < MIN_STEPS:
        return _no_result("only %d flow steps" % len(steps), glitches=glitches)

    rest = _window(samples, times, mark_t["dry"] - 0.9, mark_t["dry"])
    if len(rest) < 50:
        return _no_result("no resting samples before the run", glitches=glitches)
    p_rest = statistics.median(rest)
    # Noise from steady flow just before each step, around that window's own
    # median: the rest window can still hold the decay of a priming purge.
    pre_slow, pre_fast, resid = [], [], []
    for t, kind in steps:
        w = _window(samples, times, t - PRE_WIN, t)
        if len(w) < 10:
            continue
        (pre_slow if kind == "up" else pre_fast).extend(w)
        m = statistics.median(w)
        resid.extend(v - m for v in w)
    if not pre_slow or not pre_fast:
        return _no_result("no steady flow before the steps", glitches=glitches)
    p_slow, p_fast = statistics.median(pre_slow), statistics.median(pre_fast)
    noise = statistics.pstdev(resid)
    step_counts = abs(p_fast - p_slow)
    snr = step_counts / noise if noise else float("inf")
    outflow = _flow_map([(p_rest, 0.), (p_slow, slow), (p_fast, fast)])

    nbins = int(round(STEP_WIN / BIN))
    table = []
    for k in ks:
        t0, t1 = mark_t["k%g" % k], mark_t.get("end%g" % k, float("inf"))
        acc = {"up": [[0., 0] for _ in range(nbins)], "down": [[0., 0] for _ in range(nbins)]}
        for ts, kind in steps:
            if not (t0 <= ts < t1):
                continue
            target = fast if kind == "up" else slow
            i0 = bisect.bisect_left(times, ts)
            i1 = bisect.bisect_left(times, ts + STEP_WIN)
            for t, v in samples[i0:i1]:
                b = min(int((t - ts) / BIN), nbins - 1)
                acc[kind][b][0] += outflow(v) - target
                acc[kind][b][1] += 1
        err, fill = {}, {}
        for kind in ("up", "down"):
            means = [s / n for s, n in acc[kind] if n]
            err[kind] = sum(means) * BIN * nbins / max(len(means), 1)
            fill[kind] = len(means) / nbins
        table.append({"k": k, "up": round(err["up"], 4), "down": round(err["down"], 4),
                      "balance": round(err["up"] - err["down"], 4),
                      "fill": round(min(fill["up"], fill["down"]), 2)})

    a, _, r2 = _fit_line([r["k"] for r in table], [r["balance"] for r in table])
    # Balance is not linear over a wide PA range: take the zero crossing
    # between the two tested values that straddle it.
    fitted = None
    for r0, r1 in zip(table, table[1:]):
        if r0["balance"] <= 0 < r1["balance"]:
            fitted = r0["k"] + (r1["k"] - r0["k"]) * (
                -r0["balance"] / (r1["balance"] - r0["balance"]))
            break
    problems = []
    if snr < MIN_SNR:
        problems.append("signal/noise %.1f < %.0f" % (snr, MIN_SNR))
    if step_counts < MIN_STEP:
        problems.append("flow step %.0f counts < %d" % (step_counts, MIN_STEP))
    if min(r["fill"] for r in table) < MIN_FILL:
        problems.append("step response coverage below %d%%" % (MIN_FILL * 100))
    if any(r1["balance"] < r0["balance"] - 0.1 for r0, r1 in zip(table, table[1:])):
        problems.append("balance does not rise with PA")
    if r2 < MIN_R2:
        problems.append("trend R2 %.2f < %.2f" % (r2, MIN_R2))
    if fitted is None:
        problems.append("result outside the tested range")
    return {"fitted": round(fitted, 4) if fitted is not None else None,
            "reliable": not problems, "problems": problems,
            "r2": round(r2, 3), "slope": round(a, 3), "snr": round(snr, 1),
            "levels": {"rest": round(p_rest), "slow": round(p_slow),
                       "fast": round(p_fast), "noise_sd": round(noise, 1)},
            "flow_mm_s": {"slow": round(slow, 3), "fast": round(fast, 3)},
            "steps": len(steps), "samples": len(samples), "glitches": glitches,
            "table": table}


if __name__ == "__main__":
    print(json.dumps(analyse(*load(sys.argv[1], sys.argv[2]))))
