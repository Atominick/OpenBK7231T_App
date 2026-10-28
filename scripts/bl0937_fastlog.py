"""Record BL0937FastLog CSV from an OpenBeken LN882H device over UART or TCP log (port 9000).

Examples:
  python scripts/bl0937_fastlog.py --list-ports
  python scripts/bl0937_fastlog.py --ip 192.168.1.50 --tcp
  python scripts/bl0937_fastlog.py --mqtt-host localhost --topic plug1 --tcp 192.168.1.50
  python scripts/bl0937_fastlog.py --ip 192.168.1.50 --port COM5
  python scripts/bl0937_fastlog.py --mqtt-host localhost --topic plug1 --port COM5 --interval 20
"""
import argparse
import socket
import sys
import time

import serial
import serial.tools.list_ports


def send_command(args, cmd):
    if args.ip:
        import requests
        r = requests.get(f"http://{args.ip}/cm", params={"cmnd": cmd}, timeout=3)
        print(f"HTTP {r.status_code}: {cmd}")
    elif args.mqtt_host:
        import paho.mqtt.client as mqtt
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        if args.mqtt_user:
            c.username_pw_set(args.mqtt_user, args.mqtt_pass)
        c.connect(args.mqtt_host, args.mqtt_port)
        c.loop_start()
        name, _, payload = cmd.partition(" ")
        c.publish(f"cmnd/{args.topic}/{name}", payload).wait_for_publish()
        c.loop_stop()
        c.disconnect()
        print(f"MQTT cmnd/{args.topic}/{name} = {payload}")
    else:
        print(f"No --ip or --mqtt-host: send manually: {cmd}")


class TcpLog:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(1)
        self.buf = b""

    def readline(self):
        while b"\n" not in self.buf:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return b""
            if not chunk:
                raise ConnectionError("TCP log closed by device")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.sock.close()


def open_source(args):
    if args.tcp:
        print(f"Reading TCP log {args.tcp}:{args.tcp_port}")
        return TcpLog(args.tcp, args.tcp_port)
    print(f"Reading {args.port}@{args.baud}")
    return serial.Serial(args.port, args.baud, timeout=1)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--list-ports", action="store_true")
    p.add_argument("--port", help="USB-UART port, e.g. COM5")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--interval", type=int, default=10, help="BL0937FastLog interval, ms")
    p.add_argument("--out", default="bl0937.csv")
    p.add_argument("--ip", help="device IP (command via HTTP)")
    p.add_argument("--mqtt-host", help="broker host (command via MQTT)")
    p.add_argument("--mqtt-port", type=int, default=1883)
    p.add_argument("--mqtt-user")
    p.add_argument("--mqtt-pass")
    p.add_argument("--topic", default="obk", help="device Client Topic")
    p.add_argument("--tcp", nargs="?", const="", help="read device TCP log instead of UART (host, default --ip)")
    p.add_argument("--tcp-port", type=int, default=9000)
    p.add_argument("--duration", type=float, default=0, help="stop after N seconds (0 = until Ctrl+C)")
    args = p.parse_args()

    if args.tcp == "":
        args.tcp = args.ip
        if not args.tcp:
            p.error("--tcp needs a host or --ip")

    if args.list_ports or not (args.port or args.tcp):
        ports = list(serial.tools.list_ports.comports())
        print("Serial ports:")
        for port in ports:
            print(f"  {port.device}: {port.description}")
        if not ports:
            print("  (none)")
        if not (args.port or args.tcp):
            print("\nSpecify --port or --tcp to start recording.")
        return

    try:
        ser = open_source(args)
    except (OSError, serial.SerialException) as e:
        print(f"Cannot open log source: {e}")
        sys.exit(1)

    send_command(args, f"BL0937FastLog {args.interval}")

    prev_ms = None
    rows = gaps = trips = 0
    tripped = False
    started = time.time()
    try:
        with ser, open(args.out, "w") as f:
            f.write("dev_ms,voltage,pulses,silence_ms\n")
            print(f"Recording -> {args.out}, Ctrl+C to stop")
            while not args.duration or time.time() - started < args.duration:
                line = ser.readline().decode(errors="ignore").strip()
                idx = line.find("BL0937,")
                if idx < 0:
                    continue
                parts = line[idx:].split(",")
                if len(parts) != 5:
                    continue
                try:
                    ms, v, pulses, silence = int(parts[1]), float(parts[2]), int(parts[3]), int(parts[4])
                except ValueError:
                    continue
                f.write(f"{ms},{v},{pulses},{silence}\n")
                rows += 1
                if prev_ms is not None and ms - prev_ms > args.interval * 1.5:
                    gaps += 1
                    print(f"! gap {ms - prev_ms} ms at {ms}")
                if silence >= 5 and not tripped:
                    trips += 1
                    print(f">>> power-fail at {ms} ms, V={v:.1f}, pulses={pulses}")
                elif silence < 5 and tripped:
                    print(f"<<< power back at {ms} ms, V={v:.1f}")
                tripped = silence >= 5
                if rows % max(1, 1000 // args.interval) == 0:
                    print(f"{ms} ms  V={v:.1f}  pulses={pulses}")
                prev_ms = ms
    except KeyboardInterrupt:
        pass
    except (OSError, serial.SerialException) as e:
        print(f"Log source error: {e}")
    finally:
        try:
            send_command(args, "BL0937FastLog 0")
        except Exception as e:
            print(f"Failed to disable fast log: {e}")
        dur = time.time() - started
        print(f"Rows: {rows}, gaps: {gaps}, power-fail events: {trips}, {dur:.1f} s")


if __name__ == "__main__":
    main()
