#!/usr/bin/env python3
"""
ota_push.py — self-contained push-OTA updater for the JK BLE tunnel nodes.

It POSTs a freshly-built firmware .bin to a node's always-on OTA receiver
(via curl), then verifies the running ELF identity and OTA validation state.
Reachability by itself is never reported as a successful update.

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
  On failure : HTTP 4xx/5xx with a reason; an incomplete transfer is aborted.
               A lost response after boot selection has an uncertain outcome.
  Rollback   : the new image boots in PENDING_VERIFY and only confirms itself
               after bringup with WiFi and OTA ready (ota_mark_valid). A build
               that can't reach that point reverts on the next reset; a hang
               still needs a reset. This is not proof of BLE/application health.
               The receiver is always
               listening (no arming step); no auth (LAN range = physical access).
  Evidence   : GET /ota/status returns elf_sha256, ota_state, uptime_ms.
               Only exact identity + ota_state=2 (VALID) is confirmed.
  Backout    : --bin saved.bin selects the saved app. A legacy saved image
               lacks the status route: use --no-wait and independently check
               boot/application behaviour; never call this verified by the tool.

Note on this network: the Mac often can't resolve jk-node-*.localdomain via
pfSense DNS. If resolution fails this script says so and you pass --host <ip>
(the node prints its IP on the OLED and in its boot log).
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PORT = 3765
NODES = {
    "a": ("jk-node-a.localdomain", "node_a/build/node_a.bin"),
    "b": ("jk-node-b.localdomain", "node_b/build/node_b.bin"),
}
APP_MAGIC = 0xE9  # ESP32 image header first byte
CHIP_IDS = {"a": 9, "b": 5}  # ESP32-S3 / ESP32-C3, installed ESP-IDF image header
OTA_VALID = 2  # esp_ota_img_states_t: only VALID confirms rollback cancelled


def image_identity(path, node):
    """Read the ESP-IDF app descriptor; reject a wrong-target image locally."""
    with open(path, "rb") as f:
        header = f.read(288)  # 24-byte header, 8-byte segment header, 256-byte desc
    if len(header) != 288 or header[0] != APP_MAGIC:
        raise ValueError("not a complete ESP-IDF application image header")
    if struct.unpack_from("<H", header, 12)[0] != CHIP_IDS[node]:
        raise ValueError(f"wrong chip for Node {node.upper()}")
    if (struct.unpack_from("<I", header, 28)[0] < 256 or
            struct.unpack_from("<I", header, 32)[0] != 0xABCD5432):
        raise ValueError("missing ESP-IDF application descriptor")
    project = header[80:112].split(b"\0", 1)[0].decode("ascii", errors="strict")
    if project != f"node_{node}":
        raise ValueError(f"wrong project {project!r} for Node {node.upper()}")
    sha = header[176:208].hex()
    if sha == "0" * 64:
        raise ValueError("image has no ELF identity")
    return sha


def read_status(host, port):
    # Local-network operations must not follow system proxies or redirects.
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    try:
        with opener.open(f"http://{host}:{port}/ota/status", timeout=2) as response:
            payload = response.read(1025)
        if len(payload) > 1024:
            return None
        status = json.loads(payload)
        if not isinstance(status, dict):
            return None
        return status
    except (OSError, ValueError):
        return None


def image_confirmed(status, expected):
    return (isinstance(status, dict) and
            status.get("elf_sha256") == expected and
            type(status.get("ota_state")) is int and status["ota_state"] == OTA_VALID)


def wait_verified(host, port, expected, timeout=90):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = read_status(host, port)
        if image_confirmed(last, expected):
            return last
        time.sleep(1)
    raise TimeoutError(f"image not verified within {timeout}s; last status: {last!r}")


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
    ap.add_argument("--allow-legacy", action="store_true",
                    help="allow upload to a receiver without /ota/status (initial migration/backout only)")
    args = ap.parse_args()

    host = args.host or NODES[args.node][0]
    binpath = args.bin or os.path.join(REPO, NODES[args.node][1])

    # --- validate the image locally before shipping it ---
    if not os.path.isfile(binpath):
        die(f"firmware not found: {binpath}\n       build it first (idf.py -C node_{args.node} build)")
    size = os.path.getsize(binpath)
    try:
        expected = image_identity(binpath, args.node)
    except (OSError, ValueError) as exc:
        die(f"{binpath}: {exc}")

    # --- resolve / reachability preflight ---
    ip = args.host if (args.host and args.host[0].isdigit()) else resolve(host)
    if ip is None:
        die(f"can't resolve '{host}'. On this LAN pfSense DNS often won't answer the Mac —\n"
            f"       pass the node's IP instead:  {sys.argv[0]} {args.node} --host <ip>")
    if not port_open(ip, args.port):
        die(f"{host} ({ip}) not accepting connections on :{args.port}. Is the node up and on WiFi? "
            f"Is CFG_OTA_PORT == {args.port}?")
    before = read_status(ip, args.port)
    if before is None and not args.allow_legacy:
        die("receiver has no readable /ota/status; use --allow-legacy only for a deliberate legacy migration")
    if image_confirmed(before, expected):
        print("requested image is already running and marked valid; no upload needed.")
        return
    if before is not None and before.get("ota_state") == 1:
        die("receiver is still pending verification; do not overwrite its recovery slot")

    url = f"http://{ip}:{args.port}/ota"
    print(f"pushing {os.path.relpath(binpath, REPO)} ({size:,} bytes) -> {host} ({ip}) :{args.port}")

    # --- push via curl (as requested); --fail-with-body surfaces the node's
    #     error text on a 4xx/5xx while still failing the exit code. ---
    cmd = [
        "curl", "-sS", "--fail-with-body", "--noproxy", "*",
        "--max-time", str(args.timeout),
        "-H", "Content-Type: application/octet-stream",
        "-H", "Expect:",                    # avoid a 100-continue stall
        "-w", "\nHTTP %{http_code} in %{time_total}s\n",
        "--data-binary", f"@{binpath}",
        url,
    ]
    rc = subprocess.call(cmd)
    if rc != 0:
        die(f"curl exit {rc} — update outcome is uncertain; check /ota/status before retrying. "
            "The device may have accepted the image before the HTTP response was lost.")

    if args.no_wait:
        print("upload accepted; running image NOT VERIFIED (--no-wait).")
        return

    print(f"waiting for ELF {expected} and valid OTA state", flush=True)
    try:
        status = wait_verified(ip, args.port, expected)
    except TimeoutError as exc:
        print(f"warning: {exc}", file=sys.stderr)
        print("Do not retry blindly. Check for rollback or use the saved recovery image. "
              "Legacy backout images require independent boot/app verification.", file=sys.stderr)
        sys.exit(2)
    print(f"verified: expected image running, rollback cancelled; uptime {status.get('uptime_ms')} ms. "
          "BLE/app functional checks are still required.")


if __name__ == "__main__":
    main()
