#!/usr/bin/env python3
"""
Checks the mouse-to-stick arithmetic of firmware/main/input_mapper.c.

WHAT THIS IS. A model of the integer arithmetic, not the firmware. It exists because that
arithmetic has been wrong three separate times (AGENTS.md 4.40: a time constant expressed in
ticks, a fixed-point accumulator too coarse to move at 1 kHz, and a truncation that left three
usable stick levels), and every one of those bugs compiled, linked and felt vaguely plausible on
hardware. A model catches that class of defect in a second instead of in a measurement session.

WHAT IT CANNOT DO. It is a second copy of the arithmetic and so it can drift from the first.
Treat a failure as "one of these two is wrong", not as "the firmware is wrong", and keep the two
in step when changing either.

Integer division here truncates TOWARDS ZERO, like C and unlike Python's //. Getting that wrong
silently changes the filter's behaviour for negative deltas only, which is the kind of asymmetry
nobody notices by feel.
"""

import sys

AXIS_MAX = 127
EMA_FRAC = 4096


def cdiv(a, b):
    """C integer division: truncates towards zero."""
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def isqrt32(v):
    """Mirrors isqrt32() in input_mapper.c."""
    rem, root, bit = v, 0, 1 << 30
    while bit > rem:
        bit >>= 2
    while bit:
        if rem >= root + bit:
            rem -= root + bit
            root = (root >> 1) + bit
        else:
            root >>= 1
        bit >>= 2
    return root


def clamp_axis(v):
    return max(-AXIS_MAX, min(AXIS_MAX, v))


