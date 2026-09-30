#!/usr/bin/env python3
"""Compile-only Cortex-M3 size and conservative module-local stack analysis.
Requires Clang, GNU size and nm; no ARM C library installation is required.
SPDX-License-Identifier: Apache-2.0
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROFILES = [(0, 0, 8), (1, 0, 32), (1, 8, 32)]


def run(cmd):
    result = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=60)
    if result.returncode:
        raise RuntimeError(shlex.join([str(x) for x in cmd]) + "\n" + result.stderr)
    return result.stdout


def stack_analysis(su, assembly):
    frames = {}
    for line in su.read_text().splitlines():
        fields = line.split("\t")
        if len(fields) < 3 or fields[2] != "static":
            raise RuntimeError("Unrecognized or dynamic stack entry: " + line)
        frames[fields[0].rsplit(":", 1)[-1]] = int(fields[1])
    edges = {name: set() for name in frames}
    external = set()
    current = None
    for line in assembly.read_text().splitlines():
        label = re.match(r"^([A-Za-z_][A-Za-z_0-9]*):", line)
        if label:
            current = label.group(1) if label.group(1) in frames else None
        call = re.match(r"\s*(?:bl|blx|b)(?:\.w)?\s+([A-Za-z_][A-Za-z_0-9]*)\b", line)
        if call and current:
            name = call.group(1)
            if name in frames:
                edges[current].add(name)
            elif re.fullmatch(r"r\d+|lr", name):
                external.add("indirect port callback")
            else:
                external.add(name)

    def longest(name, seen=()):
        if name in seen:
            raise RuntimeError("Recursive call graph: " + str(seen + (name,)))
        tails = [longest(c, seen + (name,)) for c in sorted(edges[name])]
        child, path = max(tails, default=(0, []), key=lambda pair: pair[0])
        # Tail branches are deliberately added rather than optimized away.
        return frames[name] + child, [name] + path

    estimates = {}
    for name in sorted(frames):
        if name.startswith("znvs_"):
            total, path = longest(name)
            estimates[name] = {"local_stack_bound": total, "path": path}
    return estimates, sorted(external)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--size", default="size")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--out", default="_build/measure")
    args = ap.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    inc = out / "compile_only_headers"
    inc.mkdir(exist_ok=True)
    # Declarations only. No replacement libc implementation is linked or supplied.
    (inc / "string.h").write_text(
        "#include <stddef.h>\n"
        "void *memcpy(void *, const void *, size_t);\n"
        "void *memset(void *, int, size_t);\n"
        "int memcmp(const void *, const void *, size_t);\n")
    metrics = out / "metrics.c"
    metrics.write_text('#include "znvs.h"\n'
                       'const unsigned char context_bytes[sizeof(znvs_t)] = {0};\n'
                       'const unsigned char config_bytes[sizeof(znvs_cfg_t)] = {0};\n')
    flags = ["--target=arm-none-eabi", "-mcpu=cortex-m3", "-mthumb", "-std=c99",
             "-Oz", "-ffreestanding", "-fno-builtin", "-fstack-usage",
             "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-I" + str(inc), "-Iznvs"]
    rows = []
    for crc, cache, io in PROFILES:
        name = "crc%d-cache%d-io%d" % (crc, cache, io)
        opts = ["-DZNVS_DATA_CRC=%d" % crc, "-DZNVS_CACHE_SIZE=%d" % cache,
                "-DZNVS_IO_SIZE=%d" % io]
        base = [args.clang] + flags + opts
        obj, asm, sizes = out / (name + ".o"), out / (name + ".s"), out / (name + "-sizes.o")
        run(base + ["-c", "znvs/znvs.c", "-o", str(obj)])
        run(base + ["-S", "znvs/znvs.c", "-o", str(asm)])
        run(base + ["-c", str(metrics), "-o", str(sizes)])
        size_text = run([args.size, str(obj)])
        nums = size_text.splitlines()[-1].split()
        dimensions = {}
        for line in run([args.nm, "-S", str(sizes)]).splitlines():
            p = line.split()
            if len(p) == 4 and p[-1] in ("context_bytes", "config_bytes"):
                dimensions[p[-1]] = int(p[1], 16)
        estimates, external = stack_analysis(obj.with_suffix(".su"), asm)
        row = {"profile": name, "object_text_bytes": int(nums[0]),
               "object_data_bytes": int(nums[1]), "object_bss_bytes": int(nums[2]),
               **dimensions, "public_api_stack": estimates, "excluded_callees": external}
        rows.append(row)
        print("%s: text=%s data=%s bss=%s instance=%d cfg=%d; write local stack <=%d" %
              (name, nums[0], nums[1], nums[2], dimensions["context_bytes"],
               dimensions["config_bytes"], estimates["znvs_write"]["local_stack_bound"]))
    report = {
        "compiler": run([args.clang, "--version"]).splitlines()[0],
        "flags": flags,
        "notes": ["Unlinked object text, including read-only data/unwind metadata; excludes C library and flash port.",
                  "Stack: conservative sum of compiler static frames along local direct-call/tail-branch paths.",
                  "External C library/helper functions, callback implementations, exceptions and interrupts excluded.",
                  "Not a measured on-device peak or a complete task-stack requirement."],
        "profiles": rows}
    (out / "metrics.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Detailed metrics: " + str(out / "metrics.json"))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        sys.exit(1)
