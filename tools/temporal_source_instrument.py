"""Explicit private validation lift instrumentation. Default is read-only verification.

Never publish generated sources or backups. --apply requires a fresh private backup
directory; a re-lift followed by this tool reproduces the same bounded hooks.
"""
import argparse
import hashlib
import json
from pathlib import Path

SOURCE_SHA = {
    "c_00aa.c": "03b49953db095ad1f324ab650d07216995e66f06e119d81069bebd1354ee3171",
    "c_00ab.c": "fd1c1c8b2aebdb515bed3d2ce4a93f21cacad77d03d956da512dd1dccb1f6c4a",
}
INSTRUMENTED_SHA = {
    "c_00aa.c": "679a8ef7fc8fb37020adc1c5d8da50c3c6eba4de700da9d0ae7ae2b361d11659",
    "c_00ab.c": "a5465771521b768ae36073acb3d965f2d82523a71a7ca676865229359396771f",
}
# Capture values before the named original instruction; no guest-memory reads added.
SITES = {
    "c_00aa.c": {
        "71008013a0": "b3f9498ea9b19b2fc0bcb5846e2b2c189019a1ddd7f54f30d0a0dd0d5a32f149",
        "7100801494": "fa9eeb99287517202d6d6e4a416b17be6c4937f79cd621bfba676d44106356f1",
        "71008014b0": "beb63837558892e310b926fb739562751265bc69f09357d76e01a80445cfdfdc",
        "7100801518": "ee0d699df70bb0c56aebb5b907746c7cd27693ce7af65a747cb96bc06be212dc",
        "7100803140": "e1063cfd2b08b19a82f4cc9134677d81b5604a35537245dfe524bd27de55068b",
        "7100803178": "bfbd2b51ab074611dc4c94d2adbd7414a183c4de0f5c4592331bb8ab9a9157e5",
        "7100803184": "4a5499069fa8ae32e9c81bdd5ea7b158c2b910d3ce690682cad61456fa605e06",
        "710080321c": "4ee3def98ce4b9133aaaa719d06958d1db0e70a2dc57448bb2868fce6dd69357",
        "7100803238": "0d6f9b776d4657560becb77795b59faf4d724c2805de41a43db48ac51923e716",
    },
    "c_00ab.c": {
        "710080c698": "60ded6c264b6a1d239963fa0732b4da315a17f4f662dc38a8a2c3ad7d8b52b7c",
        "710080c720": "57e7e3656e52597327561c6e4ed7e45627cfff00e3dae57d9f951adfc79b9657",
        "710080c760": "95fbf140edb911e578c44083830e29b753bd1cef8f1c23c469430bfc6b70e4f6",
        "710080c7a4": "44b4ca06cd28cd0b16db1d7a8da748f16174add9b9e7a7ceb7eb69baa11b8591",
        "710080c858": "c6fafa46b6790e2dd40e14b35cb3b4a3e412970f7af9f7c8dd9fbf8861a50940",
        "710080c89c": "2aa6b2d14f9cd32256057deb2c1c682f79017f8b1734162070b75eb1fadb53e1",
    },
}


def instrument(name, source):
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA[name]:
        raise ValueError(f"{name}: unsupported or already instrumented source SHA")
    text = source.decode("utf-8")
    lines = text.splitlines(keepends=True)
    found = set()
    out = []
    for line in lines:
        site = line.split(":", 1)[0].removeprefix("L_")
        if site in SITES[name]:
            if hashlib.sha256(line.rstrip("\r\n").encode()).hexdigest() != SITES[name][site] or site in found:
                raise ValueError(f"{name}: instruction mismatch at {site}")
            found.add(site)
            line = line.replace(f"L_{site}: ", f"L_{site}: wt_temporal_guest_site(c, 0x{site}ULL); ", 1)
        # Tail-call instructions do not establish their own LR: keep their exact site.
        elif "f_7100803140(c); return;" in line and line.startswith("L_"):
            line = line.replace(f"L_{site}: ", f"L_{site}: wt_temporal_guest_site(c, 0x{site}ULL); ", 1)
        out.append(line)
    if found != set(SITES[name]):
        raise ValueError(f"{name}: missing capture sites {set(SITES[name]) - found}")
    marker = '#include "funcs.h"'
    if text.count(marker) != 1:
        raise ValueError(f"{name}: unsupported include structure")
    return "".join(out).replace(marker, marker +
        "\nextern void wt_temporal_guest_site(const Ctx *, uint64_t);").encode("utf-8")


def run(gen, backup=None):
    # Validate every input before creating backups or modifying any source.
    originals = {name: (gen / name).read_bytes() for name in SOURCE_SHA}
    changed = {name: instrument(name, b) for name, b in originals.items()}
    manifest = {name: {"before": SOURCE_SHA[name], "after": hashlib.sha256(b).hexdigest()}
                for name, b in changed.items()}
    if backup is not None:
        backup.mkdir(parents=True, exist_ok=False)
        for name, b in originals.items():
            (backup / name).write_bytes(b)
        (backup / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        # All backups are complete before the first write; restore them on partial failure.
        try:
            for name, b in changed.items():
                (gen / name).write_bytes(b)
        except BaseException:
            for name, b in originals.items():
                (gen / name).write_bytes(b)
            raise
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gen", type=Path, default=Path(__file__).resolve().parents[1] / "build/gen_eh")
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--backup", type=Path)
    args = parser.parse_args()
    if args.apply != (args.backup is not None):
        parser.error("--apply requires --backup, and --backup requires --apply")
    print(json.dumps(run(args.gen, args.backup), indent=2))
