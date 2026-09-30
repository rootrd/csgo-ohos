#!/usr/bin/env python3
"""Compile explicit HLSL variants to Vulkan 1.1 SPIR-V and record their hashes."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    if os.environ.get("CONTAINER_ID") != "dev":
        os.execvp("distrobox", ["distrobox", "enter", "-T", "-n", "dev", "--", "python3",
                               str(pathlib.Path(__file__).resolve()), *sys.argv[1:]])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxc", required=True, type=pathlib.Path)
    parser.add_argument("--source", type=pathlib.Path, default=ROOT / "src/materialsystem/shaderapivulkan/shaders")
    parser.add_argument("--output", type=pathlib.Path, default=ROOT / "runtime/vulkan/shaders")
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("BUILD_JOBS", "4")))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    source = args.source.resolve()
    manifest = json.loads((source / "manifest.json").read_text())
    if manifest.get("schema") != 1:
        raise RuntimeError("Unsupported Vulkan shader manifest schema")
    validator = shutil.which("spirv-val")
    if not validator:
        raise RuntimeError("Install spirv-tools in Distrobox dev")
    compiler = subprocess.check_output([str(args.dxc), "--version"], text=True).strip()
    args.output.mkdir(parents=True, exist_ok=True)
    profiles = {"vertex": "vs_6_0", "fragment": "ps_6_0"}
    seen = set()
    source_keys = set()
    tasks = []
    previous = {}
    try:
        old = json.loads((args.output / "manifest.json").read_text())
        if old.get("compiler") == compiler:
            previous = {entry["name"]: entry for entry in old["variants"]}
    except (OSError, ValueError, KeyError):
        pass
    def dependencies(path, seen=None):
        seen = set() if seen is None else seen
        if path in seen:
            return {}
        if not path.is_relative_to(source):
            raise RuntimeError("Shader include escaped its source directory")
        seen.add(path)
        contents = path.read_bytes()
        result = {str(path.relative_to(source)): hashlib.sha256(contents).hexdigest()}
        for include in re.findall(rb'^\s*#include\s+"([^"]+)"', contents, re.MULTILINE):
            result.update(dependencies((path.parent / include.decode()).resolve(), seen))
        return result
    inputs = {str(path.relative_to(source)): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in sorted(source.rglob("*")) if path.is_file() and path.suffix in (".hlsl", ".hlsli")}
    for shader in manifest["shaders"]:
        name, stage, entry = shader["name"], shader["stage"], shader["entry"]
        if not re.fullmatch(r"[a-zA-Z_][a-zA-Z_0-9]{0,95}", name) or name in seen:
            raise RuntimeError("Invalid or duplicate shader variant name")
        seen.add(name)
        source_name = shader.get("source_name", name)
        if not isinstance(source_name, str) or not re.fullmatch(r"[a-zA-Z_][a-zA-Z_0-9]{0,95}", source_name):
            raise RuntimeError("Invalid Source shader name")
        if not isinstance(entry, str) or not re.fullmatch(r"[a-zA-Z_][a-zA-Z_0-9]{0,95}", entry):
            raise RuntimeError("Invalid shader entry point")
        static_index, dynamic_index = shader.get("static_index", 0), shader.get("dynamic_index", 0)
        if any(type(index) is not int or not 0 <= index <= 0x7fffffff for index in (static_index, dynamic_index)):
            raise RuntimeError("Invalid Source shader combo index")
        alpha_test = shader.get("alpha_test", False)
        if type(alpha_test) is not bool or (alpha_test and stage != "fragment"):
            raise RuntimeError("Alpha test can only be implemented by a fragment shader")
        key = (source_name.lower(), stage, static_index, dynamic_index)
        if key in source_keys:
            raise RuntimeError("Duplicate Source shader name/stage/static/dynamic combination")
        source_keys.add(key)
        path = (source / shader["source"]).resolve()
        if not path.is_relative_to(source) or stage not in profiles:
            raise RuntimeError("Invalid shader source or stage")
        defines = shader.get("defines", {})
        if any(not re.fullmatch(r"[a-zA-Z_][a-zA-Z_0-9]*", key) for key in defines):
            raise RuntimeError("Invalid shader define")
        output = args.output / (name + ".spv")
        temporary = output.with_suffix(".spv.tmp")
        command = [str(args.dxc), "-spirv", "-fspv-target-env=vulkan1.1", "-fvk-use-dx-layout", "-Zpr", "-O3",
                   "-T", profiles[stage], "-E", entry, "-I", str(source), "-Fo", str(temporary), str(path)]
        if stage == "vertex":
            command.append("-fvk-invert-y")
        for key, value in sorted(defines.items()):
            command.extend(["-D", f"{key}={value}"])
        signature = hashlib.sha256(json.dumps({"shader": shader, "dependencies": dependencies(path),
            "flags": ["vulkan1.1", "dx-layout", "row-major", "O3", "invert-vertex-y"], "compiler": compiler},
            sort_keys=True).encode()).hexdigest()
        receipt = {**shader, "source_name": source_name.lower(), "static_index": static_index,
                      "dynamic_index": dynamic_index, "alpha_test": alpha_test,
                      "profile": profiles[stage], "file": output.name,
                      "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                      "input_signature": signature}
        tasks.append((command, temporary, output, receipt))

    def build(task):
        command, temporary, output, receipt = task
        old = previous.get(receipt["name"], {})
        if old.get("input_signature") == receipt["input_signature"] and output.is_file():
            digest = hashlib.sha256(output.read_bytes()).hexdigest()
            if digest == old.get("spirv_sha256"):
                return {**receipt, "spirv_sha256": digest}, False
        try:
            subprocess.run(command, check=True)
            subprocess.run([validator, "--target-env", "vulkan1.1", str(temporary)], check=True)
            temporary.replace(output)
        finally:
            temporary.unlink(missing_ok=True)
        return {**receipt, "spirv_sha256": hashlib.sha256(output.read_bytes()).hexdigest()}, True

    executor = ThreadPoolExecutor(max_workers=args.jobs)
    try:
        results = list(executor.map(build, tasks))
    finally:
        executor.shutdown(wait=True, cancel_futures=True)
    built = [entry for entry, _ in results]
    changed = sum(updated for _, updated in results)
    print(f"[vulkan-shaders] {len(built)} Vulkan 1.1 variants: {changed} compiled and validated, {len(built)-changed} unchanged", flush=True)
    receipt = {"schema": 1, "compiler": compiler, "environment": "vulkan1.1", "row_major": True,
               "invert_vertex_y": True, "sources": inputs, "variants": built}
    path = args.output / "manifest.json"
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(receipt, indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)
    # A small, versioned runtime index avoids pulling the engine's JSON/KeyValues
    # libraries into the standalone backend. The full receipt retains hashes.
    index = ["SOURCEVK_VARIANTS\t1"]
    for shader in built:
        index.append("\t".join(str(shader[field]) for field in
                               ("source_name", "stage", "static_index", "dynamic_index", "file", "entry")) +
                     "\t" + str(int(shader["alpha_test"])))
    path = args.output / "source-variants.tsv"
    temporary = path.with_suffix(".tsv.tmp")
    temporary.write_text("\n".join(index) + "\n")
    temporary.replace(path)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, KeyError, ValueError) as error:
        sys.exit("[vulkan-shaders] " + str(error))
