#!/usr/bin/env python3
"""One device run: Vulkan uploads/readback, stable presentation and two resumes."""
import argparse
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
PACKAGE = "com.csgosource.android.debug"


def main():
    if os.environ.get("CONTAINER_ID") != "dev":
        os.execvp("distrobox", ["distrobox", "enter", "-T", "-n", "dev", "--",
                               "python3", str(pathlib.Path(__file__).resolve()), *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validation", action="store_true", help="Requires a packaged Khronos validation layer")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "runtime/vulkan/android/self-test")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sdk = pathlib.Path(os.environ.get("ANDROID_SDK_ROOT", "/home/deck/Code/Toolchains/android-sdk"))
    adb = [str(sdk / "platform-tools/adb")]
    if os.environ.get("ANDROID_SERIAL"):
        adb += ["-s", os.environ["ANDROID_SERIAL"]]
    serial = subprocess.check_output(adb + ["get-serialno"], text=True).strip()
    adb = [adb[0], "-s", serial]
    metadata = json.loads((ROOT / "runtime/android/debug/symbols/build-info.json").read_text())
    build_id = metadata["build_id"]
    environment = {**os.environ, "BUILD_CONFIG": "debug", "ANDROID_SERIAL": serial}

    def shell(*command, check=True):
        return subprocess.run(adb + ["shell", shlex.join(command)], text=True, capture_output=True, check=check).stdout.strip()

    def private_file(name):
        result = subprocess.run(adb + ["exec-out", "run-as", PACKAGE, "cat", "files/" + name], capture_output=True)
        return result.stdout if result.returncode == 0 else b""

    def log():
        text = private_file("launcher.log").decode(errors="replace")
        (args.output / "launcher.log").write_text(text)
        return text

    def wait_for(predicate, seconds, message):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            text = log()
            if f"build={build_id};" in text and ("VK_PROBE_FAIL" in text or "STARTUP_FAILED:" in text):
                raise RuntimeError(text)
            if predicate(text):
                return text
            time.sleep(0.5)
        raise RuntimeError(message + "\n" + log())

    def screenshot(name):
        (args.output / name).write_bytes(subprocess.check_output(adb + ["exec-out", "screencap", "-p"]))

    logcat = None
    launched = False
    try:
        launch = ["bash", str(ROOT / "scripts/build-android.sh"), "vulkan-probe", "35"]
        if args.validation:
            launch.append("--validation")
        subprocess.run(launch, cwd=ROOT, env=environment, check=True)
        launched = True
        text = wait_for(lambda text: f"build={build_id};" in text and
                        "VK_RESOURCE_TEST_PASS: swapchain=1;" in text and
                        f"pid={shell('pidof', PACKAGE + ':game', check=False)};" in text,
                        15, "Current Debug APK did not present its first Vulkan frame")
        pid = re.search(r"; pid=(\d+);", text)[1]
        logcat = subprocess.Popen(adb + ["logcat", "--pid=" + pid, "-v", "threadtime"],
                                  stdout=(args.output / "logcat.txt").open("wb"), stderr=subprocess.STDOUT)
        screenshot("initial.png")
        for cycle in range(1, 3):
            previous = max(map(int, re.findall(r"VK_SWAPCHAIN_READY: generation=(\d+)", log())))
            shell("input", "keyevent", "KEYCODE_HOME")
            wait_for(lambda text: text.count("VK_SURFACE_SUSPENDED:") >= cycle,
                     8, "Backgrounding did not release presentation resources")
            time.sleep(1)
            shell("am", "start", "-W", "-a", "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER",
                  "-f", "0x10208000", "-n", PACKAGE + "/com.csgosource.android.LauncherActivity",
                  "--ez", "auto_start", "true", "--ez", "vulkan_probe", "true")
            wait_for(lambda text: any(int(generation) > previous for generation in
                     re.findall(r"VK_RESOURCE_TEST_PASS: swapchain=(\d+);", text)),
                     8, "Resume did not revalidate the original uploaded resources")
            if shell("pidof", PACKAGE + ":game", check=False) != pid:
                raise RuntimeError("Android replaced the game process during resume")
            print(f"[vulkan-test] Resume {cycle}: same PID {pid}; GPU resources verified", flush=True)
        screenshot("resumed.png")
        text = wait_for(lambda text: "VK_PROBE_PASS:" in text, 40, "Vulkan diagnostic did not finish")
        if text.count("VK_DEVICE_READY:") != 1:
            raise RuntimeError("Resume recreated the Vulkan device")
        generations = list(map(int, re.findall(r"VK_SWAPCHAIN_READY: generation=(\d+)", text)))
        if max(generations) != 3:
            raise RuntimeError(f"Expected initial presentation plus two resumes; saw {max(generations)} swapchains")
        if "VK_VALIDATION_ERROR:" in text:
            raise RuntimeError("Vulkan validation errors are present")
        if args.validation and "validation=1" not in text:
            raise RuntimeError("Validation was requested but did not run")
        (args.output / "vulkan-readback.ppm").write_bytes(private_file("vulkan-readback.ppm"))
        summary = {"serial": serial, "pid": int(pid), "build_id": build_id,
                   "resume_cycles": 2, "device_creations": 1, "swapchains": max(generations),
                   "validation": args.validation,
                   "result": re.search(r"^VK_PROBE_PASS:.*$", text, re.M)[0]}
        (args.output / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
        print("[vulkan-test] " + summary["result"] + "\nArtifacts: " + str(args.output), flush=True)
    finally:
        if logcat:
            logcat.terminate()
            logcat.wait(timeout=5)
        if launched:
            log()
            error = private_file("error.txt")
            if error:
                (args.output / "error.txt").write_bytes(error)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, KeyError, ValueError) as error:
        sys.exit("[vulkan-test] " + str(error))
