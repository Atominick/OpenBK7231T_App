"""Analyze power-fail events in a BL0937FastLog CSV recorded by bl0937_fastlog.py.

For every trip (silence_ms >= 5) it reports, relative to the trip moment:
  - ripple onset: CF1 pulse counts start alternating at 100 Hz (likely contact opening),
  - fall onset: 10 ms pulse rate leaves the steady-state noise band,
  - when the 10 ms pulse rate crossed 90/80/50/20 % of the steady-state rate.
All times have the precision of one log interval (5 ms for BL0937FastLog 5).

Example:
  python scripts/bl0937_powerfail_analyze.py scripts/powerfail.csv
"""
import csv
import statistics
import sys

TRIP_SILENCE_MS = 5
BASELINE_MS = 1000
BASELINE_GAP_MS = 600
LEVELS = (0.9, 0.8, 0.5, 0.2)
RIPPLE_AVG = 4
RIPPLE_THRESHOLD = 1.8
RIPPLE_MAX_QUIET = 6
MIN_STEADY_MS = 1500
DIP_LEVEL = 0.8


def load(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append((int(r["dev_ms"]), int(r["pulses"]), int(r["silence_ms"])))
    return rows


def analyze_event(rows, trip, interval):
    ms = [r[0] for r in rows]
    p = [r[1] for r in rows]
    trip_ms = ms[trip]

    # 10 ms sliding sum hides the 100 Hz ripple of a 50 Hz mains.
    win = max(1, round(10 / interval))
    rate = [sum(p[max(0, i - win + 1):i + 1]) for i in range(len(p))]

    base_idx = [i for i in range(trip) if trip_ms - BASELINE_GAP_MS - BASELINE_MS <= ms[i] < trip_ms - BASELINE_GAP_MS]
    if len(base_idx) < 20:
        return None
    base = statistics.median(rate[i] for i in base_idx)
    noise = max(abs(rate[i] - base) for i in base_idx)
    ripple_base = statistics.mean(abs(p[i] - p[i - 1]) for i in base_idx if i > 0)

    def last_index_before_trip(cond):
        i = trip - 1
        while i > base_idx[-1] and not cond(i):
            i -= 1
        return i

    res = {"trip_ms": trip_ms, "base": base, "noise": noise, "ripple_base": ripple_base}
    onset = last_index_before_trip(lambda i: rate[i] >= base - noise)
    res["fall"] = ms[onset] - trip_ms
    for level in LEVELS:
        i = last_index_before_trip(lambda i, level=level: rate[i] >= base * level)
        res[level] = ms[i] + interval - trip_ms

    diff = [0] + [abs(p[i] - p[i - 1]) for i in range(1, len(p))]
    ripple = [statistics.mean(diff[max(0, i - RIPPLE_AVG + 1):i + 1]) for i in range(len(p))]
    rip_threshold = max(RIPPLE_THRESHOLD, ripple_base * 2.5)
    rip_start = None
    quiet = 0
    i = onset
    while i > base_idx[-1] and quiet < RIPPLE_MAX_QUIET:
        if ripple[i] >= rip_threshold:
            rip_start = i
            quiet = 0
        else:
            quiet += 1
        i -= 1
    res["ripple"] = None if rip_start is None else ms[rip_start - RIPPLE_AVG + 1] - trip_ms

    lo = max(0, (rip_start or onset) - 6)
    res["rate_seq"] = rate[lo:trip + 1:win]
    return res


def find_dips(rows, interval):
    """Report every drop of the 10 ms pulse rate below DIP_LEVEL, with or without a trip."""
    ms = [r[0] for r in rows]
    p = [r[1] for r in rows]
    win = max(1, round(10 / interval))
    rate = [sum(p[max(0, i - win + 1):i + 1]) for i in range(len(p))]
    base = statistics.median(rate)
    print(f"\n=== dips below {int(DIP_LEVEL * 100)} % of {base:.0f} pulses/10 ms ===")
    i = 0
    n = 0
    while i < len(rows):
        if rate[i] >= base * DIP_LEVEL:
            i += 1
            continue
        start = i
        while start > 0 and rate[start - 1] < base - 1:
            start -= 1
        end = i
        while end < len(rows) and rate[end] < base * DIP_LEVEL:
            end += 1
        # Group stray-pulse flicker after a long loss into the same dip.
        while end < len(rows):
            nxt = next((k for k in range(end, min(len(rows), end + int(MIN_STEADY_MS / interval)))
                        if rate[k] < base * DIP_LEVEL), None)
            if nxt is None:
                break
            end = nxt
            while end < len(rows) and rate[end] < base * DIP_LEVEL:
                end += 1
        recover = end
        while recover < len(rows) and rate[recover] < base - 1:
            recover += 1
        seg = rows[start:end]
        n += 1
        tripped = any(r[2] >= TRIP_SILENCE_MS for r in seg)
        first_trip = next((r[0] for r in seg if r[2] >= TRIP_SILENCE_MS), None)
        low = min(rate[start:end])
        rec_ms = ms[recover] if recover < len(rows) else None
        print(f"#{n} fall {ms[start]} ms: min {low} ({low / base * 100:.0f} %), "
              f"below {int(DIP_LEVEL * 100)} % for {ms[end - 1] - ms[i] + interval:.0f} ms, "
              f"back to normal after {('%.0f ms' % (rec_ms - ms[start])) if rec_ms else '-'}, "
              + (f"TRIP after {first_trip - ms[start]} ms" if tripped else "no trip"))
        seq = [str(x) for x in rate[max(0, start - 2):min(len(rate), recover + 3):win]]
        if len(seq) > 40:
            seq = seq[:18] + ["..."] + seq[-15:]
        print(f"    10ms rate: {' '.join(seq)}")
        i = max(recover, end) + 1
    if n == 0:
        print("none")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "scripts/powerfail.csv"
    rows = load(path)
    if len(rows) < 2:
        print("Not enough rows")
        return
    interval = statistics.median(b[0] - a[0] for a, b in zip(rows, rows[1:]))
    gaps = [(a[0], b[0] - a[0]) for a, b in zip(rows, rows[1:]) if b[0] - a[0] > interval * 1.5]
    print(f"{path}: {len(rows)} rows, interval {interval:.0f} ms, log gaps: {len(gaps)}\n")

    events = []
    retrips = []
    tripped = False
    last_trip_ms = None
    for i, (ms, pulses, silence) in enumerate(rows):
        if silence >= TRIP_SILENCE_MS and not tripped:
            tripped = True
            # Stray CF1 pulses after a mains loss end the trip for a moment; such
            # re-trips are not new mains losses.
            is_retrip = last_trip_ms is not None and ms - last_trip_ms < MIN_STEADY_MS
            last_trip_ms = ms
            if is_retrip:
                retrips.append(ms)
                continue
            near_gap = [g for g in gaps if ms - 1500 <= g[0] <= ms]
            res = analyze_event(rows, i, interval)
            if res is None:
                print(f"trip at {ms} ms: not enough history\n")
                continue
            events.append(res)
            n = len(events)
            print(f"#{n} trip at {ms} ms  (steady {res['base']:.0f} pulses/10 ms, noise ±{res['noise']:.0f})"
                  + ("  LOG GAP NEAR EVENT" if near_gap else ""))
            print(f"    ripple onset  : {res['ripple'] if res['ripple'] is not None else '-'} ms")
            print(f"    fall onset    : {res['fall']} ms")
            for level in LEVELS:
                print(f"    rate < {int(level * 100):>2} %   : {res[level]:.0f} ms   "
                      f"(+{res[level] - res['fall']:.0f} ms after fall onset)")
            print(f"    10ms rate     : {' '.join(str(x) for x in res['rate_seq'])}\n")
        elif silence < TRIP_SILENCE_MS and tripped:
            tripped = False
            last_trip_ms = ms

    if retrips:
        print(f"Re-trips after stray CF1 pulses (ignored): {len(retrips)}")
    find_dips(rows, interval)
    if not events:
        print("No power-fail events found")
        return

    print(f"\n=== {len(events)} events, ms after fall onset (min / median / max) ===")

    def stat(values):
        return f"{min(values):.0f} / {statistics.median(values):.0f} / {max(values):.0f}"

    for level in LEVELS:
        print(f"  rate < {int(level * 100):>2} %: {stat([e[level] - e['fall'] for e in events])}")
    print(f"  trip      : {stat([-e['fall'] for e in events])}")
    rip = [e["fall"] - e["ripple"] for e in events if e["ripple"] is not None]
    if rip:
        print(f"  ripple onset before fall onset: {stat(rip)}  ({len(rip)} of {len(events)} events)")


if __name__ == "__main__":
    main()
