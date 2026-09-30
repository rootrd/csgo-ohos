#!/usr/bin/env python3
"""Validate the SDL3 game with a local map, captures, input and window changes."""

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import sys
import time


def main():
    script = Path(__file__).resolve()
    if os.environ.get("CONTAINER_ID") != "dev":
        os.execvp("distrobox", ["distrobox", "enter", "-T", "-n", "dev", "--",
                                "python3", str(script), *sys.argv[1:]])
    from PIL import Image, ImageStat

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shaderapi", default="shaderapidx9_client.so")
    parser.add_argument("--map", default="de_dust2")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--validation", action="store_true", help="Enable native Vulkan validation")
    parser.add_argument("--skip-keyboard", action="store_true",
                        help="Skip keyboard injection, for example when a desktop IME intercepts keys")
    parser.add_argument("--anisotropy", type=int, choices=(0, 1, 2, 4, 8, 16),
                        help="Use a fixed texture filtering setting for comparison")
    parser.add_argument("--shadow-scene", action="store_true",
                        help="Verify PC render-to-texture shadows with a frozen bot on Dust II")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_]+", args.map) or args.timeout <= 0:
        parser.error("Use a plain map name and a positive timeout")
    if args.shadow_scene and args.map != "de_dust2":
        parser.error("The fixed shadow scene requires de_dust2")
    repo = script.parent.parent
    output = (args.output or repo / "runtime/linux-sdl3/validation/game").resolve()
    if "vulkan" in args.shaderapi and output == (repo / "runtime/linux-sdl3/validation/game").resolve():
        parser.error("Use a separate --output directory for native Vulkan to preserve the DXVK comparison")
    output.mkdir(parents=True, exist_ok=True)
    (output / "local").mkdir(exist_ok=True)
    runtime = repo / "runtime/linux-sdl3/game"
    spec = importlib.util.spec_from_file_location("netconsole", script.with_name("test-netconsole.py"))
    netconsole = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(netconsole)
    with socket.socket() as available:
        available.bind(("127.0.0.1", 0))
        port = available.getsockname()[1]
    password = secrets.token_hex(24)
    environment = os.environ.copy()
    environment.update(CSGO_USE_STEAM="0", SDL_VIDEODRIVER="x11", USRLOCALCSGO=str(output / "local"))
    if args.shadow_scene and "vulkan" in args.shaderapi:
        environment["SOURCE_VULKAN_TRACE_DRAWS"] = "1"
    log = (output / "game.log").open("w")
    trace = (output / "commands.log").open("w")
    process = subprocess.Popen(
        ["bash", str(script.with_name("run-linux.sh")), "-windowed", "-w", "1280", "-h", "720", "-nomobileui",
         "-nojoy", "-condebug", "-shaderapi", args.shaderapi,
         "-netconport", str(port), "-netconpassword", password,
         "+mat_antialias", "0", "+mat_vsync", "1", "+fps_max", "60",
         *(["+mat_forceaniso", str(args.anisotropy)] if args.anisotropy is not None else []),
         *(["+r_shadowrendertotexture", "1", "+cl_csm_enabled", "0"] if args.shadow_scene else []),
         *( ["-vulkan-validation", "+mat_queue_mode", "0"] if args.validation else [] )],
        cwd=repo, env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    print(f"Game validation started: PID {process.pid}, {args.shaderapi}", flush=True)
    result = {"passed": False, "renderer": args.shaderapi, "map": args.map, "video_driver": "x11",
              "captures": {}, "checks": [], "skipped_checks": [], "timings_seconds": {}}
    if args.anisotropy is not None:
        result["requested_anisotropy"] = args.anisotropy
    connection = None

    def command(*commands, timeout=30):
        marker = "SDL3_CHECK_" + secrets.token_hex(8)
        trace.write("> " + "\n> ".join(commands) + "\n")
        trace.flush()
        connection.sendall(("\n".join(commands) + "\necho " + marker + "\n").encode())
        reply = netconsole.receive_until(connection, marker.encode(), timeout).decode(errors="replace")
        trace.write(reply.replace(marker, "") + "\n")
        trace.flush()
        return reply

    def capture(label, size):
        name = "sdl3_" + label + "_" + secrets.token_hex(4)
        target = runtime / "csgo/screenshots" / (name + ".tga")
        command("screenshot " + name)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                with Image.open(target) as screenshot:
                    screenshot.load()
                    image = screenshot.convert("RGB")
                break
            except (FileNotFoundError, OSError):
                if process.poll() is not None:
                    raise RuntimeError("Game exited before the screenshot was written")
                time.sleep(0.2)
        else:
            raise RuntimeError(f"Screenshot not written: {target}")
        if image.size != size:
            raise AssertionError(f"{label}: expected {size}, got {image.size}")
        if max(ImageStat.Stat(image).stddev) < 2:
            raise AssertionError(f"{label}: the framebuffer is nearly uniform")
        destination = output / (label + ".png")
        image.save(destination)
        result["captures"][label] = {"file": destination.name, "size": list(image.size),
                                     "sha256": hashlib.sha256(destination.read_bytes()).hexdigest()}
        memory = Path(f"/proc/{process.pid}/status").read_text()
        peak = re.search(r"^VmHWM:\s+(\d+)\s+kB$", memory, re.MULTILINE)
        if peak:
            result["process_peak_rss_kib"] = max(result.get("process_peak_rss_kib", 0), int(peak[1]))
        print(f"Captured {label}: {image.size[0]} x {image.size[1]}", flush=True)
        return image

    def position():
        reply = command("getpos")
        match = re.search(r"setpos(?:_exact)?\s+([-\d.e+]+)\s+([-\d.e+]+)\s+([-\d.e+]+)", reply)
        if not match:
            raise AssertionError("The client did not report a player position")
        return [float(value) for value in match.groups()]

    def xdo(*arguments):
        return subprocess.check_output(["xdotool", *map(str, arguments)], text=True, timeout=10).strip()

    try:
        deadline = time.monotonic() + args.timeout
        while connection is None:
            if process.poll() is not None:
                raise RuntimeError(f"Game exited during startup with status {process.returncode}; see game.log")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("Game startup timed out; see game.log")
            try:
                connection = netconsole.connect_authenticated(("127.0.0.1", port), password.encode(), min(3, remaining))
            except RuntimeError:
                continue
        print("Game and netconsole ready", flush=True)
        paths = sorted({line.split(None, 5)[5] for line in Path(f"/proc/{process.pid}/maps").read_text().splitlines()
                        if len(line.split(None, 5)) == 6 and line.split(None, 5)[5].startswith("/")})
        (output / "loaded-modules.txt").write_text("\n".join(paths) + "\n")
        artifacts = {}
        for name in (args.shaderapi, "stdshader_vulkan_client.so", "engine_client.so", "datacache_client.so", "client_client.so"):
            path = next((Path(path) for path in paths if Path(path).name == name), None)
            if path:
                artifacts[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        result["module_sha256"] = artifacts
        if not any("libSDL3.so" in path for path in paths):
            raise AssertionError("The game did not load SDL3")
        if any("libSDL2" in path or "libtogl" in path or "scaleformui" in path for path in paths):
            raise AssertionError("An SDL2/ToGL/Scaleform module was loaded")
        if args.shaderapi == "shaderapidx9_client.so" and not any("libdxvk_d3d9.so" in path for path in paths):
            raise AssertionError("The reference renderer did not load native DXVK")
        if "vulkan" in args.shaderapi and any("libdxvk_d3d9.so" in path or "shaderapidx9" in path for path in paths):
            raise AssertionError("The native Vulkan process loaded the reference D3D9 backend")
        if "vulkan" in args.shaderapi and not any("stdshader_vulkan_client.so" in path for path in paths):
            raise AssertionError("The native Vulkan material library was not loaded")
        result["checks"].append("game process uses SDL3 without SDL2/ToGL/Scaleform")
        command("mat_info")
        map_started = time.monotonic()
        deadline = map_started + args.timeout
        command("map " + args.map, timeout=args.timeout)
        print("Loading local map and waiting for client signon", flush=True)
        while time.monotonic() < deadline:
            # Map asset loading can occupy the main thread longer than the
            # ordinary command timeout. Use the remaining map-load deadline.
            status = command("status", timeout=max(1, deadline - time.monotonic()))
            # The listen server reports its map before the local client has
            # finished signon. Commands sent while it is still "spawning"
            # cannot reliably join a team or dismiss the loading screen.
            if (re.search(r"map\s*:\s*" + re.escape(args.map), status)
                    and re.search(r"^#\s+\d+.*\sactive\s+\d+\s+loopback\s*$", status, re.MULTILINE)):
                break
            time.sleep(1)
        else:
            raise AssertionError("The local client did not finish map signon")
        result["timings_seconds"]["map_load"] = time.monotonic() - map_started
        result["checks"].append("local client completed signon")
        command("sv_cheats 1", "bot_kick", "mp_autoteambalance 0", "mp_limitteams 0", "mp_freezetime 0",
                "mp_ignore_round_win_conditions 1", "mp_warmup_pausetimer 1", "jointeam 2",
                "bind w +forward", "gameui_hide")
        time.sleep(3)
        if args.map == "de_dust2":
            # A fixed unobstructed T-spawn view also makes renderer captures
            # comparable; a random spawn can face a wall during the input test.
            command("setpos_exact -533 -754 117.296204", "setang 0 37 0")
            time.sleep(2)
        result["initial_position"] = position()
        capture("map_hud", (1280, 720))
        result["checks"].append("local map and nonuniform game framebuffer")

        if args.shadow_scene:
            # Isolate the PC alpha-atlas shadow path. These fixture-only
            # settings are identical for native Vulkan and the DXVK reference;
            # they do not claim acceptance of CSM or full material parity.
            command("r_shadowrendertotexture 1", "r_shadows_gamecontrol 1", "r_shadows 1",
                    "r_shadow_half_update_rate 0", "r_shadowdist 200", "r_shadowfromworldlights 0",
                    "mat_force_tonemap_scale 1", "bot_stop 1", "bot_dont_shoot 1", "bot_add_t",
                    "setpos_exact 2449.14 2010.22 192.09", "setang 25 160 0")
            time.sleep(3)
            command("bot_place")
            time.sleep(3)
            result["shadow_scene"] = {"camera": position(), "settings": command(
                "r_shadowrendertotexture", "r_shadows_gamecontrol", "cl_csm_enabled",
                "r_shadow_half_update_rate", "r_shadowfromworldlights", "mat_force_tonemap_scale", "mat_forceaniso")}
            command("host_timescale 0", "r_shadows 0")
            time.sleep(1)
            off = capture("shadow_off", (1280, 720))
            command("r_shadows 1")
            time.sleep(1)
            on = capture("shadow_on", (1280, 720))
            command("r_shadows 0")
            time.sleep(1)
            repeated = capture("shadow_off_repeat", (1280, 720))
            command("host_timescale 1", "r_shadows 1")
            mask = Image.new("L", off.size)
            changed = []
            for index, (before, after, again) in enumerate(zip(off.getdata(), on.getdata(), repeated.getdata())):
                y = index // off.width
                stable = sum(abs(a - b) for a, b in zip(before, again)) <= 3
                darkened = sum(before) - sum(after) >= 12
                changed.append(255 if 216 <= y < 612 and stable and darkened else 0)
            mask.putdata(changed)
            mask.save(output / "shadow_difference.png")
            pixels = sum(value != 0 for value in changed)
            result["shadow_scene"].update(stable_darkened_pixels=pixels, difference_bounds=mask.getbbox())
            if pixels < 64:
                raise AssertionError("The frozen bot did not produce a measurable projected shadow")
            result["checks"].append("frozen bot casts a measurable render-to-texture shadow; off/on/off readback is stable")

        windows = xdo("search", "--onlyvisible", "--pid", process.pid).splitlines()
        if not windows:
            raise AssertionError("No visible SDL3 game window found")
        window = windows[0]
        def check_window_size(expected):
            geometry = xdo("getwindowgeometry", "--shell", window)
            actual = tuple(int(re.search(r"^" + key + r"=(\d+)$", geometry, re.MULTILINE)[1])
                           for key in ("WIDTH", "HEIGHT"))
            if actual != expected:
                raise AssertionError(f"SDL3 window: expected {expected}, got {actual}")
        check_window_size((1280, 720))
        xdo("windowactivate", "--sync", window)
        command("gameui_hide")
        if args.skip_keyboard:
            result["skipped_checks"].append("keyboard movement (--skip-keyboard)")
        else:
            xdo("windowfocus", "--sync", window)
            # Window-manager activation and Source's focus-state reset are handled
            # by separate event loops. Let both settle before holding a key.
            xdo("keyup", "w")
            time.sleep(0.5)
            result["keyboard_focus"] = {"target": window, "active": xdo("getactivewindow").strip(),
                                         "focused": xdo("getwindowfocus").strip()}
            if result["keyboard_focus"]["focused"] != window:
                raise AssertionError("The SDL3 game window did not receive X11 keyboard focus")
            before = position()
            try:
                xdo("keydown", "w")
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    time.sleep(0.3)
                    if math.dist(before[:2], position()[:2]) >= 8:
                        break
            finally:
                xdo("keyup", "w")
            time.sleep(0.3)
            after = position()
            distance = math.dist(before[:2], after[:2])
            if distance < 1:
                # Keep keyboard acceptance strict, but distinguish an input/focus
                # failure from a player blocked by collision or round state.
                try:
                    command("+forward")
                    time.sleep(1)
                finally:
                    command("-forward")
                result["diagnostic_console_movement"] = math.dist(after[:2], position()[:2])
                raise AssertionError(f"SDL3 keyboard input did not move the player: {before} -> {after}")
            result["movement_distance"] = distance
            result["checks"].append("X11 keyboard event moves the player through SDL3")
        resize_started = time.monotonic()
        command("mat_setvideomode 1024 768 1", timeout=args.timeout)
        result["timings_seconds"]["video_mode_change"] = time.monotonic() - resize_started
        time.sleep(2)
        capture("resized", (1024, 768))
        check_window_size((1024, 768))
        xdo("windowminimize", window)
        time.sleep(1)
        xdo("windowmap", window)
        xdo("windowactivate", "--sync", window)
        time.sleep(2)
        command("gameui_hide")
        capture("restored", (1024, 768))
        result["checks"].append("resize and minimize/restore preserve rendering")
        command("disconnect")
        time.sleep(2)
        capture("console", (1024, 768))
        connection.sendall(b"quit\n")
        returncode = process.wait(timeout=30)
        if returncode != 0:
            raise AssertionError(f"Game exited with status {returncode}")
        result["exit_code"] = returncode
        result["checks"].append("disconnect and normal shutdown")
        if "vulkan" in args.shaderapi:
            diagnostics = (output / "game.log").read_text(errors="replace")
            shutdown = re.findall(r"VK_MODULE_SHUTDOWN: validation_errors=(\d+) live_allocations=(\d+)", diagnostics)
            if not shutdown or any(errors != "0" or live != "0" for errors, live in shutdown):
                raise AssertionError("Native Vulkan did not report clean resource shutdown")
            if "VK_VALIDATION_ERROR:" in diagnostics:
                raise AssertionError("The Vulkan validation layer reported an error")
            if args.validation and "validation=1 synchronization_validation=1" not in diagnostics:
                raise AssertionError("The requested Vulkan validation layers were not active")
            if args.shadow_scene:
                for shader in ("native_shadow_vs", "native_shadowbuild_vs"):
                    if "shader=" + shader not in diagnostics:
                        raise AssertionError("Shadow fixture did not draw the native shader " + shader)
            statistics = {}
            for name, prefix in (("module", "VK_MODULE_STATS"), ("uploads", "VK_UPLOAD_STATS"),
                                 ("queries", "VK_QUERY_STATS"), ("samplers", "VK_SAMPLER_STATS")):
                records = re.findall(prefix + r": ([^\r\n]+)", diagnostics)
                if records:
                    statistics[name] = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", records[-1])}
            result["native_statistics"] = statistics
            result["native_hdr_modes"] = [{"enabled": enabled == "1", "type": hdr_type, "lightmap_scale": float(scale)}
                for enabled, hdr_type, scale in re.findall(r"VK_HDR_MODE: enabled=(\d) type=(\w+) lightmap_scale=([\d.]+)", diagnostics)]
            result["native_mode_changes"] = [{"restore_ms": float(restore), "total_ms": float(total),
                "buffer_uploads_before": int(before), "buffer_uploads_after": int(after)}
                for restore, total, before, after in re.findall(
                    r"VK_MODE_CHANGE_DONE: restore_ms=([\d.]+) total_ms=([\d.]+) buffer_uploads_before=(\d+) buffer_uploads_after=(\d+)", diagnostics)]
            result["checks"].append("native Vulkan clean shutdown with no validation errors or live allocations")
        result["passed"] = True
        print("LINUX_RENDERER_PASS: map, captures, resize, restore and normal exit; keyboard " +
              ("skipped" if args.skip_keyboard else "passed"), flush=True)
    except Exception as error:
        result["error"] = str(error)
        print(f"LINUX_RENDERER_FAIL: {error}", flush=True)
    finally:
        if process.poll() is None:
            if connection:
                try:
                    connection.sendall(b"quit\n")
                    process.wait(timeout=30)
                except (OSError, subprocess.TimeoutExpired):
                    pass
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if connection:
            connection.close()
        result.setdefault("exit_code", process.returncode)
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        log.close()
        trace.close()
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
