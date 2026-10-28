"""Stop and restart the sentinel beacon and measure how the guarded relay follows.

Example:
  python scripts/sentinel_guard_test.py 192.168.88.239 192.168.88.244 --off 4
"""
import argparse
import json
import threading
import time
import urllib.parse
import urllib.request


def cmd(ip, command):
    url = f"http://{ip}/cm?cmnd={urllib.parse.quote(command)}"
    urllib.request.urlopen(url, timeout=3).read()


def relay(ip):
    try:
        raw = urllib.request.urlopen(f"http://{ip}/api/channels", timeout=1).read().decode()
        return json.loads(raw).get("0")
    except Exception:
        return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("sentinel")
    p.add_argument("guard")
    p.add_argument("--off", type=float, default=4, help="seconds without beacon")
    p.add_argument("--channel", default="0")
    args = p.parse_args()

    events = []
    stop = threading.Event()

    def poll():
        prev = object()
        while not stop.is_set():
            v = relay(args.guard)
            t = time.time()
            if v != prev:
                events.append((t, f"relay ch{args.channel} = {v}"))
                prev = v
            time.sleep(0.1)

    th = threading.Thread(target=poll, daemon=True)
    th.start()
    time.sleep(1.5)
    events.append((time.time(), "SentinelBeacon 0 sent"))
    cmd(args.sentinel, "SentinelBeacon 0")
    time.sleep(args.off)
    events.append((time.time(), "SentinelBeacon 200 sent"))
    cmd(args.sentinel, "SentinelBeacon 200")
    time.sleep(4)
    stop.set()
    th.join()

    t0 = next(t for t, e in events if e.startswith("SentinelBeacon 0"))
    for t, e in sorted(events):
        print(f"{(t - t0) * 1000:+8.0f} ms  {e}")


if __name__ == "__main__":
    main()
