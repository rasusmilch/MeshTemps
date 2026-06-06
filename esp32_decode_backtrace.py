#!/usr/bin/env python3
"""
esp32_decode_backtrace.py

Small ESP/Arduino backtrace decoder.

Strict defaults:
  - Finds only ELF files that match the requested project name/path or nearby build metadata.
  - Uses ESP cross-toolchain addr2line by default.
  - Rejects generic host addr2line unless --allow-generic-addr2line is used.
  - --chip lets you explicitly select the target family/toolchain.

Examples:

  python esp32_decode_backtrace.py MeshTemps-GUINode --chip S3 --backtrace "Backtrace: 0x420d0ed7:0x3fcebbd0"

  python esp32_decode_backtrace.py MeshTemps-GUINode --chip S3 --backtrace-file crash.txt --list

  python esp32_decode_backtrace.py MeshTemps-GUINode \
    --elf ~/.cache/arduino/sketches/.../MeshTemps-GUINode.ino.elf \
    --addr2line ~/.arduino15/packages/esp32/tools/esp-x32/2511/bin/xtensa-esp32s3-elf-addr2line \
    --backtrace-file crash.txt

Notes:
  - ESP backtrace entries are PC:SP pairs. addr2line needs only the PC address.
  - Example: 0x420d0ed7:0x3fcebbd0 -> 0x420d0ed7
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


BACKTRACE_PC_RE = re.compile(r"(?P<pc>0x[0-9a-fA-F]+)(?::0x[0-9a-fA-F]+)?")
COMPILER_TO_ADDR2LINE_RE = re.compile(r"(?P<prefix>.+-)(?:g\+\+|gcc|cc|c\+\+)(?:\.exe)?$", re.I)

CHIP_ALIASES = {
    "esp32": "esp32",
    "classic": "esp32",
    "wroom": "esp32",
    "wrover": "esp32",
    "s2": "s2",
    "esp32s2": "s2",
    "esp32-s2": "s2",
    "s3": "s3",
    "esp32s3": "s3",
    "esp32-s3": "s3",
    "c2": "riscv",
    "esp32c2": "riscv",
    "esp32-c2": "riscv",
    "c3": "riscv",
    "esp32c3": "riscv",
    "esp32-c3": "riscv",
    "c5": "riscv",
    "esp32c5": "riscv",
    "esp32-c5": "riscv",
    "c6": "riscv",
    "esp32c6": "riscv",
    "esp32-c6": "riscv",
    "h2": "riscv",
    "esp32h2": "riscv",
    "esp32-h2": "riscv",
    "h4": "riscv",      # accepted as a user hint; current Arduino ESP tools generally use riscv32-esp-elf for RISC-V parts
    "esp32h4": "riscv",
    "esp32-h4": "riscv",
    "p4": "riscv",
    "esp32p4": "riscv",
    "esp32-p4": "riscv",
}

TOOL_NAMES_BY_CHIP = {
    "esp32": ["xtensa-esp32-elf-addr2line"],
    "s2": ["xtensa-esp32s2-elf-addr2line"],
    "s3": ["xtensa-esp32s3-elf-addr2line"],
    "riscv": ["riscv32-esp-elf-addr2line"],
}

TARGET_HINTS = [
    "esp32s3",
    "esp32-s3",
    "esp32s2",
    "esp32-s2",
    "esp32c6",
    "esp32-c6",
    "esp32c5",
    "esp32-c5",
    "esp32c3",
    "esp32-c3",
    "esp32c2",
    "esp32-c2",
    "esp32h2",
    "esp32-h2",
    "esp32p4",
    "esp32-p4",
    "esp32",
]


@dataclass(frozen=True)
class Candidate:
    path: Path
    mtime: float
    score: int
    reason: str


def is_windows() -> bool:
    return platform.system().lower().startswith("win")


def with_exe(name: str) -> list[str]:
    if is_windows() and not name.lower().endswith(".exe"):
        return [name + ".exe", name]
    return [name]


def normalize_chip(chip: str | None) -> str | None:
    if not chip:
        return None
    key = chip.strip().lower().replace("_", "-")
    key_no_dash = key.replace("-", "")
    if key in CHIP_ALIASES:
        return CHIP_ALIASES[key]
    if key_no_dash in CHIP_ALIASES:
        return CHIP_ALIASES[key_no_dash]
    raise SystemExit(
        f"ERROR: unsupported --chip '{chip}'. "
        "Use ESP32, S2, S3, C2, C3, C5, C6, H2, H4, or P4, "
        "or pass --addr2line explicitly."
    )


def safe_rglob(root: Path, pattern: str) -> Iterable[Path]:
    try:
        yield from root.rglob(pattern)
    except (OSError, PermissionError):
        return


def read_text(path: Path) -> str:
    try:
        return path.read_text(errors="replace")
    except (OSError, PermissionError, UnicodeDecodeError):
        return ""


def make_candidate(path: Path, score: int, reason: str) -> Candidate | None:
    try:
        if not path.exists() or not path.is_file():
            return None
        st = path.stat()
        return Candidate(path.resolve(), st.st_mtime, score, reason)
    except (OSError, PermissionError):
        return None


def dedupe(candidates: Sequence[Candidate]) -> list[Candidate]:
    best: dict[Path, Candidate] = {}
    for cand in candidates:
        old = best.get(cand.path)
        if old is None or cand.score > old.score:
            best[cand.path] = cand
    return sorted(best.values(), key=lambda c: (c.score, c.mtime), reverse=True)


def project_tokens(project: str) -> tuple[list[str], Path | None]:
    raw = project.strip()
    p = Path(raw).expanduser()
    resolved = None
    if p.exists():
        try:
            resolved = p.resolve()
        except OSError:
            resolved = p

    values = {raw, Path(raw).name, Path(raw).stem}
    if resolved:
        values.update({resolved.name, resolved.stem})

    tokens = set()
    for value in values:
        if value:
            tokens.add(value.lower())
            tokens.add(value.lower().replace("_", "-"))
            tokens.add(value.lower().replace("-", "_"))

    return sorted(t for t in tokens if t), resolved


def add_root(roots: list[Path], path: Path) -> None:
    try:
        path = path.expanduser()
    except RuntimeError:
        return
    if path.exists() and path not in roots:
        roots.append(path)


def search_roots(project: str) -> list[Path]:
    roots: list[Path] = []
    cwd = Path.cwd()
    home = Path.home()

    add_root(roots, cwd)
    add_root(roots, cwd / "build")
    add_root(roots, cwd / project)
    add_root(roots, cwd / project / "build")

    p = Path(project).expanduser()
    if p.exists():
        add_root(roots, p)
        if p.is_file():
            add_root(roots, p.parent)

    add_root(roots, home / ".cache" / "arduino")
    add_root(roots, Path(tempfile.gettempdir()))
    add_root(roots, home / ".arduino15")
    add_root(roots, home / ".platformio")
    add_root(roots, home / ".espressif")
    add_root(roots, home / "AppData" / "Local" / "Arduino15")
    add_root(roots, home / "AppData" / "Local" / "Temp")
    add_root(roots, home / "Library" / "Arduino15")
    add_root(roots, home / "Library" / "Caches" / "arduino")
    return roots


def metadata_files_near(path: Path) -> list[Path]:
    names = ["build.options.json", "compile_commands.json", "platformio.ini", "build.ninja", "Makefile"]
    files: list[Path] = []

    for parent in [path.parent, *list(path.parents)[:5]]:
        for name in names:
            p = parent / name
            if p.exists() and p not in files:
                files.append(p)

    try:
        for p in path.parent.iterdir():
            if p.is_file() and p.suffix.lower() in {".json", ".txt", ".mk", ".d", ".ninja"} and p not in files:
                files.append(p)
    except (OSError, PermissionError):
        pass

    return files


def metadata_text_near(path: Path) -> str:
    chunks = []
    for file in metadata_files_near(path):
        text = read_text(file)
        if text:
            chunks.append(f"\n--- {file} ---\n{text}")
            if file.suffix.lower() == ".json":
                try:
                    chunks.append(json.dumps(json.loads(text)))
                except Exception:
                    pass
    return "\n".join(chunks)


def is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except (OSError, ValueError):
        return False


def elf_candidate(path: Path, project: str, loose: bool) -> Candidate | None:
    tokens, project_path = project_tokens(project)
    name_low = path.name.lower()
    path_low = str(path).lower()
    meta = metadata_text_near(path).lower()

    inside_project = bool(project_path and is_relative_to(path, project_path))
    name_match = any(t in name_low for t in tokens)
    path_match = any(t in path_low for t in tokens)
    metadata_match = any(t in meta for t in tokens)

    if not loose and not (inside_project or name_match or path_match or metadata_match):
        return None

    score = 0
    reasons = []
    if inside_project:
        score += 250
        reasons.append("inside project path")
    if name_match:
        score += 220
        reasons.append("ELF name matches project")
    if path_match:
        score += 140
        reasons.append("ELF path matches project")
    if metadata_match:
        score += 180
        reasons.append("build metadata matches project")
    if name_low.endswith(".ino.elf"):
        score += 20
        reasons.append("Arduino sketch ELF")
    if ".cache/arduino" in path_low or ".cache\\arduino" in path_low:
        score += 40
        reasons.append("Arduino cache")
    if ".pio" in path_low:
        score += 20
        reasons.append("PlatformIO build")
    if "bootloader" in name_low or "partitions" in name_low:
        score -= 500
        reasons.append("excluded bootloader/partition ELF")

    if score <= 0:
        return None
    return make_candidate(path, score, ", ".join(reasons))


def find_elfs(project: str, roots: Sequence[Path], loose: bool) -> list[Candidate]:
    candidates = []
    for root in roots:
        for path in safe_rglob(root, "*.elf"):
            c = elf_candidate(path, project, loose)
            if c:
                candidates.append(c)
    return dedupe(candidates)


def infer_chip_from_metadata(path: Path) -> str | None:
    text = (str(path) + "\n" + metadata_text_near(path)).lower()
    for hint in TARGET_HINTS:
        if hint in text:
            try:
                return normalize_chip(hint)
            except SystemExit:
                pass
    return None


def compiler_paths_from_text(text: str) -> list[str]:
    patterns = [
        r'(?:"|\'|=|\s)(?P<path>/[^"\'\s]+(?:g\+\+|gcc|cc|c\+\+)(?:\.exe)?)',
        r'(?:"|\'|=|\s)(?P<path>[A-Za-z]:\\[^"\'\s]+(?:g\+\+|gcc|cc|c\+\+)(?:\.exe)?)',
    ]
    out = []
    for pattern in patterns:
        for match in re.finditer(pattern, text, re.I):
            value = match.group("path")
            if value not in out:
                out.append(value)
    return out


def addr2line_from_compiler(path_text: str) -> Path | None:
    p = Path(path_text.strip().strip("'").strip('"')).expanduser()
    match = COMPILER_TO_ADDR2LINE_RE.match(p.name)
    if not match:
        return None
    name = match.group("prefix") + "addr2line"
    if is_windows():
        name += ".exe"
    return p.with_name(name)


def addr2line_from_build_metadata(elf: Path) -> list[Candidate]:
    candidates = []
    for compiler in compiler_paths_from_text(metadata_text_near(elf)):
        addr = addr2line_from_compiler(compiler)
        if addr:
            c = make_candidate(addr, 2000, "derived from build metadata compiler path")
            if c:
                candidates.append(c)
    return dedupe(candidates)


def tool_roots() -> list[Path]:
    home = Path.home()
    roots = [
        home / ".arduino15" / "packages" / "esp32" / "tools",
        home / ".arduino15" / "packages",
        home / ".platformio" / "packages",
        home / ".espressif" / "tools",
        home / "AppData" / "Local" / "Arduino15" / "packages" / "esp32" / "tools",
        home / "Library" / "Arduino15" / "packages" / "esp32" / "tools",
        Path("/opt"),
        Path("/usr/local"),
        Path("/usr"),
    ]
    return [p for p in roots if p.exists()]


def is_generic_host_addr2line(path: Path) -> bool:
    name = path.name.lower()
    p = str(path).lower()
    if name in {"addr2line", "addr2line.exe"}:
        return True
    if "x86_64-linux-gnu-addr2line" in name:
        return True
    if "/usr/bin" in p and "xtensa" not in name and "riscv32" not in name and "-esp" not in name:
        return True
    return False


def addr2line_candidate(path: Path, chip: str | None, allow_generic: bool) -> Candidate | None:
    name = path.name.lower()
    p = str(path).lower()

    if "addr2line" not in name:
        return None

    generic = is_generic_host_addr2line(path)
    if generic and not allow_generic:
        return None

    score = 0
    reasons = []

    if "xtensa" in name or "riscv32" in name or "-esp" in name:
        score += 400
        reasons.append("ESP cross-toolchain")
    if ".arduino15" in p and "packages" in p and "esp32" in p and "tools" in p:
        score += 350
        reasons.append("Arduino esp32 package tool")
    if "esp-x32" in p or "esp-rv32" in p:
        score += 100
        reasons.append("Arduino ESP packaged toolchain")
    if ".platformio" in p:
        score += 150
        reasons.append("PlatformIO package tool")
    if ".espressif" in p:
        score += 120
        reasons.append("Espressif tool")

    if chip:
        expected_names = TOOL_NAMES_BY_CHIP[chip]
        if name in [n.lower() for n in expected_names]:
            score += 1000
            reasons.append(f"matches --chip {chip}")
        else:
            score -= 500
            reasons.append(f"does not match --chip {chip}")

    if generic:
        score -= 1000
        reasons.append("generic host addr2line fallback")

    if score <= 0:
        return None
    return make_candidate(path, score, ", ".join(reasons))


def find_addr2line(elf: Path, explicit: str | None, chip: str | None, allow_generic: bool, list_mode: bool) -> Path:
    if explicit:
        p = Path(explicit).expanduser()
        if p.exists():
            return p.resolve()
        found = shutil.which(explicit)
        if found:
            return Path(found).resolve()
        raise SystemExit(f"ERROR: addr2line not found: {explicit}")

    effective_chip = chip or infer_chip_from_metadata(elf)
    candidates = []

    # Best source: exact compiler path from build metadata.
    candidates.extend(addr2line_from_build_metadata(elf))

    # Search likely installed tools.
    for root in tool_roots():
        for path in safe_rglob(root, "*addr2line*"):
            c = addr2line_candidate(path, effective_chip, allow_generic)
            if c:
                candidates.append(c)

    # Search PATH.
    names = []
    if effective_chip:
        names.extend(TOOL_NAMES_BY_CHIP[effective_chip])
    else:
        for vals in TOOL_NAMES_BY_CHIP.values():
            names.extend(vals)
    names.append("addr2line")

    for name in names:
        for exe in with_exe(name):
            found = shutil.which(exe)
            if found:
                c = addr2line_candidate(Path(found).resolve(), effective_chip, allow_generic)
                if c:
                    candidates.append(Candidate(c.path, c.mtime, c.score + 20, "PATH: " + c.reason))

    candidates = dedupe(candidates)

    if list_mode:
        print("addr2line candidates:")
        if not candidates:
            print("  none")
        for i, c in enumerate(candidates, 1):
            print(f"{i:2d}. score={c.score:4d}  {c.path}")
            print(f"    {c.reason}")
        print(f"Chip/toolchain hint: {effective_chip or 'none'}")
        print()

    if not candidates:
        chip_help = ""
        if chip:
            chip_help = f" for --chip {chip}"
        raise SystemExit(
            f"ERROR: Could not find an ESP cross-toolchain addr2line{chip_help}.\n"
            "Pass --addr2line explicitly, for example:\n"
            "  --addr2line ~/.arduino15/packages/esp32/tools/esp-x32/2511/bin/xtensa-esp32s3-elf-addr2line\n"
        )

    selected = candidates[0].path
    if is_generic_host_addr2line(selected):
        print("WARNING: selected generic host addr2line; this is probably wrong for ESP firmware.", file=sys.stderr)
    return selected


def extract_pc_addresses(text: str) -> list[str]:
    if "Backtrace:" in text:
        text = text.split("Backtrace:", 1)[1]

    addrs = []
    for match in BACKTRACE_PC_RE.finditer(text):
        pc = match.group("pc").lower()
        if pc not in addrs:
            addrs.append(pc)

    if not addrs:
        raise SystemExit("ERROR: No PC addresses found in backtrace text.")
    return addrs


def read_backtrace(args: argparse.Namespace) -> str:
    parts = []
    if args.backtrace:
        parts.append(args.backtrace)
    if args.backtrace_file:
        parts.append(Path(args.backtrace_file).expanduser().read_text(errors="replace"))
    if args.stdin or (not args.backtrace and not args.backtrace_file):
        if not sys.stdin.isatty():
            parts.append(sys.stdin.read())
    text = "\n".join(parts).strip()
    if not text:
        raise SystemExit("ERROR: No backtrace text supplied.")
    return text


def choose_elf(args: argparse.Namespace, roots: Sequence[Path]) -> Path:
    if args.elf:
        p = Path(args.elf).expanduser()
        if not p.exists():
            raise SystemExit(f"ERROR: ELF file does not exist: {p}")
        return p.resolve()

    candidates = find_elfs(args.project, roots, args.loose_elf_search)
    if args.list:
        print("ELF candidates:")
        if not candidates:
            print("  none")
        for i, c in enumerate(candidates, 1):
            print(f"{i:2d}. score={c.score:4d}  {c.path}")
            print(f"    {c.reason}")
        print()

    if not candidates:
        searched = "\n".join(f"  {p}" for p in roots)
        raise SystemExit(
            f"ERROR: No matching .elf found for project '{args.project}'.\n"
            f"Searched:\n{searched}\n\n"
            "Pass --elf explicitly if auto-discovery fails."
        )

    return candidates[0].path


def decode(addr2line: Path, elf: Path, addresses: Sequence[str]) -> str:
    cmd = [str(addr2line), "-pfiaC", "-e", str(elf), *addresses]
    result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        raise SystemExit(
            "ERROR: addr2line failed.\n"
            f"Command: {' '.join(cmd)}\n"
            f"stderr:\n{result.stderr}"
        )
    return result.stdout.rstrip()


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Decode ESP Arduino/PlatformIO backtraces using addr2line.")
    parser.add_argument("project", help="Project/sketch name or path, e.g. MeshTemps-GUINode.")
    parser.add_argument("--chip", help="Target chip hint: ESP32, S2, S3, C2, C3, C5, C6, H2, H4, or P4.")
    parser.add_argument("--backtrace", help="Backtrace line/text. PC:SP pairs are accepted.")
    parser.add_argument("--backtrace-file", help="File containing a Backtrace line or full crash log.")
    parser.add_argument("--stdin", action="store_true", help="Read backtrace text from stdin.")
    parser.add_argument("--elf", help="Explicit .elf path. Skips ELF auto-discovery.")
    parser.add_argument("--addr2line", help="Explicit addr2line executable path or command name.")
    parser.add_argument("--root", action="append", default=[], help="Additional search root. Can be used more than once.")
    parser.add_argument("--list", action="store_true", help="Print candidate files/tools used during auto-discovery.")
    parser.add_argument("--addresses-only", action="store_true", help="Only print extracted PC addresses, one per line.")
    parser.add_argument("--loose-elf-search", action="store_true", help="Allow unrelated-looking .elf files. Not recommended.")
    parser.add_argument("--allow-generic-addr2line", action="store_true", help="Allow generic host addr2line fallback. Usually wrong for ESP.")
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    chip = normalize_chip(args.chip)

    roots = search_roots(args.project)
    for root in args.root:
        p = Path(root).expanduser()
        if p.exists():
            roots.insert(0, p)

    elf = choose_elf(args, roots)
    text = read_backtrace(args)
    addresses = extract_pc_addresses(text)

    if args.addresses_only:
        for addr in addresses:
            print(addr)
        return 0

    addr2line = find_addr2line(elf, args.addr2line, chip, args.allow_generic_addr2line, args.list)

    if args.list:
        print(f"Selected ELF:       {elf}")
        print(f"Selected addr2line: {addr2line}")
        print("PC addresses:")
        for addr in addresses:
            print(f"  {addr}")
        print()

    print(f"ELF:       {elf}")
    print(f"addr2line: {addr2line}")
    print()
    print(decode(addr2line, elf, addresses))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
