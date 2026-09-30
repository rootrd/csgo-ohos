#!/usr/bin/env python3
"""Compare CSM instancing on an installed Release APK at de_inferno CT spawn.

Records native
whole-CSM timestamps separately from FPS runs, which have profiling disabled.
GPU clock limits and display settings are restored even if a measurement fails.
"""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    if os.environ.get("CONTAINER_ID") != "dev":
        os.execvp("distrobox", ["distrobox", "enter", "-T", "-n", "dev", "--",
                                "python3", str(Path(__file__).resolve()), *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launch", action="store_true", help="install and launch the built Release APK")
    parser.add_argument("--no-install", action="store_true", help="reuse the installed APK when launching")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--gpu-mhz", type=int, default=389)
    parser.add_argument("--profile-frames", type=int, default=45)
    parser.add_argument("--cases", default="0,1,0,1")
    parser.add_argument("--compact-cases", help="compare compact vertex modes 0/1/2 with instancing enabled; captures upload data at map load")
    parser.add_argument("--culling-cases", help="compare legacy (0) and base-frustum fallback (1) CSM culling with instancing enabled")
    parser.add_argument("--culling-instancing", type=int, choices=(0, 1), default=1,
                        help="instancing mode for --culling-cases (default: 1)")
    parser.add_argument("--defer-flush-cases", help="compare DXVK implicit-flush deferral 0/1 with default model rendering; skips images")
    parser.add_argument("--model-sort-cases", help="compare opaque model order 0/1/2; skips images")
    parser.add_argument("--settle", type=float, default=1)
    parser.add_argument("--warmup", type=float, default=5)
    parser.add_argument("--diagnose", action="store_true", help="temporarily isolate geometry, pixels and post-processing costs")
    parser.add_argument("--shading", action="store_true", help="isolate CSM receiving on world/models and texture filtering, while preserving all caster draws")
    parser.add_argument("--breakdown", action="store_true", help="temporarily remove one main-view stage per case to split the non-CSM cost")
    parser.add_argument("--pass-profile", type=int, default=0, metavar="FRAMES",
                        help="also record DXVK per-render-pass GPU timestamps for FRAMES frames in every case")
    parser.add_argument("--shader-profile", action="store_true", help="split pass-profile pipeline counts by fragment shader (diagnostic overhead; no images)")
    parser.add_argument("--counts-only", action="store_true", help="collect pipeline counts without measuring FPS or changing GPU/display clock limits")
    parser.add_argument("--background-after", action="store_true")
    parser.add_argument("--skip-images", action="store_true")
    args = parser.parse_args()
    if args.shader_profile:
        if args.pass_profile <= 0:
            parser.error("--shader-profile requires --pass-profile FRAMES")
        args.skip_images = True
    if args.counts_only:
        if args.pass_profile <= 0:
            parser.error("--counts-only requires --pass-profile FRAMES")
        args.skip_images = True
    modes = [int(mode) for mode in args.cases.split(",")]
    if not modes or any(mode not in (0, 1) for mode in modes):
        parser.error("--cases must be a comma-separated list of 0 and 1")
    compact_modes = [int(mode) for mode in args.compact_cases.split(",")] if args.compact_cases else None
    culling_modes = [int(mode) for mode in args.culling_cases.split(",")] if args.culling_cases else None
    flush_modes = [int(mode) for mode in args.defer_flush_cases.split(",")] if args.defer_flush_cases else None
    sort_modes = [int(mode) for mode in args.model_sort_cases.split(",")] if args.model_sort_cases else None
    if sort_modes and any(mode not in (0, 1, 2) for mode in sort_modes):
        parser.error("--model-sort-cases requires modes 0/1/2")
    if flush_modes and any(mode not in (0, 1) for mode in flush_modes):
        parser.error("--defer-flush-cases requires modes 0/1")
    if flush_modes or sort_modes:
        args.skip_images = True
    if culling_modes and any(mode not in (0, 1) for mode in culling_modes):
        parser.error("--culling-cases requires modes 0/1")
    if compact_modes and (args.diagnose or any(mode not in (0, 1, 2) for mode in compact_modes)):
        parser.error("--compact-cases requires modes 0/1/2 and cannot be combined with --diagnose")
    if sum(map(bool, (args.shading, args.diagnose, compact_modes, culling_modes, flush_modes, sort_modes, args.breakdown))) > 1:
        parser.error("the diagnostic case selectors are mutually exclusive")
    if args.gpu_mhz <= 0 or args.profile_frames < 0 or any(
            not math.isfinite(value) or value < 0 for value in (args.seconds, args.settle, args.warmup)) or args.seconds == 0:
        parser.error("invalid timing or frequency parameter")
    started = time.monotonic()
    args.output.mkdir(parents=True, exist_ok=True)
    perf = load_module("csm_perf", ROOT / "scripts/android-perf.py")
    net = load_module("csm_net", ROOT / "scripts/test-netconsole.py")
    config = dict(os.environ, BUILD_CONFIG="release")
    package = "com.csgosource.android"
    results = {"gpu_mhz_requested": None if args.counts_only else args.gpu_mhz,
               "counts_only": args.counts_only, "cases": [], "profiles": [], "complete": False}
    shading_settings = {}
    base_culling_setting = None
    flush_setting = None
    model_sort_setting = None
    display_settings = {}
    shader_profile_setting = None
    stage_defaults = ["r_3dsky 1", "r_drawworld 1", "r_drawtranslucentworld 1", "r_drawtranslucentrenderables 1",
                      "r_drawviewmodel 1", "r_drawparticles 1", "cl_drawhud 1", "r_drawropes 1", "r_drawsprites 1"]

    def save():
        (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")

    def command(*commands):
        return perf.console("release", *commands)

    def focused():
        text = perf.sh("dumpsys window | grep mCurrentFocus")
        if "u0 " + package + "/" not in text or "GameActivity" not in text:
            raise RuntimeError("Game lost focus; measurement is invalid")

    def pass_profile(name):
        tag = f"{name}-{time.monotonic_ns()}"
        perf.sh("logcat -c")
        perf.sh(f"setprop debug.csgo.dxvk_passes {args.pass_profile}:{tag}")
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            time.sleep(1)
            focused()
            text = perf.sh("logcat -d -s CSGO-DXVK-Passes:I")
            lines = [line[line.index("[pass profile]"):] for line in text.splitlines()
                     if "[pass profile] tag=" + tag + " " in line]
            if any(line.endswith(" end") for line in lines):
                print("\n".join(lines), flush=True)
                return lines
        raise RuntimeError("No DXVK pass profile for " + name)

    def capture(name):
        focused()
        with (args.output / name).open("wb") as output:
            subprocess.run(perf.ADB + ["exec-out", "screencap", "-p"], stdout=output, check=True, timeout=30)

    def profile(frames=None):
        frames = args.profile_frames if frames is None else frames
        session = json.loads((ROOT / "runtime/android/release/netconsole.json").read_text())
        # The preceding console command has already restored the ADB forwarding.
        with net.connect_authenticated(("127.0.0.1", session["port"]), session["password"].encode(), 10) as connection:
            if sort_modes:
                connection.sendall(b"developer 1\nr_model_depth_sort_stats 3\n")
            connection.sendall(f"r_csm_profile {frames}\n".encode())
            text = net.receive_until(connection, b"[CSM profile] mode=", 45)
            while b"\n" not in text[text.index(b"[CSM profile] mode="):]:
                connection.settimeout(5)
                text += connection.recv(65536)
            if sort_modes:
                marker = f"__CSM_SORT_RESTORED_{time.monotonic_ns()}__"
                connection.sendall(f"developer {display_settings['developer']}\necho {marker}\n".encode())
                text += net.receive_until(connection, marker.encode(), 10)
        reply = text.decode(errors="replace")
        rows = [line for line in reply.splitlines() if "[CSM profile] mode=" in line]
        if len(rows) != 1:
            raise RuntimeError("No complete GPU profile result: " + reply)
        values = {key: float(value) for key, value in re.findall(r"(\w+)=([\d.]+)", rows[0])}
        if values.get("invalid") != 0 or values.get("samples") != frames:
            raise RuntimeError("GPU timestamp samples were invalid: " + rows[0])
        print(rows[0], flush=True)
        return {"raw": rows[0], **values, "model_sort_details": [line for line in reply.splitlines() if "[model depth sort]" in line]}

    if "mWakefulness=Awake" not in perf.sh("dumpsys power | grep mWakefulness="):
        perf.sh("input keyevent KEYCODE_WAKEUP")
        time.sleep(1)
    if "mKeyguardShowing=true" in perf.sh("dumpsys activity activities | grep -m1 mKeyguardShowing"):
        perf.sh("wm dismiss-keyguard")
        time.sleep(2)
        if "mKeyguardShowing=true" in perf.sh("dumpsys activity activities | grep -m1 mKeyguardShowing"):
            raise RuntimeError("The phone is locked; unlock it before measuring")
    if args.launch:
        # A completed one-shot request must not run again in a new DXVK context.
        perf.sh("setprop debug.csgo.dxvk_passes 0")
        actions = [] if args.no_install else [["install"]]
        capture_args = ["+r_csm_compact_vertices", "2", "+r_csm_instancing", "1"] if compact_modes else []
        actions.append(["run", "-devcvars", *capture_args, "+game_type", "0", "+game_mode", "1", "+map", "de_inferno"])
        for action in actions:
            subprocess.run(["bash", str(ROOT / "scripts/build-android.sh"), *action],
                           env=config, check=True, capture_output=True, timeout=300)
        start = time.monotonic()
        while time.monotonic() - start < 300:
            reply = perf.console("release", "status", timeout=15, optional=True)
            if "de_inferno" in reply and " active " in reply:
                break
            if time.monotonic() - start > 10:
                focus = perf.sh("dumpsys window | grep mCurrentFocus")
                if package + "/" in focus and "LauncherActivity" in focus:
                    raise RuntimeError("Game returned to the launcher while loading; inspect Games/CSGO/logs/error.txt")
            time.sleep(2)
        else:
            raise RuntimeError("de_inferno did not finish loading")

    focused()
    restore_keys = ("fps_max", "mat_vsync", "mat_viewportscale", "joystick") + (("developer",) if sort_modes else ())
    reply = command(*restore_keys)
    for key in restore_keys:
        match = re.search(r'"?' + re.escape(key) + r'"?\s*=\s*"?([-\d.]+)', reply)
        if not match:
            raise RuntimeError("Cannot record display/input setting: " + key)
        display_settings[key] = match[1]
    results["display_settings_before"] = display_settings
    settings = {key: perf.sh("settings get system " + key).strip()
                for key in ("peak_refresh_rate", "min_refresh_rate")}
    clocks = perf.sh("su -c 'cat /sys/class/kgsl/kgsl-3d0/min_clock_mhz /sys/class/kgsl/kgsl-3d0/max_clock_mhz'").split()
    if len(clocks) != 2 or not all(value.isdigit() for value in clocks):
        raise RuntimeError("Cannot record GPU clock limits for restoration")
    results["previous_clock_limits"] = clocks
    try:
        if not args.counts_only:
            for key in settings:
                perf.sh("settings put system " + key + " 120.0")
            perf.sh("su -c " + shlex.quote("cd /sys/class/kgsl/kgsl-3d0; echo 160 > min_clock_mhz; "
                    f"echo {args.gpu_mhz} > max_clock_mhz; echo {args.gpu_mhz} > min_clock_mhz"))
        command("sv_cheats 1", "bot_quota 0", "bot_kick", "mp_ignore_round_win_conditions 1",
                "mp_warmup_pausetimer 1", "jointeam 3", "joystick 0", "gameui_hide", "hideconsole")
        time.sleep(4)
        command("cl_lock_camera 0", "setpos 2449.14 2010.22 192.09", "setang 0 160 0",
                "r_csm_instancing 0", "cl_csm_enabled 1", "cl_csm_static_prop_shadows 1",
                "r_csm_compact_vertices 0",
                "r_csm_profile_no_sampling 0",
                "mat_viewportscale 0.75", "fps_max 0", "mat_vsync 0", "gameui_hide", "hideconsole")
        time.sleep(3)
        command("cl_lock_camera 1")
        results["scene"] = command("status", "getpos", "cl_modelfastpath", "mat_depthwrite_new_path", "r_csm_instancing")
        if culling_modes:
            reply = command("r_csm_base_frustum_culling")
            match = re.search(r'"?r_csm_base_frustum_culling"?\s*=\s*"?([01])', reply)
            if not match:
                raise RuntimeError("Cannot record CSM culling setting: " + reply)
            base_culling_setting = int(match[1])
            results["base_culling_setting_before"] = base_culling_setting
        log = perf.sh("cat /storage/emulated/0/Games/CSGO/logs/launcher.log")
        match = re.search(r"build=([0-9a-f]+)", log)
        results["build_id"] = match[1] if match else None
        time.sleep(args.warmup)
        if args.shader_profile:
            shader_profile_setting = perf.sh("getprop debug.csgo.dxvk_shader_stats").strip()
            results["shader_profile_setting_before"] = shader_profile_setting
            perf.sh("setprop debug.csgo.dxvk_shader_stats 1")
        cases = [(f"mode-{mode}", mode, []) for mode in modes]
        if sort_modes:
            reply = command("r_model_depth_sort")
            match = re.search(r'"?r_model_depth_sort"?\s*=\s*"?([012])', reply)
            if not match:
                raise RuntimeError("Cannot record model sorting setting: " + reply)
            model_sort_setting = int(match[1])
            results["model_sort_setting_before"] = model_sort_setting
            cases = [(f"model-sort-{mode}", 0, [f"r_model_depth_sort {mode}"]) for mode in sort_modes]
        if flush_modes:
            flush_setting = perf.sh("getprop debug.csgo.dxvk_defer_hints").strip()
            results["defer_flush_setting_before"] = flush_setting
            cases = [(f"defer-flush-{mode}", 0, []) for mode in flush_modes]
        if culling_modes:
            cases = [(f"culling-{mode}", args.culling_instancing, [f"r_csm_base_frustum_culling {mode}"]) for mode in culling_modes]
        if compact_modes:
            # Warm both buffer layouts before sampling; neither uploads nor first-use
            # driver compilation belong in the steady-state comparison.
            command("r_csm_instancing 1", "r_csm_compact_vertices 1")
            time.sleep(1)
            command("r_csm_compact_vertices 2")
            time.sleep(1)
            cases = [(f"compact-{mode}", 1, [f"r_csm_compact_vertices {mode}"]) for mode in compact_modes]
        if args.diagnose:
            cases = [("baseline", 1, []), ("shadow-lod2", 1, ["r_csm_profile_lod 2"]),
                     ("scale-50", 1, ["mat_viewportscale 0.5"]),
                     ("no-static-shadows", 1, ["cl_csm_static_prop_shadows 0"]),
                     ("no-static-props", 1, ["r_drawstaticprops 0"]),
                     ("no-post", 1, ["mat_postprocess_enable 0"]), ("baseline-end", 1, [])]
        if args.shading:
            for cvar in ("mat_forceaniso", "mat_filtertextures"):
                reply = command(cvar)
                match = re.search(r'"?' + re.escape(cvar) + r'"?\s*=\s*"?([0-9]+)', reply)
                if not match:
                    raise RuntimeError("Cannot record texture setting: " + reply)
                shading_settings[cvar] = int(match[1])
            results["shading_settings_before"] = shading_settings
            texture_case = ("no-aniso", 1, ["mat_forceaniso 1"]) if shading_settings["mat_forceaniso"] > 1 else (
                "point-filter", 1, ["mat_filtertextures 0"])
            cases = [("baseline", 1, []), ("no-world-receive", 1, ["r_csm_profile_no_sampling 1"]),
                     ("no-model-receive", 1, ["r_csm_profile_no_sampling 2"]),
                     ("no-csm-receive", 1, ["r_csm_profile_no_sampling 3"]), texture_case, ("baseline-end", 1, [])]
        if args.breakdown:
            cases = [("baseline", 1, []), ("no-3dsky", 1, ["r_3dsky 0"]), ("no-world", 1, ["r_drawworld 0"]),
                     ("no-translucent", 1, ["r_drawtranslucentworld 0", "r_drawtranslucentrenderables 0"]),
                     ("no-viewmodel", 1, ["r_drawviewmodel 0"]), ("no-particles", 1, ["r_drawparticles 0"]),
                     ("no-hud", 1, ["cl_drawhud 0"]), ("no-static-props", 1, ["r_drawstaticprops 0"]),
                     ("no-post", 1, ["mat_postprocess_enable 0"]), ("baseline-end", 1, [])]
        sampling_start = time.monotonic()
        for name, mode, extra in cases:
            if flush_modes:
                perf.sh(f"setprop debug.csgo.dxvk_defer_hints {name[-1]}")
            command(f"r_csm_instancing {mode}", "r_csm_compact_vertices 0", "r_csm_profile_lod -1", "mat_viewportscale 0.75",
                    "r_csm_profile_no_sampling 0", "cl_csm_static_prop_shadows 1", "r_drawstaticprops 1", "mat_postprocess_enable 1",
                    *(f"{key} {value}" for key, value in shading_settings.items()), *stage_defaults, *extra)
            time.sleep(args.settle)
            focused()
            measured = {}
            if not args.counts_only:
                measured = perf.measure(args.seconds, "release")
                focused()
                if abs(measured["telemetry_mean"].get("gpu_hz", 0) / 1e6 - args.gpu_mhz) > 5:
                    raise RuntimeError("GPU did not stay at the requested clock")
            results["cases"].append({"name": name, "mode": mode, "commands": extra, **measured})
            if flush_modes:
                lines = perf.sh("logcat -d -s CSGO-DXVK-Passes:I")
                results["cases"][-1]["flush_policy_log"] = [line for line in lines.splitlines() if "[flush policy]" in line]
            save()
            if args.profile_frames:
                row = profile()
                if sort_modes:
                    details = row["model_sort_details"]
                    sorted_counts = [int(x) for line in details for x in re.findall(r"sorted=(\d+)", line)]
                    if not sorted_counts or (name[-1] != "0" and max(sorted_counts) == 0):
                        raise RuntimeError("Model sorting diagnostic did not reach eligible meshes: " + str(details))
                if compact_modes and (row.get("inspected_instances", 0) == 0 or
                                      (int(name[-1]) and row.get("compact_draws", 0) == 0)):
                    raise RuntimeError("Compact geometry was not exercised; enable it before reloading the map")
                if args.shading and (row.get("world_sampling_sets", 0) == 0 or row.get("model_sampling_sets", 0) == 0):
                    raise RuntimeError("CSM receiving diagnostic did not reach both shader families")
                results["profiles"].append({"name": name, **row})
                save()
            if args.pass_profile:
                results.setdefault("pass_profiles", []).append({"name": name, "lines": pass_profile(name)})
                save()
        results["sampling_seconds"] = time.monotonic() - sampling_start
        if not args.skip_images:
            command("sv_pausable 1", "setpause", "cl_drawhud 0", "net_graph 0", "cl_csm_capture_state 1")
            time.sleep(2)
            image_cases = [("off-a", 0, 0), ("off-b", 0, 0), ("on-a", 1, 0), ("on-b", 1, 0)]
            if compact_modes:
                image_cases.extend([("packed-a", 1, 1), ("packed-b", 1, 1), ("welded-a", 1, 2), ("welded-b", 1, 2)])
            image_cases.append(("off-c", 0, 0))
            if culling_modes:
                image_cases = [(name, args.culling_instancing, 0) for name in ("legacy-a", "legacy-b", "base-a", "base-b", "legacy-c")]
            if args.shading:
                image_cases = [("sampling-on", 1, 0), ("sampling-off", 1, 0), ("sampling-restored", 1, 0)]
            if args.breakdown:
                image_cases = [("breakdown-end", 1, 0)]
            for name, mode, compact in image_cases:
                command(f"r_csm_instancing {mode}", f"r_csm_compact_vertices {compact}",
                        f"r_csm_profile_no_sampling {3 if name == 'sampling-off' else 0}",
                        *([f"r_csm_base_frustum_culling {0 if name.startswith('legacy-') else 1}"] if culling_modes else []))
                time.sleep(1)
                if name in ("off-a", "on-a", "packed-a", "welded-a", "legacy-a", "base-a") and args.profile_frames:
                    row = profile(10)
                    if row["model_draws"] == 0:
                        raise RuntimeError("Paused image would compare an old frame instead of rendering CSM")
                    if compact and row.get("compact_draws", 0) == 0:
                        raise RuntimeError("Paused image did not exercise compact geometry")
                    results.setdefault("image_profiles", []).append({"name": name, **row})
                capture(name + ".png")
        results["complete"] = True
    except Exception as error:
        results["error"] = str(error)
        raise
    finally:
        try:
            command("r_csm_instancing 0", "r_csm_compact_vertices 0", "r_csm_profile 0", "unpause", "cl_drawhud 1",
                    "r_csm_profile_no_sampling 0", *(f"{key} {value}" for key, value in shading_settings.items()),
                    "r_csm_profile_lod -1", "cl_csm_static_prop_shadows 1", "r_drawstaticprops 1",
                    "mat_postprocess_enable 1", "mat_viewportscale 0.75", *stage_defaults,
                    *([f"r_csm_base_frustum_culling {base_culling_setting}"] if base_culling_setting is not None else []),
                    *([f"r_model_depth_sort {model_sort_setting}", "r_model_depth_sort_stats 0"] if model_sort_setting is not None else []),
                    "cl_csm_clear_captured_state 1", "cl_lock_camera 0", "mat_vsync 1",
                    *(f"{key} {value}" for key, value in display_settings.items()))
        except Exception as error:
            results["cleanup_console_error"] = str(error)
        finally:
            if args.pass_profile:
                try:
                    perf.sh("setprop debug.csgo.dxvk_passes 0")
                except Exception as error:
                    results["cleanup_pass_profile_error"] = str(error)
            if shader_profile_setting is not None:
                try:
                    perf.sh("setprop debug.csgo.dxvk_shader_stats " + shlex.quote(shader_profile_setting))
                except Exception as error:
                    results["cleanup_shader_profile_error"] = str(error)
            if flush_setting is not None:
                try:
                    perf.sh("setprop debug.csgo.dxvk_defer_hints " + shlex.quote(flush_setting))
                except Exception as error:
                    results["cleanup_flush_error"] = str(error)
            if not args.counts_only:
                perf.sh("su -c " + shlex.quote("cd /sys/class/kgsl/kgsl-3d0; echo 160 > min_clock_mhz; "
                        f"echo {clocks[1]} > max_clock_mhz; echo {clocks[0]} > min_clock_mhz"))
                for key, value in settings.items():
                    perf.sh("settings delete system " + key if value in ("", "null")
                            else "settings put system " + key + " " + shlex.quote(value))
            results["total_seconds"] = time.monotonic() - started
            save()
            if args.background_after:
                try:
                    focused()
                except RuntimeError:
                    pass
                else:
                    perf.sh("input keyevent KEYCODE_HOME")
    print("Saved", args.output, flush=True)


if __name__ == "__main__":
    main()
