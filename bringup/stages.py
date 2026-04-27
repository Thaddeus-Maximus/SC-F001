"""One function per bring-up stage. Each is explicit and independently
runnable from the operator prompt — no implicit sequencing between them."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

import fmt
from protocol import Link, Response, Event


@dataclass
class Tally:
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    warnings: int = 0

    def note_pass(self) -> None: self.passed += 1
    def note_fail(self) -> None: self.failed += 1
    def note_skip(self) -> None: self.skipped += 1
    def note_warn(self) -> None: self.warnings += 1


def _prompt(msg: str, accept_skip: bool = True) -> str:
    """Block for operator input. Returns 'run', 'skip', or 'quit'."""
    hint = fmt.dim(" [Enter=run" + (", s=skip" if accept_skip else "") + ", q=quit]")
    ans = input(fmt.prompt(msg) + hint + ": ").strip().lower()
    if ans in ("", "y", "yes", "r", "run"):
        return "run"
    if ans in ("s", "skip") and accept_skip:
        return "skip"
    if ans in ("q", "quit", "exit"):
        return "quit"
    # anything else — treat as run, operator can always quit with Ctrl-C
    return "run"


def _show_response(label: str, r: Response) -> None:
    bag = " ".join(f"{k}={v}" for k, v in r.fields.items())
    print(f"  {fmt.status_tag(r.status)} {label}  {fmt.dim(bag)}")


# ---------------------------------------------------------------------------
# Stage 0 — Begin + identify
# ---------------------------------------------------------------------------

def stage_begin(link: Link, t: Tally) -> None:
    print(fmt.stage("Stage 0 — Begin"))
    print("  Waiting for device to finish booting ...")
    r = link.wait_ready("BU.BEGIN", per_attempt_s=1.5, overall_timeout_s=30.0)
    _show_response("begin", r)
    if r.status != "OK":
        t.note_fail(); raise SystemExit("Device did not enter bring-up mode")
    r = link.request("BU.INFO")
    _show_response("info", r)
    (t.note_pass if r.status == "OK" else t.note_fail)()
    print(f"  -> fw={r.get('fw')}  board={r.get('board', '?')}  reset={r.get('reset')}  "
          f"heap={r.get('heap')}")


# ---------------------------------------------------------------------------
# Stage 1 — Flash & persistence
# ---------------------------------------------------------------------------

def stage_flash(link: Link, t: Tally) -> None:
    print(fmt.stage("Stage 1 — Flash & storage"))
    if _prompt("  Run flash roundtrip + log head/tail check") != "run":
        t.note_skip(); return
    r = link.request("BU.FLASH", overall_timeout_s=10)
    _show_response("flash", r)
    (t.note_pass if r.status == "OK" else t.note_fail)()


# ---------------------------------------------------------------------------
# Stage 2 — I2C + LEDs
# ---------------------------------------------------------------------------

def stage_i2c_led(link: Link, t: Tally) -> None:
    import threading

    print(fmt.stage("Stage 2 — I2C / TCA9555 / LEDs"))
    if _prompt("  Probe TCA9555 and run LED check") != "run":
        t.note_skip(); return

    r = link.request("BU.I2C")
    _show_response("i2c", r)
    if r.status != "OK":
        t.note_fail(); return
    t.note_pass()

    # Firmware-driven LED watch: all LEDs solid when the button is released,
    # fast waterfall while held. Operator actuates the button and confirms.
    def reader() -> None:
        try:
            for item in link.request_stream("BU.LED.WATCH",
                                            overall_timeout_s=3600):
                if isinstance(item, Event) and item.cmd == "led":
                    state = "PRESSED" if item.fields.get("pressed") == "1" else "released"
                    print(f"    [led] button {state}")
                elif isinstance(item, Response):
                    return
        except Exception as e:  # pragma: no cover — defensive
            print(f"    [led-reader] {e!r}")

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    print("  LEDs should be SOLID (all on) when button is released.")
    print("  Press-and-hold the button: LEDs should WATERFALL at ~3× speed.")
    try:
        while True:
            ans = input("  LEDs behaved correctly? [y/n]: ").strip().lower()
            if ans.startswith("y"):
                verdict = "pass"; break
            if ans.startswith("n"):
                verdict = "fail"; break
    finally:
        link.send("")  # any byte aborts BU.LED.WATCH
        th.join(timeout=3)

    if verdict == "pass":
        t.note_pass()
    else:
        print(f"  {fmt.fail('LED visual check FAILED')}")
        t.note_fail()


# ---------------------------------------------------------------------------
# Stage 3 — ADC + battery calibration
# ---------------------------------------------------------------------------

def stage_adc(link: Link, t: Tally, calibrate: bool = True) -> None:
    print(fmt.stage("Stage 3 — Analog front-end"))
    if _prompt("  Read ADC snapshot (battery / motor current)") != "run":
        t.note_skip(); return

    r = link.request("BU.ADC")
    _show_response("adc", r)
    if r.status != "OK":
        t.note_fail(); return
    t.note_pass()

    bat_V = r.getf("bat_V", 0.0)
    print(f"  -> battery reports {bat_V:.3f} V")

    # VOC and FAULT pins on V5 are unusable (wired direct to input-only
    # ESP32 GPIOs, no external resistors — see README "V5 hardware caveats").
    # They're intentionally ignored here.

    if not calibrate:
        return
    _run_battery_cal(link, t)


def _run_battery_cal(link: Link, t: Tally) -> None:
    from calibrate import single_point_offset, verify

    print(fmt.section("Battery voltage calibration"))
    while True:
        ans = input("  Run calibration now? [y/n]: ").strip().lower()
        if ans.startswith("n"):
            t.note_skip(); return
        if ans.startswith("y"):
            break

    # Read current K and raw mV.
    k_r = link.request("BU.PARAM GET V_SENS_K")
    if k_r.status != "OK":
        print("  Could not read V_SENS_K"); t.note_fail(); return
    k = k_r.getf("value")

    adc_r = link.request("BU.ADC")
    bat_mv = adc_r.getf("bat_mv")
    if bat_mv == 0.0:
        print("  ADC read looks bogus (mv=0)"); t.note_fail(); return

    raw_ans = input("  Measure the battery at the board terminals with a DMM.\n"
                    "  Enter true voltage (V): ").strip()
    try:
        v_true = float(raw_ans)
    except ValueError:
        print("  Not a number — skipping cal"); t.note_skip(); return

    cal = single_point_offset(bat_mv, v_true, k)
    predicted = verify(bat_mv, cal)
    print(f"  bat_mv={bat_mv:.0f}  K={k:.10f}  new OFFSET={cal.offset:+.6f} V")
    print(f"  predicted V_bat after cal = {predicted:.3f}  (true = {v_true:.3f})")

    if input("  Write this to the device? [y/n]: ").strip().lower().startswith("y"):
        wr = link.request(f"BU.PARAM SET V_SENS_OFFSET {cal.offset:.6f}")
        _show_response("param.set", wr)
        if wr.status != "OK":
            t.note_fail(); return
        # Verify by re-reading the ADC
        check = link.request("BU.ADC")
        new_V = check.getf("bat_V")
        err = new_V - v_true
        print(f"  Post-cal bat_V = {new_V:.3f}  (err {err*1000:+.1f} mV)")
        if abs(err) < 0.05:
            t.note_pass()
        else:
            print("  WARN: residual error > 50 mV")
            t.note_warn()
    else:
        print("  Calibration not written (operator declined)")
        t.note_skip()


# ---------------------------------------------------------------------------
# Stage 4 — Discrete sensors (mandatory edges)
# ---------------------------------------------------------------------------

SENSOR_NAMES = ["SAFETY"]  # JACK and DRIVE are checked via the relay pulse stage.


def stage_sensors(link: Link, t: Tally) -> None:
    """Live-print safety-sensor edges until operator presses Enter.

    Drive and jack sensors are encoder-style and only trip while the motor
    runs — they're verified as a side effect of Stage 5 relay pulses.
    """
    import threading

    print(fmt.stage("Stage 4 — Sensor live view"))
    print("  Live state of all 4 sensor pins is printed below when any one")
    print("  changes. Per-edge events also print as they arrive.")
    print("  Poke each sensor by hand / magnet / jumper to verify it responds.")
    print("  SAFETY must show both break and make to pass; the others are")
    print("  diagnostic only (drive/jack are properly tested in Stage 5).")
    print("  Press Enter when you're satisfied.")

    state = {"make": False, "break": False}

    last_state_line = {"v": ""}

    def reader() -> None:
        try:
            for item in link.request_stream("BU.SENSORS.WATCH 0",
                                            overall_timeout_s=3600):
                if isinstance(item, Event):
                    if item.cmd == "sensor":
                        name = item.fields.get("name")
                        edge = item.fields.get("edge")
                        if name == "SAFETY" and edge in state:
                            state[edge] = True
                        print(f"    [{name}] {edge}")
                    elif item.cmd == "state":
                        # Live snapshot of all four sensors. Only print when
                        # the level line changes, so steady state doesn't spam.
                        f = item.fields
                        line = (f"SAFETY={f.get('SAFETY','?')} "
                                f"DRIVE={f.get('DRIVE','?')} "
                                f"JACK={f.get('JACK','?')} "
                                f"AUX={f.get('AUX','?')}  "
                                f"isr=(s={f.get('isr_s','?')} "
                                f"d={f.get('isr_d','?')} "
                                f"j={f.get('isr_j','?')} "
                                f"a={f.get('isr_a','?')})")
                        if line != last_state_line["v"]:
                            print(f"    [state] {line}")
                            last_state_line["v"] = line
                elif isinstance(item, Response):
                    # terminating OK after we aborted
                    return
        except Exception as e:  # pragma: no cover — defensive
            print(f"    [reader] {e!r}")

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    input("  Press Enter when SAFETY has been actuated: ")

    # Kick the firmware out of its watch loop: any byte aborts.
    link.send("")  # just the \n
    th.join(timeout=3)

    if state["make"] and state["break"]:
        print(f"  SAFETY: {fmt.pass_('PASS')} (saw break and make)")
        t.note_pass()
    else:
        missing = [k for k, v in state.items() if not v]
        print(f"  SAFETY: {fmt.fail('FAIL')} — missed {missing}")
        t.note_fail()


# ---------------------------------------------------------------------------
# Stage 5 — Relay bridges
# ---------------------------------------------------------------------------

#  (bridge, dir, ms, (dI_min, dI_max), check_edges)
#  check_edges → bridge has an encoder-style sensor; pulse must produce
#                at least one edge on it.
RELAY_TESTS = [
    ("SENSORS", "ON",   200, (0.0, 0.0),  False),
    ("DRIVE",   "FWD",  1000, (0.5, 25.0), True),
    ("DRIVE",   "REV",  1000, (0.5, 25.0), True),
    ("JACK",    "UP",   500, (0.2, 25.0), True),
    ("JACK",    "DOWN", 500, (0.2, 25.0), True),
    ("AUX",     "FWD",  150, (0.1, 25.0), False),
]


def stage_relays(link: Link, t: Tally) -> None:
    print(fmt.stage("Stage 5 — Relay bridges"))
    print("  PRECONDITIONS:")
    print("   - Battery connected, fuse in place")
    print("   - Drive wheels off ground / disengaged")
    print("   - Safety interlock asserted (SAFETY sensor HIGH)")
    if _prompt("  Proceed with live relay tests", accept_skip=True) != "run":
        print("  Relay stage SKIPPED"); t.note_skip(); return

    for bridge, direction, ms, (lo, hi), check_edges in RELAY_TESTS:
        prompt = f"  Pulse {bridge} {direction} for {ms} ms"
        if _prompt(prompt) != "run":
            t.note_skip(); continue
        r = link.request(f"BU.RELAY {bridge} {direction} {ms}",
                         overall_timeout_s=ms / 1000.0 + 5.0)
        _show_response(f"{bridge}/{direction}", r)
        if r.status == "SKIP":
            print("  Device refused (safety?)"); t.note_skip(); continue
        if r.status != "OK":
            t.note_fail(); continue
        if bridge == "SENSORS":
            t.note_pass(); continue

        i_before = r.getf("I_before")
        i_mid    = r.getf("I_mid")
        delta = abs(i_mid - i_before)
        tripped = r.geti("tripped") == 1
        edges = r.geti("edges")
        edge_str = f"  edges={edges}" if check_edges else ""
        print(f"    |ΔI| = {delta:.2f} A (expected {lo}-{hi})  "
              f"tripped={tripped}{edge_str}")

        if tripped:
            print(f"    {fmt.fail('FAIL')}: efuse tripped"); t.note_fail(); continue
        if check_edges and edges <= 0:
            print(f"    {fmt.fail('FAIL')}: {bridge} sensor saw no edges — motor not turning?")
            t.note_fail(); continue
        if lo <= delta <= hi:
            t.note_pass()
        else:
            print(f"    {fmt.warn('WARN')}: ΔI outside nominal")
            t.note_warn()


# ---------------------------------------------------------------------------
# Stage 6 — Radio & connectivity
# ---------------------------------------------------------------------------

def stage_rf(link: Link, t: Tally) -> None:
    import threading

    print(fmt.stage("Stage 6a — RF 433 MHz"))
    if _prompt("  Watch for RF remote codes") != "run":
        t.note_skip(); return

    print("  Press buttons on the RF remote. Codes will print live.")
    print("  Press Enter to stop.")

    count = {"seen": 0}

    def reader() -> None:
        try:
            for item in link.request_stream("BU.RF.WATCH 0",
                                            overall_timeout_s=3600):
                if isinstance(item, Event) and item.cmd == "rf":
                    code = item.fields.get("code", "?")
                    print(f"    rf code={code}")
                    count["seen"] += 1
                elif isinstance(item, Response):
                    return
        except Exception as e:  # pragma: no cover
            print(f"    [reader] {e!r}")

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    input("  Press Enter when done: ")
    link.send("")          # abort the watch
    th.join(timeout=3)

    print(f"  -> {count['seen']} code(s) captured")
    (t.note_pass if count["seen"] > 0 else t.note_warn)()


def stage_wifi(link: Link, t: Tally) -> None:
    print(fmt.stage("Stage 6b — WiFi + web UI"))
    if _prompt("  Start SoftAP and wait for a client to load the web UI") != "run":
        t.note_skip(); return
    r = link.request("BU.WIFI.START", overall_timeout_s=20)
    _show_response("wifi.start", r)
    if r.status != "OK":
        t.note_fail(); return
    ssid = r.get("ssid", "?")
    print(f"\n  Connect a device to WiFi SSID `{ssid}` and open http://192.168.4.1/")
    print("  Waiting for a client to associate and load the page... (Ctrl+C to abort)")
    try:
        for item in link.request_stream("BU.WIFI.WAIT", overall_timeout_s=3600):
            if isinstance(item, Event):
                print(f"    {item.cmd}  {' '.join(f'{k}={v}' for k,v in item.fields.items())}")
            elif isinstance(item, Response):
                _show_response("wifi.wait", item)
                (t.note_pass if item.status == "OK" else t.note_fail)()
                break
    except KeyboardInterrupt:
        print("  WiFi wait aborted by operator"); t.note_skip()


# ---------------------------------------------------------------------------
# End
# ---------------------------------------------------------------------------

def stage_end(link: Link, t: Tally) -> None:
    print(fmt.stage("Stage — End"))
    if _prompt("  Exit bring-up mode (device will reboot)") != "run":
        print("  Leaving device in bring-up mode — reset manually to resume normal firmware.")
        return
    r = link.request("BU.END", overall_timeout_s=5)
    _show_response("end", r)
    # Device reboots; no further response expected.


# ---------------------------------------------------------------------------
# Top-level driver
# ---------------------------------------------------------------------------

Stage = Callable[[Link, Tally], None]


def all_stages(skip_relays: bool = False, no_calibrate: bool = False) -> list[Stage]:
    stages: list[Stage] = [
        stage_begin,
        stage_flash,
        stage_i2c_led,
        lambda link, t: stage_adc(link, t, calibrate=not no_calibrate),
        stage_sensors,
    ]
    if not skip_relays:
        stages.append(stage_relays)
    stages.extend([
        stage_rf,
        stage_wifi,
        stage_end,
    ])
    return stages
