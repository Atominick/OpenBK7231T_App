"""Listen for OpenBeken Sentinel UDP beacons (port 4451) and report their timing.

Example:
  python scripts/sentinel_listen.py --duration 10
"""
import argparse
import socket
import statistics
import time


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=4451)
    p.add_argument("--duration", type=float, default=10)
    args = p.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", args.port))
    s.settimeout(0.5)
    started = time.time()
    last = {}
    gaps = {}
    while time.time() - started < args.duration:
        try:
            data, (ip, _) = s.recvfrom(64)
        except socket.timeout:
            continue
        now = time.time()
        if ip in last:
            gaps.setdefault(ip, []).append((now - last[ip]) * 1000)
        else:
            print(f"first beacon from {ip}: {data!r}", flush=True)
        last[ip] = now
    for ip, g in gaps.items():
        print(f"{ip}: {len(g) + 1} beacons, interval ms min/median/max "
              f"{min(g):.0f}/{statistics.median(g):.0f}/{max(g):.0f}")
    if not last:
        print("no beacons received")


if __name__ == "__main__":
    main()
