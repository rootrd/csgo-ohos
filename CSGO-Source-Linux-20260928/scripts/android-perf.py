#!/usr/bin/env python3
"""Measure the game on the phone (needs root for GPU/thermal nodes).

  android-perf.py [--config release] [--map de_dust2] [--seconds 10] [--cmd 'cvar value' ...]

--map relaunches the game, loads the map through netconsole and reports startup and
map-load times. Frame statistics come from SurfaceFlinger present timestamps.
"""
import argparse, json, math, os, re, shlex, statistics, subprocess, sys, time
from pathlib import Path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADB = [os.path.join(os.environ.get("ANDROID_SDK_ROOT", "/home/deck/Code/Toolchains/android-sdk"), "platform-tools/adb")]


def sh(cmd, *, optional=False):
    result = subprocess.run(ADB + ["shell", cmd], capture_output=True, text=True, timeout=15)
    if result.returncode and not optional:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "ADB shell failed")
    return result.stdout


def console(config, *commands, timeout=60, optional=False):
    env = dict(os.environ, BUILD_CONFIG=config)
    try:
        result = subprocess.run(["bash", f"{ROOT}/scripts/build-android.sh", "console", *commands], env=env,
                                capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        if optional:
            return ""
        raise RuntimeError("Netconsole command timed out") from None
    if result.returncode and not optional:
        raise RuntimeError(result.stderr.strip() or "Netconsole command failed")
    return result.stdout


def wait(predicate, limit):
    start = time.monotonic()
    while time.monotonic() - start < limit:
        if predicate():
            return time.monotonic() - start
        time.sleep(0.5)
    sys.exit("timed out")


def layer(config):
    package = "com.csgosource.android" + (".debug" if config == "debug" else "")
    matches = set()
    for line in sh("dumpsys SurfaceFlinger --list").splitlines():
        match = re.search(r"\b[0-9a-f]+ SurfaceView\[" + re.escape(package) + r"/[^\]]+\]\(BLAST\)#\d+", line)
        if match:
            matches.add(match.group(0))
    if len(matches) != 1:
        raise RuntimeError(f"Expected one {config} game Surface, found {len(matches)}")
    return next(iter(matches))


def latency(name):
    lines = sh("dumpsys SurfaceFlinger --latency " + shlex.quote(name)).splitlines()
    period = int(lines[0]) if lines and lines[0].strip().isdigit() else 0
    stamps = set()
    for line in lines[1:]:
        parts = line.split()
        if len(parts) == 3 and all(p.isdigit() for p in parts):
            present = int(parts[1])
            if 0 < present < 2**62:  # Ignore unpresented frames and INT64_MAX fences.
                stamps.add(present)
    return period, stamps


def frame_stats(stamps, period):
    stamps = sorted(set(stamps))
    if len(stamps) < 3:
        raise RuntimeError("Not enough new frames presented during the measurement")
    deltas = sorted((b - a) / 1e6 for a, b in zip(stamps, stamps[1:]))
    pct = lambda q: deltas[min(len(deltas) - 1, int(q * len(deltas)))]
    span = (stamps[-1] - stamps[0]) / 1e9
    return {"display_hz": 1e9 / period if period else None, "fps": len(deltas) / span,
            "presented_frames": len(stamps), "observed_seconds": span,
            "frametime_ms": {"p50": pct(.5), "p90": pct(.9), "p99": pct(.99), "max": deltas[-1]}}


def telemetry_command():
    # Resolve thermal paths once; scanning every thermal zone in each polling
    # iteration can overrun SurfaceFlinger's 128-frame history at high FPS.
    nodes = {"gpu_busy_pct": "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
             "gpu_hz": "/sys/class/kgsl/kgsl-3d0/gpuclk",
             "cpu0_khz": "/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq",
             "cpu6_khz": "/sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq"}
    discover = ('for z in /sys/class/thermal/thermal_zone*; do t=$(cat "$z/type"); '
                'case "$t" in cpu-1-0-0|gpuss-0|shell_front|shell_back) echo "$t $z/temp";; esac; done')
    for line in sh("su -c " + shlex.quote(discover), optional=True).splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1].startswith("/sys/class/thermal/"):
            nodes[fields[0] + "_millic"] = fields[1]
    script = "; ".join("if [ -r " + shlex.quote(v) + " ]; then printf '%s ' " + shlex.quote(k) +
                       "; cat " + shlex.quote(v) + "; fi" for k, v in nodes.items())
    return "su -c " + shlex.quote(script)


