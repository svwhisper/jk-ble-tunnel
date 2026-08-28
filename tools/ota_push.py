#!/usr/bin/env python3
"""
ota_push.py — self-contained push-OTA updater for the JK BLE tunnel nodes.

It POSTs a freshly-built firmware .bin to a node's always-on OTA receiver
(via curl), then waits for the node to reboot and re-open the receiver so you
get positive confirmation the update took.

    ./ota_push.py a                      # push node_a/build/node_a.bin to Node A
    ./ota_push.py b                      # push node_b/build/node_b.bin to Node B
    ./ota_push.py a --host 192.168.3.241 # if the hostname doesn't resolve (see below)
    ./ota_push.py a --bin some.bin --port 3765 --no-wait

------------------------------------------------------------------------------
FIRMWARE CONTRACT (authoritative implementation: components/ota/ota.c)
------------------------------------------------------------------------------
  Endpoint   : POST http://<node>:<port>/ota      (port default 3765)
  Body       : the raw application .bin (Content-Type: application/octet-stream)
  On success : HTTP 200 "OK: <n> bytes -> ota_X, rebooting"  then the node
               reboots ~1 s later and boots the new slot.
  On failure : HTTP 4xx/5xx with a reason; the running image is left untouched
               (the OTA handle is aborted), so a dropped transfer can't brick it.
  Rollback   : the new image boots in PENDING_VERIFY and only confirms itself
               once WiFi is up (ota_mark_valid). A build that can't reach that
               point auto-reverts on the next reset. The receiver is always
               listening (no arming step); no auth (LAN range = physical access).

Note on this network: the Mac often can't resolve jk-node-*.localdomain via
pfSense DNS. If resolution fails this script says so and you pass --host <ip>
(the node prints its IP on the OLED and in its boot log).
"""
import argparse
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PORT = 3765
NODES = {
    "a": ("jk-node-a.localdomain", "node_a/build/node_a.bin"),
    "b": ("jk-node-b.localdomain", "node_b/build/node_b.bin"),
}
APP_MAGIC = 0xE9  # ESP32 image header first byte


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def resolve(host):
    try:
        return socket.gethostbyname(host)
    except socket.gaierror:
        return None


def port_open(host, port, timeout=1.0):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser(description="Push a firmware .bin to a JK tunnel node over OTA.")
    ap.add_argument("node", choices=sorted(NODES), help="which node: a or b")
    ap.add_argument("--host", help="override hostname/IP (use an IP if the name won't resolve)")
    ap.add_argument("--bin", help="firmware .bin (default: that node's build output)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"OTA port (default {DEFAULT_PORT}; must match CFG_OTA_PORT)")
    ap.add_argument("--timeout", type=int, default=120, help="max seconds for the upload (default 120)")
    ap.add_argument("--no-wait", action="store_true", help="don't wait for the node to reboot and come back")
    args = ap.parse_args()

    host = args.host or NODES[args.node][0]
    binpath = args.bin or os.path.join(REPO, NODES[args.node][1])

    # --- validate the image locally before shipping it ---
    if not os.path.isfile(binpath):
        die(f"firmware not found: {binpath}\n       build it first (idf.py -C node_{args.node} build)")
    size = os.path.getsize(binpath)
    with open(binpath, "rb") as f:
        magic = f.read(1)
    if not magic or magic[0] != APP_MAGIC:
        die(f"{binpath} doesn't look like an ESP32 app image (first byte 0x{magic.hex() if magic else '??'}, "
            f"expected 0x{APP_MAGIC:02x}). Wrong file?")

    # --- resolve / reachability preflight ---
    ip = args.host if (args.host and args.host[0].isdigit()) else resolve(host)
    if ip is None:
        die(f"can't resolve '{host}'. On this LAN pfSense DNS often won't answer the Mac —\n"
            f"       pass the node's IP instead:  {sys.argv[0]} {args.node} --host <ip>")
    if not port_open(ip, args.port):
        die(f"{host} ({ip}) not accepting connections on :{args.port}. Is the node up and on WiFi? "
            f"Is CFG_OTA_PORT == {args.port}?")

    url = f"http://{ip}:{args.port}/ota"
    print(f"pushing {os.path.relpath(binpath, REPO)} ({size:,} bytes) -> {host} ({ip}) :{args.port}")

    # --- push via curl (as requested); --fail-with-body surfaces the node's
    #     error text on a 4xx/5xx while still failing the exit code. ---
    cmd = [
        "curl", "-sS", "--fail-with-body",
        "--max-time", str(args.timeout),
        "-H", "Content-Type: application/octet-stream",
        "-H", "Expect:",                    # avoid a 100-continue stall
        "-w", "\nHTTP %{http_code} in %{time_total}s\n",
        "--data-binary", f"@{binpath}",
        url,
    ]
    rc = subprocess.call(cmd)
    if rc != 0:
        die(f"curl exit {rc} — upload/flash did not succeed (running image unchanged)")

    if args.no_wait:
        print("upload accepted; node is rebooting into the new image.")
        return

    # --- wait for the reboot: the port drops, then re-opens on the new image ---
    print("waiting for reboot", end="", flush=True)
    t0 = time.time()
    # 1) watch it go down (brief; may be missed if the reboot is very fast)
    while time.time() - t0 < 8 and port_open(ip, args.port, 0.5):
        print(".", end="", flush=True)
        time.sleep(0.5)
    # 2) watch it come back up
    back = False
    while time.time() - t0 < 45:
        if port_open(ip, args.port, 0.5):
            back = True
            break
        print(".", end="", flush=True)
        time.sleep(0.5)
    dt = time.time() - t0
    if back:
        print(f"\nback up on :{args.port} after {dt:.0f}s — new image is running and confirmed.")
    else:
        print(f"\nwarning: {host} didn't re-open :{args.port} within {dt:.0f}s.")
        print("         it may have rolled back to the previous image (check the OLED / boot log),")
        print("         or WiFi didn't come back. Re-run to retry.")
        sys.exit(2)


if __name__ == "__main__":
    main()
