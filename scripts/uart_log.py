"""Record a serial port (device UART log) to the console and a file.

Example:
  python scripts/uart_log.py COM4 --baud 115200 --duration 180 --out scripts/uart_log.txt
"""
import argparse
import time

import serial


def main():
    p = argparse.ArgumentParser()
    p.add_argument("port")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--duration", type=float, default=0, help="seconds, 0 = until Ctrl+C")
    p.add_argument("--out", default="scripts/uart_log.txt")
    args = p.parse_args()

    started = time.time()
    ser = None
    buf = b""
    with open(args.out, "a", encoding="utf-8") as f:
        print(f"Recording {args.port}@{args.baud} -> {args.out}", flush=True)
        try:
            while not args.duration or time.time() - started < args.duration:
                if ser is None:
                    # The USB adapter may disappear while the board is power-cycled.
                    try:
                        ser = serial.Serial(args.port, args.baud, timeout=0.5)
                        print(f"{time.time() - started:8.3f} [port opened]", flush=True)
                    except serial.SerialException:
                        time.sleep(0.5)
                        continue
                try:
                    buf += ser.read(4096)
                except serial.SerialException:
                    print(f"{time.time() - started:8.3f} [port lost, reconnecting]", flush=True)
                    ser.close()
                    ser = None
                    continue
                while b"\n" in buf:
                    raw, _, buf = buf.partition(b"\n")
                    line = f"{time.time() - started:8.3f} {raw.decode(errors='replace').rstrip()}"
                    print(line, flush=True)
                    f.write(line + "\n")
        except KeyboardInterrupt:
            pass
        if buf:
            f.write(f"{time.time() - started:8.3f} {buf.decode(errors='replace')}\n")
        if ser:
            ser.close()


if __name__ == "__main__":
    main()
