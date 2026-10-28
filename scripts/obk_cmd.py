"""Send one or more OpenBeken console commands over HTTP (/cm?cmnd=...).

Example:
  python scripts/obk_cmd.py 192.168.88.239 "SetPinRole 7 Rel" "SetPinChannel 7 0"
"""
import sys
import urllib.parse
import urllib.request


def main():
    ip = sys.argv[1]
    for cmd in sys.argv[2:]:
        url = f"http://{ip}/cm?cmnd={urllib.parse.quote(cmd)}"
        with urllib.request.urlopen(url, timeout=5) as r:
            print(f"{cmd!r}: HTTP {r.status} {r.read().decode(errors='ignore')[:200]}")


if __name__ == "__main__":
    main()