class Filter:
    """The EMA plus the scaling, as the firmware does it now."""

    def __init__(self, rate_hz, div_x, div_y, tau_ms, anti_dz_pct=0, invert_y=False):
        self.rate = rate_hz
        self.ticks = max(1, (rate_hz * tau_ms) // 1000)
        self.denom_x = div_x * 400 * EMA_FRAC
        self.denom_y = div_y * 400 * EMA_FRAC
        self.anti_dz = (anti_dz_pct * AXIS_MAX) // 100
        self.invert_y = invert_y
        self.ema_x = 0
        self.ema_y = 0

    def _step(self, ema, sample):
        ema += cdiv(sample * EMA_FRAC - ema, self.ticks)
        if sample == 0 and -EMA_FRAC < ema < EMA_FRAC:
            ema = 0
        return ema

    def tick(self, dx, dy):
        self.ema_x = self._step(self.ema_x, dx)
        self.ema_y = self._step(self.ema_y, dy)
        x = cdiv(self.ema_x * AXIS_MAX * self.rate, self.denom_x)
        y = cdiv(self.ema_y * AXIS_MAX * self.rate, self.denom_y)
        if self.invert_y:
            y = -y
        x, y = clamp_axis(x), clamp_axis(y)
        x, y = anti_deadzone(x, y, self.anti_dz)
        return clamp_axis(x), clamp_axis(y)


def anti_deadzone(x, y, dz):
    """Mirrors apply_anti_deadzone()."""
    if dz <= 0 or (x == 0 and y == 0):
        return x, y
    mag = isqrt32(x * x + y * y)
    if mag == 0:
        return x, y
    scaled = dz + (mag * (AXIS_MAX - dz)) // AXIS_MAX
    return cdiv(x * scaled, mag), cdiv(y * scaled, mag)


def legacy_tick(state, rate_hz, div, tau_ms, dx, dy):
    """
    The arithmetic as it was BEFORE the configuration struct: one divisor, a compile-time time
    constant, no deadzone, no inversion. This is the behaviour the refactor promised not to
    change.
    """
    ticks = max(1, (rate_hz * tau_ms) // 1000)
    denom = div * 400 * EMA_FRAC

    def step(ema, sample):
        ema += cdiv(sample * EMA_FRAC - ema, ticks)
        if sample == 0 and -EMA_FRAC < ema < EMA_FRAC:
            ema = 0
        return ema

    state[0] = step(state[0], dx)
    state[1] = step(state[1], dy)
    return (clamp_axis(cdiv(state[0] * AXIS_MAX * rate_hz, denom)),
            clamp_axis(cdiv(state[1] * AXIS_MAX * rate_hz, denom)))


def movement_sequence():
    """
    Deliberately includes negatives, zeros, a hard stop and a sign reversal. The truncation bug
    and the C-vs-Python division difference are both invisible on positive-only input.
    """
    seq = []
    for dx, dy, n in ((0, 0, 5), (3, -2, 40), (1, 0, 30), (-7, 5, 40),
                      (0, 0, 60), (-1, -1, 30), (40, -40, 20), (0, 0, 120),
                      (2, 2, 10), (-2, -2, 10)):
        seq.extend([(dx, dy)] * n)
    return seq


def check_equivalence():
    """Defaults must reproduce the old arithmetic exactly, at both rates we ship."""
    problems = []
    for rate, div in ((100, 24), (250, 24), (1000, 64), (1000, 24)):
        f = Filter(rate, div, div, 80)
        legacy_state = [0, 0]
        for i, (dx, dy) in enumerate(movement_sequence()):
            got = f.tick(dx, dy)
            want = legacy_tick(legacy_state, rate, div, 80, dx, dy)
            if got != want:
                problems.append(
                    f"rate={rate} div={div} tick {i}: new {got} != legacy {want}")
                break
    return problems


def check_anti_deadzone():
    problems = []
    for dz_pct in (0, 5, 15, 25, 50, 80):
        dz = (dz_pct * AXIS_MAX) // 100

        # Zero must stay zero: a deadzone that pushes a centred stick off centre would make the
        # pad drift on its own, which is worse than the problem it solves.
        if anti_deadzone(0, 0, dz) != (0, 0):
            problems.append(f"dz={dz_pct}%: centre does not stay centred")

        # Any non-zero deflection must clear the deadzone, which is the entire point.
        if dz > 0:
            for v in (1, 2, 3):
                x, _ = anti_deadzone(v, 0, dz)
                if x < dz:
                    problems.append(f"dz={dz_pct}%: input {v} gives {x}, below the deadzone {dz}")

        # Full deflection must still be reachable, or the top of the range is lost.
        x, _ = anti_deadzone(AXIS_MAX, 0, dz)
        if x != AXIS_MAX:
            problems.append(f"dz={dz_pct}%: full deflection gives {x}, not {AXIS_MAX}")

        # Monotonic, and never out of int8_t range after the final clamp.
        prev = -1
        for v in range(0, AXIS_MAX + 1):
            x, _ = anti_deadzone(v, 0, dz)
            x = clamp_axis(x)
            if x < prev:
                problems.append(f"dz={dz_pct}%: not monotonic at {v} ({x} < {prev})")
                break
            prev = x

        # A diagonal must not end up longer than a straight push of the same input, which is
        # what a per-axis deadzone would do. Allowed slack is the clamp, not the model.
        dx, dy = anti_deadzone(90, 90, dz)
        straight, _ = anti_deadzone(127, 0, dz)
        if isqrt32(clamp_axis(dx) ** 2 + clamp_axis(dy) ** 2) > straight + 2:
            problems.append(f"dz={dz_pct}%: diagonal overshoots a straight push")

        # Symmetry: the mapping must not favour one direction.
        for v in (1, 40, 127):
            pos, _ = anti_deadzone(v, 0, dz)
            neg, _ = anti_deadzone(-v, 0, dz)
            if pos != -neg:
                problems.append(f"dz={dz_pct}%: asymmetric at {v} ({pos} vs {neg})")
    return problems


def steady_axis(rate, div, tau_ms, counts_per_tick):
    """Drives a constant mouse speed until the filter settles, and returns the X axis value."""
    f = Filter(rate, div, div, tau_ms)
    x = 0
    for _ in range(rate * 2):  # two seconds is many time constants at any rate we ship
        x = f.tick(counts_per_tick, 0)[0]
    return x


def check_rate_independence():
    """
    THE invariant, and the one AGENTS.md 4.40 records as broken: sensitivity is defined in counts
    per SECOND, so the same physical mouse speed must produce the same deflection whether the
    task runs at 100 Hz or at 1 kHz. When the constants were per-tick, going from 100 Hz to
    250 Hz silently made the mouse 2.5x less sensitive and the filter 2.5x faster.

    TOLERANCE OF ONE AXIS UNIT, and it is not slack for its own sake. The integer EMA stops
    short of its target: the step is (target - ema) / ticks truncated, so it stalls once the
    difference falls below `ticks`, leaving up to ticks-1 of the accumulator unused. In axis
    units that is (ticks-1) * 127 * rate / (div * 400 * 4096), which at 1 kHz with div=24 comes
    to 0.26 - under one unit of 127 - and at 100 Hz to 0.002. So the shortfall is real, is
    bounded below one step of the axis, and is larger at higher rates. Rounding instead of
    truncating would remove it; that is not done because the measured 997 Hz and 830 Hz figures
    were taken with this arithmetic and changing the filter feel is not free.

    Speeds are multiples of 1000 so that counts-per-tick divides exactly at 100, 250 and 1000 Hz.
    Without that the test drives a DIFFERENT speed at each rate and then reports the difference
    as a firmware fault - which is exactly what the first version of it did.
    """
    problems = []
    tolerance = 1

    for div in (24, 64):
        for counts_per_second in (1000, 2000, 5000, 9000):
            results = {}
            for rate in (100, 250, 1000):
                per_tick, remainder = divmod(counts_per_second, rate)
                assert remainder == 0 and per_tick >= 1, "speed must divide exactly at every rate"
                results[rate] = steady_axis(rate, div, 80, per_tick)

            spread = max(results.values()) - min(results.values())
            if spread > tolerance:
                problems.append(
                    f"div={div} at {counts_per_second} counts/s: deflection varies by {spread} "
                    f"across task rates {results}")

            # And the value has to be the one the documented formula predicts: full deflection
            # at div * 400 counts per second.
            expected = (AXIS_MAX * counts_per_second) // (div * 400)
            for rate, got in results.items():
                if abs(got - expected) > tolerance:
                    problems.append(
                        f"div={div} rate={rate} at {counts_per_second} counts/s: got {got}, "
                        f"formula says {expected}")
    return problems


def check_filter_behaviour():
    problems = []

    # The 1 kHz stall (AGENTS.md 4.40): a coarse accumulator could not be moved at all by a
    # delta of about one count per tick, so slow movement stopped registering entirely.
    if steady_axis(1000, 24, 80, 1) == 0:
        problems.append("1 kHz, one count per tick: stick stays centred - the filter is stalling")

    for rate in (100, 250, 1000):
        # It must return to centre after the mouse stops, or the stick sticks where it was.
        f = Filter(rate, 24, 24, 80)
        for _ in range(rate):
            f.tick(20, -20)
        for _ in range(rate):
            f.tick(0, 0)
        if f.tick(0, 0) != (0, 0):
            problems.append(f"rate={rate}: does not return to centre after movement stops")

        # The time constant must be a time, not a tick count: settling should take roughly the
        # same wall-clock time at every rate. Measured as ticks to reach 60 % of steady state.
        target = steady_axis(rate, 24, 80, max(1, 1000 // rate))
        if target > 1:
            f = Filter(rate, 24, 24, 80)
            ticks = 0
            per_tick = max(1, 1000 // rate)
            while f.tick(per_tick, 0)[0] * 10 < target * 6 and ticks < rate:
                ticks += 1
            ms = (ticks * 1000) // rate
            if not 40 <= ms <= 160:  # 80 ms nominal, generous window for integer effects
                problems.append(
                    f"rate={rate}: reaching 60 % took {ms} ms, expected about 80 ms - the time "
                    f"constant is behaving like a tick count")
    return problems


def main():
    groups = (
        ("defaults reproduce the pre-refactor arithmetic", check_equivalence()),
        ("anti-deadzone properties", check_anti_deadzone()),
        ("sensitivity is independent of the task rate", check_rate_independence()),
        ("filter resolution, centring and time constant", check_filter_behaviour()),
    )

    failed = False
    for name, problems in groups:
        if problems:
            failed = True
            print(f"FAIL: {name}")
            for p in problems:
                print(f"  {p}")
        else:
            print(f"ok:   {name}")

    if failed:
        print("\nOne of the model and the firmware is wrong. They are separate copies of the "
              "same arithmetic, so check which.")
        return 1
    print("\nOK: mapper arithmetic behaves as intended, and the defaults are unchanged.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
