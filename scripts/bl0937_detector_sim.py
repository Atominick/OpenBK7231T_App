"""Offline simulation of a fast mains-interruption detector on BL0937FastLog CSV files.

Idea: inside BL0937 the CF1 rate follows a ~100 ms moving average of the input, so a full
interruption makes the rate fall at the maximum possible slope (~1 % per ms). A sag with
residual voltage falls proportionally slower. The detector trips when the short-window rate
drops by DROP of a frozen baseline within SLOPE_MS after it left the baseline band.

Example:
  python scripts/bl0937_detector_sim.py scripts/powerfail2.csv scripts/avr.csv
"""
import csv
import statistics
import sys

BASE_MS = 200       # baseline averaging window
BASE_GAP_MS = 60    # baseline ends this long before the current sample
EVENT_LEVEL = 0.8   # ground truth: a real event dips below this fraction of baseline
MIN_BASE = 12       # pulses/10 ms (~160 V): below this the detector is not armed


def load(path):
    with open(path, newline="", encoding="utf-8") as f:
        return [(int(r["dev_ms"]), int(r["pulses"])) for r in csv.DictReader(f)]


def simulate(rows, interval, win_ms, drop, slope_ms):
    ms = [r[0] for r in rows]
    p = [r[1] for r in rows]
    win = max(1, round(win_ms / interval))
    rate = [sum(p[max(0, i - win + 1):i + 1]) * (10 / (win * interval)) for i in range(len(p))]
    nb = round(BASE_MS / interval)
    ng = round(BASE_GAP_MS / interval)
    ns = max(1, round(slope_ms / interval))

    base = None
    trips = []
    hold_until = -1
    for i in range(nb + ng, len(p)):
        if i > hold_until:
            seg = rate[i - ng - nb:i - ng]
            # freeze the baseline while a disturbance is in progress or mains is low
            if max(seg) - min(seg) <= 3 and min(seg) >= MIN_BASE:
                base = statistics.mean(seg)
        if base is None:
            continue
        if rate[i] <= base * (1 - drop):
            # slope test: within slope_ms before, the rate was still near baseline
            j = i - ns
            if j >= 0 and rate[j] >= base * (1 - drop / 3):
                if not trips or ms[i] - trips[-1] > 1000:
                    trips.append(ms[i])
                hold_until = i + round(1500 / interval)
    return trips


def ground_truth(rows, interval):
    ms = [r[0] for r in rows]
    p = [r[1] for r in rows]
    rate = [p[i] + p[i - 1] if i else 2 * p[0] for i in range(len(p))]
    base = statistics.median(rate)
    events = []
    i = 0
    while i < len(p):
        if rate[i] < base * EVENT_LEVEL:
            s = i
            while s > 0 and rate[s - 1] < base - 1:
                s -= 1
            # event lasts until the rate is back near baseline for 500 ms
            e = i
            ok = 0
            while e < len(p) and ok < round(500 / interval):
                ok = ok + 1 if rate[e] >= base - 2 else 0
                e += 1
            if events and ms[s] - events[-1][1] < 200:
                events[-1] = (events[-1][0], ms[min(e, len(p) - 1)])
            else:
                events.append((ms[s] - interval, ms[min(e, len(p) - 1)]))
            i = e
        else:
            i += 1
    return events


def main():
    files = sys.argv[1:] or ["scripts/powerfail2.csv", "scripts/avr.csv"]
    data = []
    for f in files:
        rows = load(f)
        interval = statistics.median(b[0] - a[0] for a, b in zip(rows, rows[1:]))
        data.append((f, rows, interval, ground_truth(rows, interval)))
        print(f"{f}: {len(rows)} rows ({len(rows) * interval / 1000:.0f} s), "
              f"{len(data[-1][3])} events")

    print(f"\n{'win':>4} {'drop':>5} {'slope':>5} | {'hit':>5} {'miss':>4} {'false':>5} | "
          f"latency ms min/med/max")
    for win_ms in (10, 15, 20):
        for drop in (0.08, 0.10, 0.15, 0.20):
            for slope_ms in (15, 20, 30):
                hits, false, lat = 0, 0, []
                false_at = []
                total = 0
                for f, rows, interval, events in data:
                    trips = simulate(rows, interval, win_ms, drop, slope_ms)
                    total += len(events)
                    used = set()
                    for t in trips:
                        ev = next((e for e in events if e[0] <= t <= e[1]), None)
                        if ev is None:
                            false += 1
                            false_at.append((f, t))
                        elif ev not in used:
                            used.add(ev)
                            lat.append(t - ev[0])
                    hits += len(used)
                ls = f"{min(lat)}/{statistics.median(lat):.0f}/{max(lat)}" if lat else "-"
                print(f"{win_ms:>4} {drop:>5.2f} {slope_ms:>5} | {hits:>5} {total - hits:>4} "
                      f"{false:>5} | {ls}"
                      + (f"   false at {false_at[:3]}" if false_at else ""))


if __name__ == "__main__":
    main()
