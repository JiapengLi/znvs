#!/usr/bin/env python3
"""Cortex-M3 object/linked size and conservative module-local stack analysis.
Requires Clang/LLD and GNU or LLVM size/nm; no ARM C library is required.
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
PROFILES = [("full", {"PROFILE": 0}), ("boot", {"PROFILE": 1}), ("readonly", {"PROFILE": 2})]
PUBLIC_APIS = {"znvs_" + name for name in ("init", "mount", "format", "write", "delete", "read", "read_hist", "max_size", "available", "rotate", "arg")}


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
        if name in PUBLIC_APIS:
            total, path = longest(name)
            estimates[name] = {"local_stack_bound": total, "path": path}
    return estimates, sorted(external)


def linked_size(base, support, out, name, readonly, size_tool, nm_tool):
    elf = out / (name + ".elf")
    roots = ["znvs_init", "znvs_read"]
    if not readonly:
        roots += ["znvs_write", "znvs_delete"]
    flags = [flag for flag in base if flag != "-fstack-usage"]
    run(flags + ["-flto", "-nostdlib", "-Wl,--gc-sections", "-Wl,-e,znvs_init"] + ["-Wl,-u," + root for root in roots] + ["znvs/znvs.c", str(support), "-o", str(elf)])
    sections = {}
    for line in run([size_tool, "-A", str(elf)]).splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[0].startswith(".") and fields[1].isdigit():
            sections[fields[0]] = int(fields[1])
    helpers = {}
    for line in run([nm_tool, "-S", str(elf)]).splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[-1] in ("memcpy", "memset", "memcmp"):
            helpers[fields[-1]] = int(fields[1], 16)
    rom = sum(value for key, value in sections.items() if key.startswith((".text", ".rodata", ".data", ".ARM.exidx", ".ARM.extab")))
    return {"linked_roots": roots, "linked_sections": sections, "linked_support_bytes": sum(helpers.values()), "linked_rom_with_support_bytes": rom, "linked_module_rom_bytes": rom - sum(helpers.values()), "linked_support_symbols": helpers}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--size", default="size")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--out", default="_build/measure")
    ap.add_argument("--max-boot-rom", type=int, help="Optional boot linked ROM budget in bytes, including the reported C support")
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
    support = out / "size_libc.o"
    run([args.clang] + flags + ["-ffunction-sections", "-c", "tests/size_libc.c", "-o", str(support)])
    for name, defines in PROFILES:
        opts = ["-DZNVS_%s=%d" % (key, value) for key, value in defines.items()]
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
               **dimensions, "defines": defines, "public_api_stack": estimates, "excluded_callees": external}
        row.update(linked_size(base, support, out, name, (defines["PROFILE"] == 2), args.size, args.nm))
        rows.append(row)
        api = "znvs_read" if (defines["PROFILE"] == 2) else "znvs_write"
        print("%s: object_text=%s data=%s bss=%s instance=%d cfg=%d; %s local stack <=%d; linked_module=%d support=%d total=%d" %
              (name, nums[0], nums[1], nums[2], dimensions["context_bytes"],
               dimensions["config_bytes"], api, estimates[api]["local_stack_bound"], row["linked_module_rom_bytes"], row["linked_support_bytes"], row["linked_rom_with_support_bytes"]))
    report = {
        "compiler": run([args.clang, "--version"]).splitlines()[0],
        "flags": flags,
        "notes": ["Unlinked object text, including read-only data/unwind metadata; excludes C library and flash port.",
                  "Stack: conservative sum of compiler static frames along local direct-call/tail-branch paths.",
                  "External C library/helper functions, callback implementations, exceptions and interrupts excluded.",
                  "Not a measured on-device peak or a complete task-stack requirement.",
                  "Linked measurements use -flto -nostdlib --gc-sections and retain the listed API roots.",
                  "The size-only memcpy/memset/memcmp support is compiled without LTO; its exact symbol sizes are reported separately.",
                  "Linked module ROM includes shared ARM unwind tables, excludes the flash port/startup/vector table, and is not a complete firmware image."],
        "profiles": rows}
    (out / "metrics.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Detailed metrics: " + str(out / "metrics.json"))
    if args.max_boot_rom is not None:
        actual = next(row["linked_rom_with_support_bytes"] for row in rows if row["profile"] == "boot")
        if actual > args.max_boot_rom:
            raise RuntimeError("boot ROM budget exceeded: %d > %d" % (actual, args.max_boot_rom))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        sys.exit(1)
