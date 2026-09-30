#!/usr/bin/env python3
"""Check the dynamic touch inventory using real Android pointer events.

Uses the installed build, a local Dust II match and the existing netconsole
runner. Saves HUD snapshots and screenshots, and stops its game on completion.
No arguments prints the procedure; --run executes it on the connected phone.
"""

import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("touch_gameplay", ROOT / "scripts/test-android-gameplay.py")
gameplay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gameplay)


class TouchRun(gameplay.DeviceRun):
    def hud(self):
        reply = self.command("mobileui_status")
        header = re.search(r"MOBILE_HUD: (.*)", reply)
        if not header:
            raise RuntimeError("The installed game has no mobile HUD diagnostics; see commands.log")
        state = dict(re.findall(r"(\w+)=(\S+)", header[1]))
        controls = {}
        for name, values in re.findall(r"MOBILE_CONTROL: (\S+) (.*)", reply):
            fields = dict(re.findall(r"(\w+)=(\S+)", values))
            fields["rect"] = [float(v) for v in fields["rect"].split(",")]
            for key in ("action", "entity", "count", "enabled", "active", "equipped", "clip", "reserve"):
                fields[key] = int(fields[key])
            controls[name] = fields
        state["controls"] = controls
        return state

    def until(self, predicate, description, seconds=6):
        end = time.monotonic() + self.remaining(seconds)
        while time.monotonic() < end:
            state = self.hud()
            if predicate(state):
                return state
            self.pause(.1)
        raise AssertionError(description)

    def foreground(self):
        focus = self.shell("dumpsys", "window")
        if not re.search(r"mCurrentFocus=.*" + re.escape(self.package) + r"/.*GameActivity", focus):
            raise RuntimeError("Game lost foreground focus; touch injection stopped")

    def point(self, control):
        button = self.hud()["controls"].get(control)
        if not button or not button["enabled"]:
            raise AssertionError(f"Touch control {control} is absent or disabled")
        x, y, w, h = button["rect"]
        return round((x + w / 2) * self.size[0]), round((y + h / 2) * self.size[1])

    def tap(self, control):
        x, y = self.point(control)
        self.foreground()
        self.shell("input", "tap", str(x), str(y))

    def equip(self, control):
        self.tap(control)
        return self.until(lambda s: s["controls"].get(control, {}).get("equipped") == 1,
                          f"Tapping {control} did not equip its weapon")

    def gesture(self, steps):
        self.foreground()
        path = self.output / "gesture.json"
        path.write_text(json.dumps(steps) + "\n")
        remote = "/data/local/tmp/csgo-touch-check.json"
        subprocess.run(self.adb + ["push", str(path), remote], capture_output=True,
                       check=True, timeout=self.remaining())
        reply = self.shell("env", "CLASSPATH=/data/local/tmp/csgo-touch.dex", "app_process",
                           "/system/bin", "com.csgosource.tests.TouchInput", remote)
        if "ANDROID_TOUCH_PASS:" not in reply:
            raise AssertionError("Android pointer injection failed: " + reply)

    def snapshot(self, name):
        state = self.hud()
        (self.output / (name + ".json")).write_text(json.dumps(state, indent=2) + "\n")
        self.capture(name)
        return state


