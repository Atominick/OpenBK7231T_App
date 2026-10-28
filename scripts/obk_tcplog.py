"""Stream the OpenBeken TCP log (port 9000) to the console and a file.

Example:
  python scripts/obk_tcplog.py 192.168.88.244 --duration 600 --grep "BL0937|Relay|Sentinel|CHANNEL_Set"
"""
import argparse
import re
import socket
import time


def main():
    p = argparse.ArgumentParser()
    p.add_argument("ip")
    p.add_argument("--port", type=int, default=9000)
    p.add_argument("--duration", type=float, default=0, help="seconds, 0 = until Ctrl+C")
    p.add_argument("--grep", help="regex; print and save only matching lines")
    p.add_argument("--out", default="scripts/obk_tcplog.txt")
    args = p.parse_args()
    pattern = re.compile(args.grep) if args.grep else None

    started = time.time()
    buf = b""
    with socket.create_connection((args.ip, args.port), timeout=5) as s, \
            open(args.out, "a", encoding="utf-8") as f:
        s.settimeout(1)
        print(f"Logging {args.ip}:{args.port} -> {args.out}", flush=True)
        try:
            while not args.duration or time.time() - started < args.duration:
                try:
                    chunk = s.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    print("connection closed by device", flush=True)
                    break
                buf += chunk
                while b"\n" in buf:
                    raw, _, buf = buf.partition(b"\n")
                    line = raw.decode(errors="ignore").rstrip()
                    if pattern and not pattern.search(line):
                        continue
                    stamped = f"{time.strftime('%H:%M:%S')} {line}"
                    print(stamped, flush=True)
                    f.write(stamped + "\n")
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
