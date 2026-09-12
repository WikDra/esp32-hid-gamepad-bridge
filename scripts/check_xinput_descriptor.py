#!/usr/bin/env python3
"""Validate the USB XInput descriptor that will actually be on the wire.

Run against a built image:

    python scripts/check_xinput_descriptor.py firmware/build.win.esp32s3.s3pad

Why this exists. The pad chip cannot be tested without a PC on the other end, and a
descriptor bug shows up there as "Unknown USB Device" with no hint as to which byte is
wrong. Everything checkable without hardware is checked here instead:

  * the interface-0 block is compared BYTE FOR BYTE against a capture of a genuine
    Microsoft Xbox 360 wired controller,
  * the descriptor is parsed and its internal lengths verified (wTotalLength against the
    real total, every bLength against what it describes),
  * the endpoint addresses and payload sizes repeated inside the vendor 0x21 descriptor are
    checked against the endpoint descriptors themselves - the source that documented this
    descriptor found that changing one without the other stops the Windows driver from
    talking to the device, so a mismatch here is a real, silent bug.

Reference: partsnotincluded.com, "Understanding the Xbox 360 Wired Controller's USB Data"
(Wireshark capture of a genuine pad), cross-checked against the Linux xpad driver, which
binds on interface class 0xFF / subclass 0x5D / protocol 1.
"""
import sys
from pathlib import Path

# Interface 0 of a genuine Xbox 360 wired controller: interface descriptor, the vendor
# descriptor of type 0x21, then the two interrupt endpoints. 40 bytes.
REFERENCE_IFACE0 = bytes([
    0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0x00,
    0x11, 0x21, 0x00, 0x01, 0x01, 0x25, 0x81, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x13, 0x01, 0x08, 0x00, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
    0x07, 0x05, 0x01, 0x03, 0x20, 0x00, 0x08,
])

DESC_CONFIG, DESC_INTERFACE, DESC_ENDPOINT, DESC_VENDOR = 0x02, 0x04, 0x05, 0x21


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def find_descriptor(blob):
    """Locate our configuration descriptor by the interface-0 signature."""
    at = blob.find(REFERENCE_IFACE0)
    if at < 0:
        fail("interface 0 block not found in the image - either the descriptor differs from "
             "the real controller, or the build does not contain usb_pad.c")
    # The configuration descriptor is the 9 bytes immediately before it.
    start = at - 9
    if start < 0 or blob[start + 1] != DESC_CONFIG:
        fail("found interface 0, but no configuration descriptor directly in front of it")
    total = blob[start + 2] | (blob[start + 3] << 8)
    return blob[start:start + total], total


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    build = Path(sys.argv[1])
    images = list(build.glob("*.bin"))
    images = [p for p in images if "bootloader" not in str(p) and "partition" not in str(p)]
    if not images:
        fail("no application .bin in " + str(build))
    blob = images[0].read_bytes()
    print("image: %s (%d B)" % (images[0].name, len(blob)))

    desc, total = find_descriptor(blob)
    print("configuration descriptor: %d B (wTotalLength says %d)" % (len(desc), total))
    if len(desc) != total:
        fail("wTotalLength %d does not match the bytes present (%d)" % (total, len(desc)))

    # Walk it.
    ifaces, endpoints, vendor = [], [], []
    i = 0
    while i < len(desc):
        blen, btype = desc[i], desc[i + 1]
        if blen == 0:
            fail("descriptor with bLength 0 at offset %d - the walk would never end" % i)
        if i + blen > len(desc):
            fail("descriptor at offset %d claims %d B but only %d remain" %
                 (i, blen, len(desc) - i))
        if btype == DESC_INTERFACE:
            ifaces.append(desc[i:i + blen])
        elif btype == DESC_ENDPOINT:
            endpoints.append(desc[i:i + blen])
        elif btype == DESC_VENDOR:
            vendor.append(desc[i:i + blen])
        i += blen

    print("  interfaces: %d, endpoints: %d, vendor 0x21 descriptors: %d" %
          (len(ifaces), len(endpoints), len(vendor)))

    if len(ifaces) != 1:
        fail("expected exactly 1 interface, found %d" % len(ifaces))
    ifd = ifaces[0]
    if (ifd[5], ifd[6], ifd[7]) != (0xFF, 0x5D, 0x01):
        fail("interface is %02X/%02X/%02X, must be FF/5D/01 or Windows will not bind XUSB" %
             (ifd[5], ifd[6], ifd[7]))
    if ifd[4] != len(endpoints):
        fail("interface says %d endpoints, %d present" % (ifd[4], len(endpoints)))
    print("  interface 0: class FF subclass 5D protocol 01, %d endpoints - OK" % ifd[4])

    if len(vendor) != 1 or len(vendor[0]) != 17:
        fail("expected one 17-byte vendor 0x21 descriptor")

    # The endpoint addresses and sizes are repeated inside the vendor descriptor. Offsets
    # 6/7 describe the IN endpoint, 13/14 the OUT one.
    v = vendor[0]
    v_in_ep, v_in_len, v_out_ep, v_out_len = v[6], v[7], v[13], v[14]
    eps = {e[2]: e for e in endpoints}
    if v_in_ep not in eps or v_out_ep not in eps:
        fail("vendor descriptor names endpoints 0x%02X/0x%02X which are not declared" %
             (v_in_ep, v_out_ep))
    if v_in_len != 20:
        fail("vendor descriptor says the IN payload is %d B, the XInput report is 20" % v_in_len)
    if v_out_len != 8:
        fail("vendor descriptor says the OUT payload is %d B, rumble packets are 8" % v_out_len)
    print("  vendor 0x21: IN 0x%02X/%d B, OUT 0x%02X/%d B, consistent with the endpoints - OK" %
          (v_in_ep, v_in_len, v_out_ep, v_out_len))

    for e in endpoints:
        addr, attrs = e[2], e[3]
        size = e[4] | (e[5] << 8)
        interval = e[6]
        if attrs & 0x03 != 0x03:
            fail("endpoint 0x%02X is not an interrupt endpoint (bmAttributes 0x%02X)" %
                 (addr, attrs))
        if size != 32:
            fail("endpoint 0x%02X has wMaxPacketSize %d, the real pad uses 32" % (addr, size))
        expect_interval = 4 if addr & 0x80 else 8
        if interval != expect_interval:
            fail("endpoint 0x%02X has bInterval %d, the real pad uses %d" %
                 (addr, interval, expect_interval))
        print("  endpoint 0x%02X: interrupt, %d B, every %d ms - OK" % (addr, size, interval))

    # Device descriptor: the identity is what makes Windows load XInput at all.
    dev = blob.find(bytes([0x12, 0x01, 0x00, 0x02, 0xFF, 0xFF, 0xFF]))
    if dev < 0:
        fail("device descriptor (vendor-specific, USB 2.0) not found in the image")
    vid = blob[dev + 8] | (blob[dev + 9] << 8)
    pid = blob[dev + 10] | (blob[dev + 11] << 8)
    if (vid, pid) != (0x045E, 0x028E):
        fail("identity is VID 0x%04X PID 0x%04X; xusb22.inf only matches 045E:028E, "
             "045E:0719 and 045E:028F" % (vid, pid))
    print("  device: VID 0x%04X PID 0x%04X (Xbox 360 wired) - matches xusb22.inf" % (vid, pid))

    print("\nOK: descriptor is internally consistent and interface 0 is byte-for-byte the "
          "real controller's.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
