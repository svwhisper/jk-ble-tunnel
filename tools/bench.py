#!/usr/bin/env python3
"""bench.py — drive the test board's TCP control channel (see ctl_server.h).

Usage:
    tools/bench.py <board-ip> [port]      # interactive; type commands, see EVT lines
    echo "role app" | tools/bench.py <ip> # one-shot from a pipe

Mac-native, stdlib only. Reads your typed lines and the board's event lines
concurrently so you see notifications as they arrive.
"""
import socket
import sys
import threading

def reader(sock):
    buf = b""
    while True:
        try:
            data = sock.recv(4096)
        except OSError:
            break
        if not data:
            print("\n[board closed connection]")
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            print(line.decode(errors="replace"))

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
    with socket.create_connection((host, port), timeout=10) as s:
        threading.Thread(target=reader, args=(s,), daemon=True).start()
        try:
            for line in sys.stdin:
                s.sendall(line.rstrip("\n").encode() + b"\n")
        except KeyboardInterrupt:
            pass

if __name__ == "__main__":
    main()
