#!/usr/bin/env python3
"""Offline tests; Python 3.8+, a C compiler, and optionally Clang sanitizers.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def check_profiles(cc, out, sanitize=False):
    flags = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Iznvs"]
    if sanitize:
        flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    objects = []
    for prefix, extra in [("boot", ["-DZNVS_PROFILE=1"]), ("ro", ["-DZNVS_PROFILE=2"])]:
        obj = out / (prefix + ".o")
        names = ["init", "mount", "read", "write", "delete", "max_size", "read_hist", "available", "arg"]
        rename = ["-Dznvs_%s=%s_%s" % (name, prefix, name) for name in names]
        run(cc + flags + extra + rename + ["-c", "znvs/znvs.c", "-o", str(obj)], out / (prefix + "-build.log"))
        objects.append(str(obj))
    exe = out / ("profiles.exe" if os.name == "nt" else "profiles")
    run(cc + flags + ["znvs/znvs.c", "tests/test_profiles.c"] + objects + ["-o", str(exe)], out / "profiles-build.log")
    return run([str(exe)], out / "profiles.log")


def run(cmd, log, timeout=240):
    result = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    log.write_text("$ " + shlex.join([str(x) for x in cmd]) + "\n" + result.stdout,
                   encoding="utf-8")
    if result.returncode:
        raise RuntimeError("command failed; see " + str(log) + "\n" + result.stdout)
    return result.stdout.strip()


def verify_upstream():
    for name, expected in [
        ("nvs.c", "fd6dd480d5712c08f5275830fb21e5b05f25df38"),
        ("nvs_priv.h", "5d9e9dcdaa6d8c931ca36c6c7649f99fa9640dc5")
    ]:
        data = (ROOT / "tests/upstream" / name).read_bytes()
        digest = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        if digest != expected:
            raise RuntimeError("upstream reference changed: " + name)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--out", default="_build/matrix")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--reference-only", action="store_true")
    mode.add_argument("--sanitize-only", action="store_true")
    args = ap.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cc = shlex.split(args.cc)
    compiler = run(cc + ["--version"], out / "compiler.log").splitlines()[0]
    results = []
    if not args.reference_only:
        name = "full"
        exe = out / (name + (".exe" if os.name == "nt" else ""))
        flags = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Wpedantic"]
        if args.sanitize_only:
            flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        cmd = cc + flags + ["-Iznvs", "znvs/znvs.c", "tests/test_znvs.c", "-o", str(exe)]
        run(cmd, out / (name + "-build.log"))
        text = run([str(exe)], out / (name + ".log"))
        print(text, flush=True)
        results.append({"name": name, "kind": "sanitizer" if args.sanitize_only else "nor",
                        "output": text})
    if not args.sanitize_only:
        verify_upstream()
        name = "reference"
        exe = out / (name + (".exe" if os.name == "nt" else ""))
        cmd = cc + ["-std=c11", "-O2", "-Iznvs", "-Itests/upstream/include"]
        cmd += ["-DCONFIG_NVS_DATA_CRC=1"]
        cmd += ["znvs/znvs.c", "tests/upstream/nvs.c", "tests/test_upstream.c", "-o", str(exe)]
        run(cmd, out / (name + "-build.log"))
        text = run([str(exe)], out / (name + ".log"))
        print(text, flush=True)
        results.append({"name": name, "kind": "reference", "output": text})
    if not args.reference_only:
        text = check_profiles(cc, out, args.sanitize_only)
        print(text, flush=True)
        results.append({"name": "profiles", "kind": "interoperability", "output": text})
    if not args.reference_only and not args.sanitize_only:
        exe = out / ("benchmark.exe" if os.name == "nt" else "benchmark")
        run(cc + ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-Iznvs", "znvs/znvs.c", "tests/bench_znvs.c", "-o", str(exe)], out / "benchmark-build.log")
        text = run([str(exe)], out / "benchmark.log")
        results.append({"name": "benchmark", "kind": "callback-counts", "output": text})
        print("Callback counts: " + str(out / "benchmark.log"))
    report = {"compiler": compiler, "results": results}
    (out / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("All requested tests passed. Logs: " + str(out))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        sys.exit(1)
