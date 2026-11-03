#!/usr/bin/env python3
"""
Patch the RAPT Pill 2.0.0 app image so its cloud calls go to YOUR host over
plain HTTP instead of https://<aws-exec-api> over TLS.

Why this is needed: the firmware's OTA/registration HTTP client takes the
protocol from the URL. `https://` => mbedTLS verifies the server cert against
the baked-in global CA store. There is NO https->http downgrade on failure, so
a DNS redirect alone cannot work; the scheme must be changed in the binary.

Rewrites three rodata URL strings in place (new string is shorter, so the tail
is NUL-padded within each string's own byte span — nothing else moves):

  0x54e6  https://<aws>/v1/devices/firmwareVersionCheck-wifi?...   (OTA check)
  0x42f0  https://<aws>/v1/devices/autoUpdatePermission            (update gate)
  0x3d30  https://<aws>/v1/device/download-automation?...          (automations)

Only the scheme+host prefix is swapped; the PATH and query (%s specifiers) are
kept byte-identical, so your server just needs to answer the same paths.

Usage:
  python patch_ota_url.py app_2.0.0.bin 192.168.4.50 -o app_2.0.0.patched.bin
  python patch_ota_url.py app_2.0.0.bin 192.168.4.50:8080 -o out.bin   # host:port

After flashing the patched image once (flash_pill.py, pogo jig), the Pill will
hit http://<host>/v1/devices/firmwareVersionCheck-wifi?... Return a manifest:

  {"updateAvailable":true,"targetVersion":"9.9.9","size":<bytes>,
   "downloadUrl":"http://<host>/fw.bin",
   "checksum":{"algorithm":"SHA256","value":"<sha256-hex-of-fw.bin>"}}

and serve the image at that downloadUrl. (The SHA256 is yours to set; the gate
just checks the bytes match the manifest you wrote.) Also answer
/v1/devices/autoUpdatePermission permissively.
"""
import argparse, sys

AWS_PREFIX = b"https://d78naeycz0.execute-api.ap-southeast-2.amazonaws.com"  # 59 bytes

# (file_offset, declared_length) of each URL string in app_2.0.0.bin
URLS = [
    (0x54e6, 161),  # firmwareVersionCheck-wifi
    (0x42f0, 91),   # autoUpdatePermission
    (0x3d30, 116),  # download-automation
]

def patch_one(buf, off, length, new_prefix):
    orig = bytes(buf[off:off+length])
    if not orig.startswith(AWS_PREFIX):
        # tolerate already-patched / unexpected content
        print("  @%#06x: prefix not found (got %r) - skipped" % (off, orig[:16]))
        return False
    tail = orig[len(AWS_PREFIX):]          # the /v1/... path+query, kept verbatim
    new = new_prefix + tail
    if len(new) >= length:                 # must fit incl. NUL terminator
        print("  @%#06x: host too long (%d >= %d) - skipped" % (off, len(new), length))
        return False
    buf[off:off+length] = new + b"\x00" * (length - len(new))
    print("  @%#06x: -> %s" % (off, new.decode("latin1")))
    return True

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("host", help="your server as IP, host, or host:port (no scheme)")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()

    host = a.host.strip().strip("/")
    new_prefix = b"http://" + host.encode("latin1")
    budget = len(AWS_PREFIX)  # 59
    if len(new_prefix) > budget:
        sys.exit("host too long: 'http://%s' is %d bytes, max %d "
                 "(use an IP or a short LAN hostname)" % (host, len(new_prefix), budget))

    buf = bytearray(open(a.image, "rb").read())
    print("Patching %s  ->  http://%s" % (a.image, host))
    n = sum(patch_one(buf, off, ln, new_prefix) for off, ln in URLS)
    open(a.out, "wb").write(buf)
    print("Wrote %s  (%d/%d URLs patched)" % (a.out, n, len(URLS)))
    if n != len(URLS):
        print("NOTE: not all URLs patched - re-check offsets for this image build.")

if __name__ == "__main__":
    main()
