#!/usr/bin/env python3
"""Prepare, or explicitly run, a bounded Android recoil and buy-menu check.

Without --run this only prints the test plan; it never contacts the phone.
--run starts the installed APK, uses Dust II, saves evidence, and stops the app.
It does not install an APK or iterate over maps.
"""

import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import secrets
import shlex
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def punch_values(reply):
    values = {}
    for name in ("m_aimPunchAngle", "m_viewPunchAngle", "m_aimPunchAngleVel"):
        match = re.search(r"::" + name + r"\s+vector\s*\(([^)]+)\)", reply)
        if not match:
            raise RuntimeError(f"cl_dumpplayer did not report {name}; see commands.log")
        vector = [float(value) for value in match[1].split()]
        if len(vector) != 3 or not all(math.isfinite(value) for value in vector):
            raise RuntimeError(f"Invalid player state: {name}={vector}")
        values[name] = vector
    return values


def check_recoil(samples, recovered):
    if max(math.dist(s["m_aimPunchAngle"], [0, 0, 0]) for s in samples) < .1:
        raise AssertionError("Sustained firing produced no aim recoil")
    if max(math.dist(s["m_viewPunchAngle"], [0, 0, 0]) for s in samples) < .01:
        raise AssertionError("Sustained firing produced no visual view punch")
    if any(math.dist(value, [0, 0, 0]) > .05 for value in recovered.values()):
        raise AssertionError("Recoil did not settle after releasing fire")


