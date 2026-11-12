"""Read-only reproduction of compile.rs Cfg::hash for Android lifted objects.

Pass the exact compiler/repo spelling used by wtbuild. A mismatch blocks reuse;
this tool neither compiles nor updates stamps. No private source is printed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def digest(data):
    return hashlib.sha256(data).digest()


def includes(data):
    return re.findall(r'^\s*#\s*include\s+"([^"\n]+)"', data.decode("latin1"), re.M)


def config_hash(repo, spelling, clang):
    runtime, gen = repo / "runtime", repo / "build/gen_eh"
    if not gen.is_dir() or not any(gen.glob("*.c")):
        raise ValueError("missing or empty generated source set")
    root = spelling + "/scripts/.."
    inc = root + "/runtime"
    flags = ["--target=aarch64-linux-android29", "-ffp-contract=off", "-w", "-fno-strict-aliasing",
             "-fPIC", "-Dinline=inline __attribute__((always_inline))", "-nostdlibinc", "-isystem",
             inc + "/freestanding", "-I", inc, "-I", root + "/build/gen_eh"]
    compiler = Path(clang).stat()
    head = [1, clang, compiler.st_size, compiler.st_mtime_ns, "-O2", flags, 5000, "android-arm64"]
    h = hashlib.sha256(json.dumps(head).encode())
    todo = {name for p in gen.glob("*.c") for name in includes(p.read_bytes()[:400])}
    seen = {}
    while todo:
        name = min(todo); todo.remove(name)
        if name == "funcs.h" or name in seen:
            continue
        path = next((d / name for d in (runtime, gen) if (d / name).is_file()), None)
        if path is None:
            raise ValueError(f"missing included header {name}")
        data = path.read_bytes(); seen[name] = digest(data); todo.update(includes(data))
    for path in sorted((runtime / "freestanding").glob("*.h")):
        h.update(f"<{path.name}>".encode()); h.update(b"\0"); h.update(digest(path.read_bytes()))
    for name in sorted(seen):
        h.update(name.encode()); h.update(b"\0"); h.update(seen[name])
    return h.hexdigest(), {name: value.hex() for name, value in seen.items()}


def verify(repo, spelling, clang):
    gen = repo / "build/gen_eh"
    if not gen.is_dir() or not any(gen.glob("*.c")):
        return {"objects": 0, "reuse_verified": False, "failures": ["missing_or_empty_generated_sources"]}
    stamps = json.loads((repo / "build/android-arm64/obj/_stamps.json").read_text())
    cfg, headers = config_hash(repo, spelling, clang)
    failures = []
    names = sorted(p.name for p in (repo / "build/gen_eh").glob("*.c"))
    for name in names:
        stamp = stamps.get(name, {})
        src = repo / "build/gen_eh" / name
        obj = repo / "build/android-arm64/obj" / (src.stem + ".obj")
        if hashlib.sha256(src.read_bytes()).hexdigest() != stamp.get("src"):
            failures.append(f"{name}:source")
        if stamp.get("cfg") != cfg:
            failures.append(f"{name}:config")
        if not obj.is_file():
            failures.append(f"{name}:missing_object"); continue
        with obj.open("rb") as f:
            header = f.read(20)
        if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", header, 16) != (1, 183):
            failures.append(f"{name}:not_aarch64_elf_rel")
        if obj.stat().st_mtime_ns != stamp.get("obj"):
            failures.append(f"{name}:object_stamp")
    return {"objects": len(names), "config": cfg, "recorded_configs": sorted({s["cfg"] for s in stamps.values()}),
            "header_sha256": headers, "reuse_verified": not failures, "failures": failures}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--repo-spelling")
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    result = verify(args.repo, args.repo_spelling or str(args.repo), args.clang)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["reuse_verified"] else 1)
