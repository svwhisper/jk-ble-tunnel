#!/usr/bin/env python3
"""bench.py — drive the test board's SERIAL control channel (see ctl_server.h).

The bench board is USB-tethered and has no WiFi; commands go over its serial
port. Usage:

    tools/bench.py <serial-port> [baud]        # interactive
    tools/bench.py /dev/cu.wchusbserial120      # (macOS CH340 UART port)

Type commands (role app | role bms <name> | scan | connect <name> | sub | read
| write <hex> | disconnect | push | autopush <ms>); the board's OK/ERR/EVT lines
and logs stream back. Ctrl-C to quit.

Needs pyserial (`pip install pyserial`). Alternatively just use
`idf.py -C test_board -p <port> monitor` and type commands into it.
"""
import sys
import threading

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not found — `pip install pyserial`, or use `idf.py monitor`")


def reader(s):
    while True:
        try:
            line = s.readline()
        except OSError:
            break
        if line:
            sys.stdout.write(line.decode(errors="replace"))
            sys.stdout.flush()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    port = sys.argv[1]
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
    with serial.Serial(port, baud, timeout=1) as s:
        threading.Thread(target=reader, args=(s,), daemon=True).start()
        try:
            for line in sys.stdin:
                s.write(line.rstrip("\n").encode() + b"\n")
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