class DeviceRun:
    def __init__(self, args, output):
        self.args = args
        self.output = output
        self.package = "com.csgosource.android" + (".debug" if args.config == "debug" else "")
        sdk = Path(os.environ.get("ANDROID_SDK_ROOT", "/home/deck/Code/Toolchains/android-sdk"))
        self.adb = [str(sdk / "platform-tools/adb")]
        if os.environ.get("ANDROID_SERIAL"):
            self.adb += ["-s", os.environ["ANDROID_SERIAL"]]
        self.connection = None
        self.started = False
        self.port = None
        self.deadline = time.monotonic() + args.timeout
        self.received = ""

    def remaining(self, limit=10):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Total test time limit reached")
        return min(limit, remaining)

    def shell(self, *args, check=True):
        result = subprocess.run(self.adb + ["shell", shlex.join(args)], capture_output=True,
                                text=True, timeout=self.remaining(), check=check)
        return result.stdout

    def alive(self):
        return bool(self.shell("pidof", self.package + ":game", check=False).strip())

    def pause(self, seconds):
        time.sleep(min(seconds, self.remaining(seconds)))

    def start(self):
        serial = subprocess.check_output(self.adb + ["get-serialno"], text=True,
                                         timeout=self.remaining()).strip()
        self.adb = [self.adb[0], "-s", serial]
        if self.alive():
            raise RuntimeError("The game is already running; close it before starting this test")
        # Own a separate forwarding/password pair; other console sessions are
        # left alone, and cleanup can remove precisely this test's forwarding.
        self.port = int(subprocess.check_output(self.adb + ["forward", "tcp:0", "tcp:27991"],
                                                text=True, timeout=self.remaining()))
        password = secrets.token_hex(16)
        # Load the map through the console after startup, so readiness is measured
        # separately and a failed startup never waits through a map-load timeout.
        engine_args = ["-netconport", "27991", "-netconpassword", password, "-devcvars"]
        self.started = True
        launch = self.shell("am", "start", "-W", "-S", "-a", "android.intent.action.MAIN",
                            "-c", "android.intent.category.LAUNCHER", "-f", "0x10208000",
                            "-n", self.package + "/com.csgosource.android.LauncherActivity",
                            "--ez", "auto_start", "true", "--es", "engine_args_json", json.dumps(engine_args))
        (self.output / "launch.log").write_text(launch)
        spec = importlib.util.spec_from_file_location("gameplay_netconsole", ROOT / "scripts/test-netconsole.py")
        net = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(net)
        startup_deadline = min(self.deadline, time.monotonic() + 30)
        while time.monotonic() < startup_deadline:
            if not self.alive():
                raise RuntimeError("Game exited during startup")
            try:
                self.connection = net.connect_authenticated(("127.0.0.1", self.port),
                                                           password.encode(), self.remaining(2))
                break
            except RuntimeError:
                continue
        if self.connection is None:
            raise TimeoutError("Game console did not become ready within 30 seconds")
        log = self.shell("cat", "/storage/emulated/0/Games/CSGO/logs/launcher.log")
        (self.output / "startup.log").write_text(log)
        build = re.search(r"CSGO Android (debug|release); build=([0-9a-f]+); pid=(\d+)", log)
        expected = json.loads((ROOT / "runtime/android" / self.args.config / "symbols/build-info.json").read_text())
        if not build or build[1] != self.args.config or build[2] != expected["build_id"]:
            raise RuntimeError("Installed APK does not match local build-info.json; test stopped")
        return {"serial": serial, "build_id": build[2], "pid": int(build[3])}

    def command(self, *commands):
        marker = ("GAMEPLAY_" + secrets.token_hex(8)).encode()
        self.connection.sendall(("\n".join(commands) + "\necho ").encode() + marker + b"\n")
        received = bytearray()
        with (self.output / "commands.log").open("a") as log:
            log.write("> " + "\n> ".join(commands) + "\n")
            log.flush()
            while marker not in received:
                self.connection.settimeout(self.remaining(1))
                try:
                    data = self.connection.recv(65536)
                except socket.timeout:
                    if not self.alive():
                        raise RuntimeError("Game exited while executing console commands")
                    continue
                if not data:
                    raise RuntimeError("Game disconnected the console")
                received.extend(data)
                log.write(data.decode(errors="replace"))
                log.flush()
        reply = received.decode(errors="replace")
        self.received += reply
        return reply

    def capture(self, name):
        focus = self.shell("dumpsys", "window")
        if not re.search(r"mCurrentFocus=.*" + re.escape(self.package) + r"/.*GameActivity", focus):
            raise RuntimeError("Game lost foreground focus; test stopped")
        image = subprocess.check_output(self.adb + ["exec-out", "screencap", "-p"], timeout=self.remaining())
        if not image.startswith(b"\x89PNG\r\n\x1a\n"):
            raise RuntimeError("Device did not return a PNG screenshot")
        (self.output / (name + ".png")).write_bytes(image)

    def close(self):
        try:
            if self.connection:
                self.connection.close()
        finally:
            # This is independent of the test deadline: even a failed command or
            # Ctrl-C must release the phone. Only this test's package is stopped.
            try:
                if self.started:
                    subprocess.run(self.adb + ["shell", "am", "force-stop", self.package],
                                   check=True, capture_output=True, timeout=5)
            finally:
                if self.port is not None:
                    subprocess.run(self.adb + ["forward", "--remove", f"tcp:{self.port}"],
                                   check=False, capture_output=True, timeout=3)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="explicitly allow this test to start and stop the installed game")
    parser.add_argument("--config", choices=("debug", "release"), default="release")
    parser.add_argument("--timeout", type=float, default=120, help="total phone test limit in seconds, excluding final cleanup")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or not 15 <= args.timeout <= 600:
        parser.error("--timeout must be between 15 and 600 seconds")
    if not args.run:
        print(f"Prepared {args.config} test (phone untouched):\n"
              "  1. Start the installed APK and verify its build ID.\n"
              "  2. Load Dust II, join T, equip an AK-47.\n"
              "  3. Check aim/view punch during firing and recovery after release.\n"
              "  4. Open/close the buy menu and save screenshots.\n"
              f"  5. Stop the game on success, failure, or the {args.timeout:g}s time limit.\n"
              "Use --run only when the phone is available. No APK is installed by this script.")
        return 0
    if os.environ.get("CONTAINER_ID") != "dev":
        os.execvp("distrobox", ["distrobox", "enter", "-T", "-n", "dev", "--",
                                "python3", str(Path(__file__).resolve()), *sys.argv[1:]])
    metadata = ROOT / "runtime/android" / args.config / "symbols/build-info.json"
    if not metadata.is_file():
        parser.error("Build/package the APK first; local build-info.json is missing")
    output = (args.output or ROOT / "runtime/android/gameplay" / time.strftime("%Y%m%dT%H%M%S")).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "result.json").exists():
        parser.error("Choose a new output directory to preserve earlier results")
    result = {"passed": False, "checks": [], "configuration": args.config,
              "visual_review": ["viewmodel animation and muzzle movement", "buy menu layout and touch targets"]}
    run = DeviceRun(args, output)
    try:
        result.update(run.start())
        print("Game ready; loading Dust II", flush=True)
        run.command("game_type 0", "game_mode 1", "bot_quota 0", "map de_dust2")
        while True:
            status = run.command("status")
            if (re.search(r"map\s*:\s*de_dust2\b", status) and
                    re.search(r"^#\s+\d+.*\sactive\s+\d+\s+loopback\s*$", status, re.M)):
                break
            run.pause(.2)
        run.command("sv_cheats 1", "bot_kick", "mp_autoteambalance 0", "mp_limitteams 0",
                    "mp_freezetime 0", "mp_ignore_round_win_conditions 1", "mp_warmup_pausetimer 1",
                    "mp_buytime 9999", "mp_buy_anywhere 1", "jointeam 2", "gameui_hide")
        run.pause(2)
        run.command("give weapon_ak47", "use weapon_ak47", "setpos_exact -533 -754 117.296204", "setang 0 37 0", "gameui_hide")
        run.pause(2)
        result["idle"] = punch_values(run.command("cl_dumpplayer 1"))
        run.capture("idle")
        samples = []
        try:
            run.command("+attack")
            for _ in range(3):
                run.pause(.35)
                samples.append(punch_values(run.command("cl_dumpplayer 1")))
            run.capture("firing")
        finally:
            try:
                run.command("-attack")
            except (OSError, RuntimeError):
                # The outer cleanup also stops the process if its console died
                # or the total deadline elapsed while firing.
                pass
        run.pause(4)
        recovered = punch_values(run.command("cl_dumpplayer 1"))
        result.update(firing=samples, recovered=recovered)
        check_recoil(samples, recovered)
        result["checks"].append("aim recoil, visual view punch, and recovery after firing")
        print("Recoil check passed; checking buy menu", flush=True)
        offset = len(run.received)
        run.command("buymenu")
        run.pause(.3)
        run.command("echo BUY_MENU_CHECK")
        if "MOBILE_UI_POINTER: menu menus=1" not in run.received[offset:]:
            raise AssertionError("Buy menu did not acquire pointer input")
        run.capture("buy_menu")
        offset = len(run.received)
        run.command("buymenu 0")
        run.pause(.3)
        run.command("echo BUY_MENU_CLOSED")
        if "MOBILE_UI_POINTER: game menus=0" not in run.received[offset:]:
            raise AssertionError("Closing the buy menu did not restore game input")
        result["checks"].append("buy menu opens and closes with paired pointer input capture")
        result["passed"] = True
    except (Exception, KeyboardInterrupt) as error:
        result["error"] = str(error) or type(error).__name__
    finally:
        try:
            run.close()
        except (OSError, subprocess.SubprocessError) as error:
            result.update(passed=False, cleanup_error=str(error))
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(("PASS" if result["passed"] else "FAIL") + ": " + str(output), flush=True)
    if "error" in result:
        print(result["error"], file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
