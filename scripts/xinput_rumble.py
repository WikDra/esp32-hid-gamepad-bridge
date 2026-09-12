"""Reads an XInput pad and makes it rumble - the decisive test for the USB pad.

    python scripts\\xinput_rumble.py [slot]

WHY THIS EXISTS. Two questions about the USB pad cannot be answered by looking at the device
tree. Whether Windows BOUND the XInput driver is visible there (Service=xusb22), but whether
XInput actually TALKS to us is not - and neither is whether our OUT endpoint works.

Rumble settles both at once, and it is the same argument that settled the BLE profile in
AGENTS.md 4.32: a rumble command can only come from the pad driver. Generic HID handling never
sends one. So a line

    rumble from host: left=255 right=0

in the device log proves the whole chain: XInput enumerated us, its driver accepted us as a
controller, and the interrupt OUT endpoint carries host-to-device traffic.

Doing it through XInputSetState rather than through a game removes every other variable - no
Steam, no browser, no third-party tool. It also reads XInputGetState first, which proves the
pad occupies a real XInput slot rather than merely existing in Device Manager.

The three bursts use DELIBERATELY DIFFERENT motor values, because one ambiguous line proves
less than three distinguishable ones: the log should mirror the sequence, not just show
activity. XInput takes 16-bit speeds and the driver scales them to the 8-bit fields of the
Xbox 360 output report, so 0xFFFF arrives as 255 and 0x4000 as roughly 64.
"""
import ctypes
import sys
import time
from ctypes import wintypes


class XINPUT_GAMEPAD(ctypes.Structure):
    _fields_ = [
        ("wButtons", wintypes.WORD),
        ("bLeftTrigger", ctypes.c_ubyte),
        ("bRightTrigger", ctypes.c_ubyte),
        ("sThumbLX", ctypes.c_short),
        ("sThumbLY", ctypes.c_short),
        ("sThumbRX", ctypes.c_short),
        ("sThumbRY", ctypes.c_short),
    ]


class XINPUT_STATE(ctypes.Structure):
    _fields_ = [("dwPacketNumber", wintypes.DWORD), ("Gamepad", XINPUT_GAMEPAD)]


class XINPUT_VIBRATION(ctypes.Structure):
    _fields_ = [("wLeftMotorSpeed", wintypes.WORD), ("wRightMotorSpeed", wintypes.WORD)]


ERROR_SUCCESS = 0
ERROR_DEVICE_NOT_CONNECTED = 1167

# XInput1_4 ships with Windows 8 and later; the others are fallbacks for older systems and for
# the redistributable version. Load order matters only in that the newest one supports the most.
for name in ("XInput1_4.dll", "xinput1_3.dll", "XInput9_1_0.dll"):
    try:
        xinput = ctypes.WinDLL(name)
        break
    except OSError:
        xinput = None
if xinput is None:
    print("ERROR: no XInput DLL found (tried XInput1_4, xinput1_3, XInput9_1_0)")
    sys.exit(1)
print(f"--- using {name} ---", flush=True)


def read_slot(slot):
    st = XINPUT_STATE()
    rc = xinput.XInputGetState(slot, ctypes.byref(st))
    return rc, st


def describe(st):
    g = st.Gamepad
    return (
        f"buttons=0x{g.wButtons:04x} LT={g.bLeftTrigger} RT={g.bRightTrigger} "
        f"L=({g.sThumbLX},{g.sThumbLY}) R=({g.sThumbRX},{g.sThumbRY})"
    )


def rumble(slot, left, right):
    v = XINPUT_VIBRATION(left, right)
    rc = xinput.XInputSetState(slot, ctypes.byref(v))
    return rc


wanted = int(sys.argv[1]) if len(sys.argv) > 1 else None
slots = [wanted] if wanted is not None else [0, 1, 2, 3]

found = []
for slot in slots:
    rc, st = read_slot(slot)
    if rc == ERROR_SUCCESS:
        found.append(slot)
        print(f"slot {slot}: CONNECTED, packet={st.dwPacketNumber}, {describe(st)}", flush=True)
    elif rc == ERROR_DEVICE_NOT_CONNECTED:
        print(f"slot {slot}: empty", flush=True)
    else:
        print(f"slot {slot}: XInputGetState returned {rc}", flush=True)

if not found:
    print("\nNo XInput device in any slot. The pad exists in Device Manager but XInput does "
          "not see it - that is a different failure from the driver not binding.")
    sys.exit(2)

slot = found[0]
print(f"\n--- rumbling slot {slot}; watch the device log for 'rumble from host' ---", flush=True)

# Left only, right only, then both at a middling level. Distinct values on purpose.
for left, right, label in ((0xFFFF, 0x0000, "left full"),
                           (0x0000, 0xFFFF, "right full"),
                           (0x4000, 0x4000, "both ~25%")):
    rc = rumble(slot, left, right)
    print(f"  {label:11s} -> XInputSetState(0x{left:04x}, 0x{right:04x}) rc={rc}"
          f"{'  <- FAILED' if rc != ERROR_SUCCESS else ''}", flush=True)
    time.sleep(1.5)

rumble(slot, 0, 0)
print("  stop        -> XInputSetState(0x0000, 0x0000)", flush=True)

rc, st = read_slot(slot)
if rc == ERROR_SUCCESS:
    print(f"\nslot {slot} after the test: {describe(st)}", flush=True)
