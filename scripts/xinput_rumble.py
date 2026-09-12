"""Reads an XInput pad and makes it rumble - the decisive test for the USB pad.

    python scripts\\xinput_rumble.py [slot]              rumble test
    python scripts\\xinput_rumble.py --watch [s] [slot]   live state, end-to-end test
    python scripts\\xinput_rumble.py --rate [s] [slot]    how often the pad really updates

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


wanted = None
watch_secs = 0.0
rate_secs = 0.0
args = sys.argv[1:]
if args and args[0] == "--watch":
    watch_secs = float(args[1]) if len(args) > 1 else 15.0
    if len(args) > 2:
        wanted = int(args[2])
elif args and args[0] == "--rate":
    rate_secs = float(args[1]) if len(args) > 1 else 5.0
    if len(args) > 2:
        wanted = int(args[2])
elif args:
    wanted = int(args[0])

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

if rate_secs:
    # Measures how often the pad ACTUALLY updates, which is not the same as its endpoint
    # interval: the endpoint says how often the host asks, the firmware decides how often it
    # has something new. dwPacketNumber is the right counter for this - XInput bumps it once
    # per state delivered - and the loop deliberately does not sleep, because a sleeping poll
    # loop measures itself rather than the device.
    #
    # The pad must be MOVING throughout, otherwise there is nothing to count: a state that
    # does not change produces no new packets, by design.
    print(f"\n--- measuring slot {slot} for {rate_secs:.0f}s; keep the mouse moving ---",
          flush=True)
    last_packet = None
    packets = 0
    polls = 0
    start = time.time()
    end = start + rate_secs
    while time.time() < end:
        rc, st = read_slot(slot)
        polls += 1
        if rc != ERROR_SUCCESS:
            print("  device went away", flush=True)
            break
        if st.dwPacketNumber != last_packet:
            if last_packet is not None:
                packets += 1
            last_packet = st.dwPacketNumber
    elapsed = time.time() - start
    print(f"  {packets} state updates in {elapsed:.2f}s = {packets / elapsed:.0f} Hz", flush=True)
    print(f"  (polled {polls / elapsed:.0f} times per second, so the loop is not the limit)",
          flush=True)
    sys.exit(0)

if watch_secs:
    # Watch mode is the end-to-end test for the whole bridge, taken from the side that
    # matters: this is the same API a game reads. It needs no console on the pad chip, so
    # the single USB-UART adapter can stay on the input chip while this runs.
    #
    # Only changes are printed, keyed on dwPacketNumber - XInput bumps it whenever the state
    # differs, so a still pad produces no output and a moving one produces a readable trace
    # rather than a wall of identical lines.
    print(f"\n--- watching slot {slot} for {watch_secs:.0f}s; move the mouse, press keys ---",
          flush=True)
    last_packet = None
    end = time.time() + watch_secs
    changes = 0
    while time.time() < end:
        rc, st = read_slot(slot)
        if rc != ERROR_SUCCESS:
            print("  device went away", flush=True)
            break
        if st.dwPacketNumber != last_packet:
            last_packet = st.dwPacketNumber
            changes += 1
            print(f"  [{changes:4d}] {describe(st)}", flush=True)
        time.sleep(0.01)
    print(f"\n{changes} state changes in {watch_secs:.0f}s", flush=True)
    sys.exit(0)

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
