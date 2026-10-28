import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
for baud in (115200, 921600):
    try:
        with serial.Serial(port, baud, timeout=0.5) as s:
            t = time.time()
            data = b""
            while time.time() - t < 3:
                data += s.read(4096)
        print(baud, len(data), repr(data[:300]))
    except Exception as e:
        print(baud, "ERR", e)
