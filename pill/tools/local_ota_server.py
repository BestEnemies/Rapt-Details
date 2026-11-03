#!/usr/bin/env python3
"""
Local OTA server for the RAPT Pill.

Mimics the one endpoint the Pill's firmware uses to fetch an update:

    GET /api/device/update?macAddress=<mac>&device=<device>&version=<version>

Behaviour matches the real server as observed:
  - If the device's reported <version> already equals the image we're serving,
    reply 200 with an empty body  -> "up to date".
  - Otherwise reply 200 with Content-Type application/octet-stream and the
    raw ESP-IDF app image  -> the Pill streams it straight into its spare
    OTA slot and reboots into it.

The firmware is patched (separately) so the request goes to
    http://<this-host>/api/device/update?...
over plain HTTP instead of https://api.rapt.io/...

Usage:
    python local_ota_server.py --image <firmware.bin> [--host 0.0.0.0] [--port 80]

The served image's version string is read from its ESP-IDF app descriptor so
the up-to-date check works without hardcoding it.
"""
import argparse, http.server, socketserver, struct, urllib.parse, sys, datetime

def app_version(path):
    with open(path, "rb") as f:
        head = f.read(0x120)
    # ESP-IDF esp_app_desc_t lives at image offset 0x20, magic 0xABCD5432
    if head[0] != 0xE9:
        print("WARNING: %s does not start with ESP image magic 0xE9" % path)
    desc = head[0x20:0x120]
    if desc[:4] != b"\x32\x54\xcd\xab":
        print("WARNING: no app descriptor magic; version check disabled")
        return None
    return desc[16:48].rstrip(b"\x00").decode("latin1")

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        sys.stdout.write("[%s] %s - %s\n" % (ts, self.client_address[0], fmt % args))
        sys.stdout.flush()

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        mac = q.get("macAddress", ["?"])[0]
        dev = q.get("device", ["?"])[0]
        ver = q.get("version", ["?"])[0]
        self.log_message("UPDATE check: device=%s mac=%s version=%s path=%s",
                         dev, mac, ver, u.path)

        if not u.path.rstrip("/").endswith("/api/device/update"):
            self.send_response(404); self.end_headers(); return

        target = self.server.image_version
        if target is not None and ver == target:
            self.log_message("  -> up to date (serving empty 200)")
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        data = self.server.image_bytes
        self.log_message("  -> sending firmware %s (%d bytes, version %s)",
                         self.server.image_path, len(data), target)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True, help="firmware .bin to serve")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=80)
    a = ap.parse_args()

    data = open(a.image, "rb").read()
    ver = app_version(a.image)
    srv = Server((a.host, a.port), Handler)
    srv.image_bytes = data
    srv.image_path = a.image
    srv.image_version = ver
    print("Serving %s (%d bytes, version %s) on %s:%d" %
          (a.image, len(data), ver, a.host, a.port))
    print("Endpoint: http://<host>:%d/api/device/update" % a.port)
    print("Waiting for the Pill...  (Ctrl+C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")

if __name__ == "__main__":
    main()
