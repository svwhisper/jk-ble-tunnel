"""Bounded passive capture for the cold/warm attach test (2026-09-26).
A UDP :3766 logs, B USB console (read-only, no DTR/RTS), MQTT jkbms/# via
mosquitto_sub. Sends nothing to either node. Stops after argv[2] minutes
(default 45)."""
import datetime, json, os, select, socket, subprocess, sys, time

out = sys.argv[1]
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.bind(("", 3766)); udp.setblocking(False)
usb = os.open("/dev/cu.usbmodem1101", os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
mq = subprocess.Popen(["mosquitto_sub", "-h", "192.168.2.5", "-v",
                       "-t", "jkbms/+/state/link", "-t", "jkbms/bridge/#"],
                      stdout=subprocess.PIPE, bufsize=0)
os.set_blocking(mq.stdout.fileno(), False)
minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 45
deadline = time.monotonic() + minutes * 60
with open(out, "x", buffering=1) as log:
    def rec(src, data):
        log.write(json.dumps({"at": datetime.datetime.now().astimezone().isoformat(),
                              "source": src, "data": data}) + "\n")
    rec("capture", "started: A UDP3766, passive B USB, MQTT; no node writes")
    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([udp, usb, mq.stdout], [], [], 1)
            for fd in ready:
                try:
                    if fd is udp:
                        d, peer = udp.recvfrom(65535); rec("UDP:" + peer[0], d.decode("utf-8", "replace"))
                    elif fd is mq.stdout:
                        d = os.read(mq.stdout.fileno(), 65536)
                        if d: rec("MQTT", d.decode("utf-8", "replace"))
                    else:
                        d = os.read(usb, 8192)
                        if not d: raise RuntimeError("USB EOF")
                        rec("B-USB", d.decode("utf-8", "replace"))
                except BlockingIOError:
                    pass
    finally:
        rec("capture", "stopped"); mq.terminate(); os.close(usb); udp.close()
