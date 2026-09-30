#!/usr/bin/env python3
"""Android build receipts, native-library staging and ADB debugging."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "runtime/android"
MODULES = (
    "tier0", "vstdlib", "filesystem_stdio", "launcher", "engine", "inputsystem",
    "vphysics", "materialsystem", "shaderapidx9", "datacache", "studiorender",
    "soundemittersystem", "vaudio_minimp3", "scenefilecache", "vscript", "vguimatsurface", "vgui2", "localize",
    "stdshader_dx9", "stdshader_dbg", "panorama", "panoramauiclient", "panorama_text_pango",
)
GAME_MODULES = ("client_panorama", "server")
SYSTEM = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libvulkan.so",
          "libz.so", "libaaudio.so", "libOpenSLES.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so"}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_id(scope="engine", config="release"):
    paths = ["src", "android/native/parsifal_expat.c", "android/patches"]
    if scope == "package":
        paths = ["src", "android/CMakeLists.txt", "android/native", "android/app", "android/patches",
                 "scripts/build-android.sh", "scripts/android-build-tools.py"]
    else:
        # The Source engine action builds shaderapidx9. The separate Vulkan
        # module/probe is built by native CMake and remains in the package
        # fingerprint; editing it must not invalidate every Source engine DLL.
        paths.append(":(exclude)src/materialsystem/shaderapivulkan")
    digest = hashlib.sha256(subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT))
    if scope == "package":
        digest.update(config.encode() + b"\0")
    digest.update(subprocess.check_output(["git", "diff", "HEAD", "--binary", "--", *paths], cwd=ROOT))
    untracked = subprocess.check_output(["git", "ls-files", "-z", "--others", "--exclude-standard", "--", *paths], cwd=ROOT)
    for name in sorted(untracked.split(b"\0")):
        if name:
            digest.update(name + b"\0")
            digest.update((ROOT / name.decode()).read_bytes())
    return digest.hexdigest()[:20]


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)


def module_paths(config):
    directory = ROOT / "game/bin/androidarm64" / config
    game = ROOT / "game/csgo/bin/androidarm64" / config
    return ([directory / f"lib{name}_client.so" for name in MODULES]
            + [game / f"lib{name}_client.so" for name in GAME_MODULES]
            + [game / "libmatchmaking_client.so"])


def receipt(config, expected_source):
    if expected_source != source_id():
        raise RuntimeError("Sources changed while building. Rerun the engine action before packaging.")
    libraries = {}
    for path in module_paths(config):
        if not path.is_file():
            raise RuntimeError(f"Required engine module was not built: {path}")
        libraries[path.name] = sha(path)
    if expected_source != source_id():
        raise RuntimeError("Sources changed while recording the build. Rerun the engine action.")
    data = {"configuration": config, "source_id": expected_source, "libraries": libraries}
    write_json(OUT / config / "engine-build.json", data)


def read_elf(path, readelf):
    text = subprocess.check_output([str(readelf), "-h", "-lW", "-d", "-n", "-V", str(path)], text=True)
    if "AArch64" not in text or re.search(r"GLIBC_|GLIBCXX_", text):
        raise RuntimeError(f"Non-Android ARM64 library: {path}")
    alignments = re.findall(r"^\s*LOAD\s+.*\s(0x[0-9a-f]+)\s*$", text, re.M)
    if not alignments or any(int(x, 16) < 16384 for x in alignments):
        raise RuntimeError(f"Library does not support 16 KiB pages: {path}")
    soname = re.search(r"\(SONAME\).*\[(.*?)\]", text)
    build_id = re.search(r"Build ID: (\w+)", text)
    return {"needed": re.findall(r"\(NEEDED\).*\[(.*?)\]", text),
            "soname": soname[1] if soname else None, "build_id": build_id[1] if build_id else None}


def validate_mobile_layout(path):
    root = ET.parse(path).getroot()
    for snippet in root.findall("./snippets/snippet"):
        panels = list(snippet)
        if len(panels) != 1 or panels[0].get("id"):
            raise RuntimeError(
                f"{path}: Panorama snippet {snippet.get('name', '<unnamed>')} must contain "
                "exactly one root panel without an id; place named controls inside it.")


def stage(config, ndk):
    for path in sorted((ROOT / "android/app/src/main/assets/mobile_ui").glob("*.xml")):
        validate_mobile_layout(path)
    variant = OUT / config
    record = json.loads((variant / "engine-build.json").read_text())
    if record["configuration"] != config or record["source_id"] != source_id():
        raise RuntimeError("Engine sources changed after the last successful build. Run the engine action first.")
    build_id = source_id("package", config)
    cache = (variant / "build/CMakeCache.txt").read_text()
    if not re.search(r"^CSGO_BUILD_ID:STRING=" + re.escape(build_id) + r"$", cache, re.M):
        raise RuntimeError("Android launcher sources changed after its build. Run the native action before packaging.")
    if build_id.encode() not in (variant / "build/libcsgo_android.so").read_bytes():
        raise RuntimeError("Android launcher compilation did not complete for this build. Run the native action.")
    roots = module_paths(config)
    for path in roots:
        if record["libraries"].get(path.name) != sha(path):
            raise RuntimeError(f"Engine module changed outside the recorded build: {path}")
    prefix = OUT / "install/lib"
    runtime = ndk / "toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
    roots += [variant / "build/libcsgo_android.so", prefix / "libSDL3.so", prefix / "libdxvk_d3d9.so", runtime]
    readelf = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    search = [prefix, ROOT / "src/lib/common/androidarm64"]
    selected = {path.name: path for path in roots}
    pending = list(selected)
    metadata = {}
    while pending:
        name = pending.pop()
        path = selected[name]
        elf = read_elf(path, readelf)
        if elf["soname"] and elf["soname"] != name:
            raise RuntimeError(f"Filename/SONAME mismatch: {path} declares {elf['soname']}")
        if name.endswith("_client.so") and not elf["build_id"]:
            raise RuntimeError(f"Engine module has no Build ID: {path}")
        metadata[name] = {**elf, "sha256": sha(path), "source": str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)}
        for dependency in elf["needed"]:
            if dependency in SYSTEM or dependency in selected:
                continue
            matches = [directory / dependency for directory in search if (directory / dependency).is_file()]
            if not matches:
                raise RuntimeError(f"{name} requires missing {dependency}; build its Android dependency first")
            selected[dependency] = matches[0]
            pending.append(dependency)
    destination = variant / "package/lib/arm64-v8a"
    destination.mkdir(parents=True, exist_ok=True)
    symbols = variant / "symbols"
    symbols.mkdir(parents=True, exist_ok=True)
    for name, path in selected.items():
        shutil.copy2(path, destination / name)
        # gendbg keeps the full symbol file next to a Source module; the CMake
        # bridge carries its own DWARF until the package copy is stripped.
        debug = pathlib.Path(str(path) + ".dbg")
        if debug.exists():
            shutil.copy2(debug, symbols / (name + ".dbg"))
        elif name.endswith("_client.so") or name == "libcsgo_android.so":
            shutil.copy2(path, symbols / name)
    info = {"configuration": config, "build_id": build_id, "source_id": record["source_id"], "libraries": metadata}
    write_json(variant / "package/assets/build-info.json", info)
    write_json(symbols / "build-info.json", info)
    manifest = ET.parse(ROOT / "android/app/AndroidManifest.xml")
    namespace = "{http://schemas.android.com/apk/res/android}"
    ET.register_namespace("android", namespace[1:-1])
    manifest.getroot().find("application").set(namespace + "debuggable", str(config == "debug").lower())
    manifest.write(variant / "package/AndroidManifest.xml", encoding="utf-8", xml_declaration=True)
    print(f"Staged {len(selected)} libraries for {config}, source {record['source_id']}")


def device_action(action, config, adb_path, arguments, probe, vulkan_probe=None, validation=False):
    package = "com.csgosource.android.debug" if config == "debug" else "com.csgosource.android"
    adb = [str(adb_path)]
    if os.environ.get("ANDROID_SERIAL"):
        adb += ["-s", os.environ["ANDROID_SERIAL"]]
    if validation and vulkan_probe is None:
        raise RuntimeError("--validation requires --vulkan-probe and a packaged Khronos validation layer.")
    duration = vulkan_probe if vulkan_probe is not None else probe
    if config != "debug" and duration is not None:
        raise RuntimeError("Graphics and Vulkan probes require BUILD_CONFIG=debug.")
    serial = subprocess.check_output(adb + ["get-serialno"], text=True).strip()
    adb = [str(adb_path), "-s", serial]
    session_path = OUT / config / "netconsole.json"
    if action == "console":
        if not session_path.is_file():
            raise RuntimeError(f"No {config} console session. Start the APK with the run action first.")
        session = json.loads(session_path.read_text())
        if session["serial"] != serial:
            raise RuntimeError("The console session belongs to another ADB device. Run the APK on this device first.")
        mapping = f"{serial} tcp:{session['port']} tcp:27991"
        mappings = subprocess.check_output(adb + ["forward", "--list"], text=True).splitlines()
        if mapping not in mappings:
            subprocess.run(adb + ["forward", "--no-rebind", f"tcp:{session['port']}", "tcp:27991"], check=True)
        command = [sys.executable, str(ROOT / "scripts/test-netconsole.py"),
                   "--port", str(session["port"]), "--password", session["password"], "--timeout", "30"]
        for argument in arguments:
            command += ["--command=" + argument]
        if not arguments:
            command += ["--interactive"]
        if subprocess.call(command):
            raise RuntimeError("Netconsole command failed; see the output above. Use diagnose for device logs.")
        return

    extras = []
    if config == "debug":
        installed = subprocess.check_output(adb + ["shell", "dumpsys", "package", package], text=True)
        if not re.search(r"^\s*(?:pkgFlags|flags)=\[[^\]]*\bDEBUGGABLE\b", installed, re.M):
            raise RuntimeError("The installed APK is not debuggable. Install it with BUILD_CONFIG=debug first.")
    if duration is not None:
        if arguments or not 1 <= duration <= 600:
            raise RuntimeError("Usage: probe [seconds from 1 to 600]; no engine arguments.")
        kind = "vulkan_probe" if vulkan_probe is not None else "graphics_probe"
        extras = ["--ez", kind, "true", "--ei", "probe_seconds", str(duration)]
        if validation:
            extras += ["--ez", "vulkan_validation", "true"]
    else:
        if any(argument in ("-netconport", "-netconpassword") for argument in arguments):
            raise RuntimeError("The run action manages netconsole's port/password; use the console action to connect.")
        if session_path.is_file():
            previous = json.loads(session_path.read_text())
            mapping = f"{serial} tcp:{previous['port']} tcp:27991"
            mappings = subprocess.check_output(adb + ["forward", "--list"], text=True).splitlines()
            if previous["serial"] == serial and mapping in mappings:
                subprocess.run(adb + ["forward", "--remove", f"tcp:{previous['port']}"], check=True)
        port = int(subprocess.check_output(adb + ["forward", "tcp:0", "tcp:27991"], text=True))
        password = secrets.token_hex(16)
        session_path.parent.mkdir(parents=True, exist_ok=True)
        with os.fdopen(os.open(session_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as output:
            json.dump({"serial": serial, "port": port, "password": password}, output)
        # JSON preserves spaces, commas and quotes across both host and device
        # shells. am's comma-separated --esa encoding cannot represent all argv.
        engine_args = ["-netconport", "27991", "-netconpassword", password, *arguments]
        extras = ["--es", "engine_args_json", json.dumps(engine_args)]
    command = ["am", "start", "-W", "-S", "-a", "android.intent.action.MAIN",
               "-c", "android.intent.category.LAUNCHER", "-f", "0x10208000",
               "-n", f"{package}/com.csgosource.android.LauncherActivity", "--ez", "auto_start", "true", *extras]
    if subprocess.call(adb + ["shell", shlex.join(command)]):
        raise RuntimeError("Android activity launch failed; see the ADB output above.")
    if duration is None:
        print(f"[csgo-android] Console: BUILD_CONFIG={config} bash scripts/build-android.sh console 'status'", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("fingerprint", "receipt", "stage", "run", "console"))
    parser.add_argument("--config", choices=("debug", "release"), default="release")
    parser.add_argument("--ndk", type=pathlib.Path)
    parser.add_argument("--source-id", help="Fingerprint captured before the engine build")
    parser.add_argument("--scope", choices=("engine", "package"), default="engine")
    parser.add_argument("--adb", type=pathlib.Path)
    probes = parser.add_mutually_exclusive_group()
    probes.add_argument("--probe", type=int)
    probes.add_argument("--vulkan-probe", type=int)
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--args", nargs=argparse.REMAINDER, default=[])
    args = parser.parse_args()
    if args.action == "fingerprint": print(source_id(args.scope, args.config))
    elif args.action == "receipt":
        if not args.source_id: parser.error("receipt requires --source-id from before the build")
        receipt(args.config, args.source_id)
    elif args.action == "stage":
        if args.ndk is None: parser.error("stage requires --ndk")
        stage(args.config, args.ndk)
    else:
        if args.adb is None: parser.error("run/console requires --adb")
        device_action(args.action, args.config, args.adb, args.args, args.probe, args.vulkan_probe, args.validation)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, KeyError, ValueError) as error:
        sys.exit(f"[csgo-android] {error}")
