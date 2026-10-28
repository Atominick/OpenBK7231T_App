"""Find OpenBeken devices on a /24 subnet via their /api/info endpoint.

Example:
  python scripts/obk_scan.py 192.168.88
"""
import concurrent.futures
import json
import sys
import urllib.request


def probe(ip):
    try:
        with urllib.request.urlopen(f"http://{ip}/api/info", timeout=1.5) as r:
            return ip, json.loads(r.read().decode(errors="ignore"))
    except Exception:
        return ip, None


def main():
    prefix = sys.argv[1] if len(sys.argv) > 1 else "192.168.88"
    ips = [f"{prefix}.{i}" for i in range(1, 255)]
    with concurrent.futures.ThreadPoolExecutor(64) as ex:
        for ip, info in ex.map(probe, ips):
            if info:
                print(f"{ip:16} {info.get('shortName', '?'):20} {info.get('chipset', '?'):8} "
                      f"up {info.get('uptime_s', '?')} s  {info.get('build', '')}")


if __name__ == "__main__":
    main()