def measure(seconds, config):
    name = layer(config)
    sample_command = telemetry_command()
    package = "com.csgosource.android" + (".debug" if config == "debug" else "")
    pid = sh("pidof " + package + ":game").strip()
    period, previous = latency(name)
    cutoff = max(previous, default=0)
    presents, samples = set(), []
    start = time.monotonic()
    next_sample = start
    while time.monotonic() - start < seconds:
        period, current = latency(name)
        if previous and current and min(current) > max(previous):
            raise RuntimeError("SurfaceFlinger history overran between polls; this run cannot report reliable FPS")
        presents.update(t for t in current if t > cutoff)
        previous = current
        if time.monotonic() >= next_sample:
            sample = {"elapsed_seconds": time.monotonic() - start}
            for line in sh(sample_command, optional=True).splitlines():
                fields = line.split()
                if len(fields) >= 2 and fields[1].isdigit():
                    sample[fields[0]] = int(fields[1])
            samples.append(sample)
            next_sample = time.monotonic() + 1
        time.sleep(min(.2, max(0, seconds - (time.monotonic() - start))))
    if layer(config) != name or sh("pidof " + package + ":game").strip() != pid:
        raise RuntimeError("Game process or Surface changed during the measurement")
    stats = frame_stats(presents, period)
    tolerance = max(.5, 2 * stats["frametime_ms"]["p50"] / 1000)
    if stats["observed_seconds"] < seconds - tolerance:
        raise RuntimeError("New frame timestamps do not cover the measurement interval; check that the game stays in the foreground")
    means = {key: statistics.mean(s[key] for s in samples if key in s)
             for key in {k for s in samples for k in s} if key != "elapsed_seconds"}
    ft = stats["frametime_ms"]
    print(f"display {stats['display_hz'] or 0:.0f} Hz | {stats['fps']:.1f} fps | "
          f"frametime p50 {ft['p50']:.2f} p90 {ft['p90']:.2f} p99 {ft['p99']:.2f} max {ft['max']:.2f} ms")
    if "gpu_busy_pct" in means and "gpu_hz" in means:
        print(f"GPU busy {means['gpu_busy_pct']:.0f}% @ {means['gpu_hz'] / 1e6:.0f} MHz | " +
              " ".join(f"{k[:-7]}={v / 1000:.1f}C" for k, v in sorted(means.items()) if k.endswith("_millic")))
    # Busy percentage is utilization, not a GPU timestamp or per-frame duration.
    return {**stats, "configuration": config, "pid": pid, "surface": name,
            "requested_seconds": seconds, "telemetry_mean": means, "telemetry_samples": samples,
            "present_timestamps_ns": sorted(presents)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", choices=("debug", "release"), default=os.environ.get("BUILD_CONFIG", "release"))
    parser.add_argument("--map")
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--settle", type=float, default=5)
    parser.add_argument("--cmd", action="append", default=[])
    parser.add_argument("--output", type=Path, help="save statistics and raw timestamps/telemetry as JSON")
    parser.add_argument("--gpu-mhz", type=int, help="pin the GPU clock for comparable runs; 0 restores the governor")
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0 or not math.isfinite(args.settle) or args.settle < 0:
        parser.error("--seconds must be positive and --settle nonnegative")
    if args.gpu_mhz is not None and args.gpu_mhz < 0:
        parser.error("--gpu-mhz must be nonnegative")
    if args.map and not re.fullmatch(r"[A-Za-z0-9_-]{1,96}", args.map):
        parser.error("invalid map name")
    timings = {}
    if args.gpu_mhz is not None:
        # kgsl clamps these to the current thermal limit; 0 restores the full range.
        low, high = (160, 1100) if args.gpu_mhz == 0 else (args.gpu_mhz, args.gpu_mhz)
        sh(f"su -c 'cd /sys/class/kgsl/kgsl-3d0; echo 160 > min_clock_mhz; echo {high} > max_clock_mhz; echo {low} > min_clock_mhz'")
    if args.map:
        start = time.monotonic()
        subprocess.run(["bash", f"{ROOT}/scripts/build-android.sh", "run", "-devcvars"], env=dict(os.environ, BUILD_CONFIG=args.config),
                       check=True, capture_output=True)
        wait(lambda: "READY" in console(args.config, "echo READY", timeout=15, optional=True), 300)
        timings["startup_seconds"] = time.monotonic() - start
        print(f"startup: console ready after {timings['startup_seconds']:.1f} s")
        start = time.monotonic()
        console(args.config, "game_mode 1", "game_type 0", f"map {args.map}")
        wait(lambda: re.search(r"\bactive\b", console(args.config, "status", timeout=15, optional=True)), 600)
        timings["map_load_seconds"] = time.monotonic() - start
        print(f"map load: {args.map} active after {timings['map_load_seconds']:.1f} s")
    if args.cmd:
        console(args.config, *args.cmd)
    time.sleep(args.settle)
    result = measure(args.seconds, args.config)
    if args.output:
        result.update(timings)
        result["commands"] = args.cmd
        result["scene"] = console(args.config, "status", "getpos", "mat_viewportscale", "fps_max", timeout=15)
        log = sh("cat /storage/emulated/0/Games/CSGO/logs/launcher.log")
        build = re.search(r"CSGO Android (debug|release); build=([0-9a-f]+); pid=(\d+)", log)
        result["build_id"] = build[2] if build and build[1] == args.config and build[3] == result["pid"] else None
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(f"Saved {args.output}")


if __name__ == "__main__":
    if os.environ.get("CONTAINER_ID") != "dev":
        os.execvp("distrobox", ["distrobox", "enter", "-T", "-n", "dev", "--", "python3", __file__, *sys.argv[1:]])
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(f"[android-perf] {error}")