def exercise(run, result):
    result.update(run.start())
    print("Game ready; loading touch inventory check", flush=True)
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
    run.until(lambda s: s["alive"] == "1", "Player did not spawn")
    run.command("ammo_grenade_limit_total 6", "ammo_grenade_limit_flashbang 2",
                "give weapon_ak47", "give weapon_p250", "give weapon_taser",
                "give weapon_smokegrenade", "give weapon_flashbang", "give weapon_flashbang",
                "give weapon_hegrenade", "give weapon_molotov", "use weapon_ak47",
                "setpos_exact -533 -754 117.296204", "setang 0 37 0", "gameui_hide")
    run.pause(1.5)
    state = run.snapshot("inventory-primary")
    png = (run.output / "inventory-primary.png").read_bytes()
    run.size = struct.unpack(">II", png[16:24])
    required = {"primary", "pistol", "knife", "taser", "grenade", "smoke", "flash", "molotov"}
    assert required <= state["controls"].keys(), "Owned equipment missing from touch inventory"
    assert not {"healthshot", "shield", "incendiary", "decoy"} & state["controls"].keys(), "Unowned equipment is visible"
    assert state["controls"]["flash"]["count"] == 2, "Flashbang quantity is not two"
    result["checks"].append("owned inventory and separate grenade quantities; unowned equipment hidden")

    for control in ("pistol", "primary", "knife", "taser", "flash", "smoke"):
        state = run.equip(control)
        selected = [name for name, button in state["controls"].items() if button["equipped"]]
        assert selected == [control], f"Ambiguous equipment selection: {selected}"
        run.pause(.15)
        run.snapshot("selected-" + control)
    result["checks"].append("real taps select exact primary/pistol/knife/taser/flash/smoke entities")
    print("Weapon taps passed; checking consumption and simultaneous touches", flush=True)

    run.pause(.9)
    run.tap("fire")
    state = run.until(lambda s: "smoke" not in s["controls"], "Consumed smoke grenade stayed visible")
    assert state["controls"]["flash"]["count"] == 2, "Throwing smoke changed the flashbang quantity"
    run.snapshot("smoke-consumed")
    run.equip("flash")
    run.pause(.9)
    run.tap("fire")
    run.until(lambda s: s["controls"].get("flash", {}).get("count") == 1,
              "Throwing one flashbang did not retain a single-flash control")
    run.snapshot("one-flash-remaining")
    result["checks"].append("throwing consumes only that item; last smoke hides and one flash remains selectable")

    run.equip("primary")
    run.pause(1)
    state = run.hud()
    primary_clip = state["controls"]["primary"]["clip"]
    pistol_clip = state["controls"]["pistol"]["clip"]
    fx, fy = run.point("fire")
    px, py = run.point("pistol")
    run.gesture([
        {"action": "down", "id": 0, "x": fx, "y": fy}, {"wait": 350},
        {"action": "down", "id": 1, "x": px, "y": py}, {"wait": 80},
        {"action": "up", "id": 1}, {"wait": 350}, {"action": "up", "id": 0},
    ])
    state = run.until(lambda s: s["controls"].get("pistol", {}).get("equipped") == 1,
                      "Second finger did not switch to the pistol")
    assert state["controls"]["primary"]["clip"] < primary_clip, "Held fire did not shoot"
    assert state["controls"]["pistol"]["clip"] == pistol_clip, "Old fire contact shot the newly equipped pistol"
    run.pause(1)
    state = run.snapshot("fire-switch-released")
    assert int(state["held"], 16) == 0 and state["controls"]["pistol"]["clip"] == pistol_clip, "Input remained held after release"
    result["checks"].append("two-finger fire/switch releases old attack without firing the new weapon")

    run.tap("duck")
    state = run.until(lambda s: s["controls"]["duck"]["active"] == 1, "Crouch did not toggle")
    assert not state["controls"]["duck"]["equipped"], "Action toggle inherited weapon selection"
    run.tap("duck")
    run.until(lambda s: s["controls"]["duck"]["active"] == 0, "Crouch did not release")
    run.tap("menu")
    run.pause(.2)
    run.capture("pause-menu")
    run.command("gameui_hide")
    run.pause(.2)
    state = run.snapshot("resumed")
    assert int(state["held"], 16) == 0, "Menu transition retained a touch"
    result["checks"].append("crouch toggle and pause/resume release input independently of weapon selection")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--config", choices=("release", "debug"), default="release")
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or not 30 <= args.timeout <= 600:
        parser.error("--timeout must be between 30 and 600 seconds")
    if not args.run:
        print("Touch check: owned inventory; exact weapon taps; smoke/flash consumption; "
              "two-finger fire/switch; crouch and menu reset.\n"
              "Run build-android.sh test-mobile to prepare the Android injector, install the matching APK, "
              "then pass --run. Screenshots include icon, caption and ammunition selection colors.")
        return 0
    if os.environ.get("CONTAINER_ID") != "dev":
        os.execvp("distrobox", ["distrobox", "enter", "-T", "-n", "dev", "--",
                                "python3", str(Path(__file__).resolve()), *sys.argv[1:]])
    output = (args.output or ROOT / "runtime/android/touch" / time.strftime("%Y%m%dT%H%M%S")).resolve()
    if (output / "result.json").exists():
        parser.error("Choose a new output directory to preserve earlier results")
    output.mkdir(parents=True, exist_ok=True)
    result = {"passed": False, "checks": [], "configuration": args.config,
              "visual_review": ["gold icon, caption and ammo on selected equipment", "legible action icons"]}
    run = TouchRun(args, output)
    try:
        exercise(run, result)
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
