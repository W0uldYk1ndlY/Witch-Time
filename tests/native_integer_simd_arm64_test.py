#!/usr/bin/env python3
"""Execute NDK-built integer helpers on AArch64 Unicorn against exact Python math.

Test-only dependencies: unicorn and pyelftools. Requires ANDROID_NDK_HOME or
--cc. Compiles without libc and with the lifted code's freestanding headers.
Tests both Android's default backend and WT_NATIVE_INT_SIMD=0. This validates
machine-code results, not device timing, thermal behavior or game performance.
"""
import argparse
import io
import os
from pathlib import Path
import random
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import (UC_ARM64_REG_CPACR_EL1, UC_ARM64_REG_LR,
                                 UC_ARM64_REG_PC, UC_ARM64_REG_SP,
                                 UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2)

DATA, STACK, STOP = 0x10000000, 0x20000000, 0x30000000
OPS = ("add", "sub", "mul", "cmeq", "cmgt", "cmhi", "cmge", "cmhs", "cmtst")


def wrappers():
    source = '#include "runtime.h"\n#include "rt_ext.h"\n'
    integer, high = {}, {}
    for sz in range(4):
        for count in sorted({1, 8 >> sz, 16 >> sz}):
            for op in OPS:
                if sz == 3 and op == "mul":
                    continue
                name = f"i3_{op}_{sz}_{count}"
                integer[name] = (op, 8 << sz, count)
                source += f"void {name}(V128 *d, const V128 *a, const V128 *b) {{ v_i3v(d, a, b, I_{op.upper()}, {sz}, {count}); }}\n"
    for sz in (1, 2):
        for count in (1, 8 >> sz, 16 >> sz):
            for rounding in (0, 1):
                name = f"high_{sz}_{count}_{rounding}"
                high[name] = (8 << sz, count, rounding)
                source += f"void {name}(V128 *d, const V128 *a, uint64_t y) {{ v_ielem(d, a, y, {3 + rounding}, {sz}, {count}); }}\n"
    return source, integer, high


def integer_oracle(a, b, op, bits, count):
    size, mask = bits // 8, (1 << bits) - 1
    out = bytearray(16)
    for lane in range(count):
        start, end = lane * size, (lane + 1) * size
        x, y = int.from_bytes(a[start:end], "little"), int.from_bytes(b[start:end], "little")
        sx = int.from_bytes(a[start:end], "little", signed=True)
        sy = int.from_bytes(b[start:end], "little", signed=True)
        if op == "add": result = x + y
        elif op == "sub": result = x - y
        elif op == "mul": result = x * y
        elif op == "cmeq": result = mask if x == y else 0
        elif op == "cmgt": result = mask if sx > sy else 0
        elif op == "cmge": result = mask if sx >= sy else 0
        elif op == "cmhi": result = mask if x > y else 0
        elif op == "cmhs": result = mask if x >= y else 0
        elif op == "cmtst": result = mask if x & y else 0
        else: raise AssertionError(op)
        out[start:end] = (result & mask).to_bytes(size, "little")
    return bytes(out)


def high_oracle(a, multiplier, bits, count, rounding):
    size, sign = bits // 8, 1 << (bits - 1)
    multiplier &= 2 * sign - 1
    y = multiplier - 2 * sign if multiplier & sign else multiplier
    out = bytearray(16)
    for lane in range(count):
        start, end = lane * size, (lane + 1) * size
        x = int.from_bytes(a[start:end], "little", signed=True)
        # Arbitrary precision plus floor division: no C overflow or right-shift assumptions.
        result = (2 * x * y + (sign if rounding else 0)) // (2 * sign)
        result = max(-sign, min(sign - 1, result))
        out[start:end] = (result & (2 * sign - 1)).to_bytes(size, "little")
    return bytes(out)


class Runner:
    def __init__(self, path):
        elf = ELFFile(io.BytesIO(path.read_bytes()))
        self.machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        pages = set()
        for segment in elf.iter_segments():
            if segment["p_type"] == "PT_LOAD":
                begin, length = segment["p_vaddr"], segment["p_memsz"]
                pages.update(range(begin & ~4095, (begin + length + 4095) & ~4095, 4096))
        for page in sorted(pages):
            self.machine.mem_map(page, 4096)
        for segment in elf.iter_segments():
            if segment["p_type"] == "PT_LOAD":
                self.machine.mem_write(segment["p_vaddr"], segment.data())
        self.symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()
                        if s["st_info"]["type"] == "STT_FUNC"}
        self.machine.mem_map(DATA, 4096)
        self.machine.mem_map(STACK, 65536)
        self.machine.mem_map(STOP, 4096)
        self.machine.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
        self.cases = 0

    def call(self, name, a, b, expected, alias, multiplier=None):
        memory = bytearray(b"\x69" * 96)
        memory[8:24], memory[40:56] = a, b
        out = 8 if alias in (1, 4) else 40 if alias == 2 else 72
        right = 8 if alias in (3, 4) else 40
        want = memory.copy()
        want[out:out + 16] = expected
        uc = self.machine
        uc.mem_write(DATA, bytes(memory))
        uc.reg_write(UC_ARM64_REG_X0, DATA + out)
        uc.reg_write(UC_ARM64_REG_X1, DATA + 8)
        uc.reg_write(UC_ARM64_REG_X2, DATA + right if multiplier is None else multiplier)
        uc.reg_write(UC_ARM64_REG_SP, STACK + 32768)
        uc.reg_write(UC_ARM64_REG_LR, STOP)
        uc.emu_start(self.symbols[name], STOP, count=10000)
        assert uc.reg_read(UC_ARM64_REG_PC) == STOP, (name, "failed to return within instruction limit")
        actual = bytes(uc.mem_read(DATA, len(want)))
        assert actual == bytes(want), (name, alias, a.hex(), b.hex(), multiplier,
                                       actual.hex(), bytes(want).hex())
        self.cases += 1


def samples(bits, rng):
    size, sign = bits // 8, 1 << (bits - 1)
    edges = (0, 1, sign // 2 - 1, sign // 2, sign // 2 + 1,
             sign - 1, sign, sign + 1, 2 * sign - 1)
    for x in edges:
        for y in edges:
            yield x.to_bytes(size, "little") * (16 // size), y.to_bytes(size, "little") * (16 // size)
    for _ in range(64):
        yield rng.randbytes(16), rng.randbytes(16)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    ndk = os.environ.get("ANDROID_NDK_HOME", os.environ.get("ANDROID_NDK_ROOT", ""))
    parser.add_argument("--cc", default=str(Path(ndk) / "toolchains/llvm/prebuilt/linux-x86_64/bin/clang") if ndk else None)
    args = parser.parse_args()
    if not args.cc:
        parser.error("set ANDROID_NDK_HOME or pass --cc with NDK clang")
    root = Path(__file__).resolve().parent.parent
    source, integer, high = wrappers()
    env = os.environ.copy()
    env.pop("CPATH", None)
    with tempfile.TemporaryDirectory(prefix="wt-arm64-integers-") as tmp:
        tmp = Path(tmp)
        input_path = tmp / "helpers.c"
        input_path.write_text(source)
        for backend in ("default", "scalar"):
            binary = tmp / f"{backend}.elf"
            command = [args.cc, "--target=aarch64-linux-android29", "-O2", "-ffp-contract=off",
                       "-fno-strict-aliasing", "-Dinline=inline __attribute__((always_inline))", "-fPIC",
                       "-nostdlibinc", "-isystem", str(root / "runtime/freestanding"), "-I", str(root / "runtime"),
                       "-fno-stack-protector", "-nostdlib", "-static", "-fuse-ld=lld",
                       "-Wl,-Ttext=0x100000", "-Wl,-e,i3_add_0_1", "-Wl,--build-id=none", "-Wl,--no-undefined"]
            if backend == "scalar":
                command += ["-DWT_NATIVE_INT_SIMD=0"]
            subprocess.run(command + [str(input_path), "-o", str(binary)], check=True, env=env)
            runner, rng = Runner(binary), random.Random(0x5749544348)
            for name, (op, bits, count) in integer.items():
                for a, b in samples(bits, rng):
                    for alias in range(5):
                        expected = integer_oracle(a, a if alias >= 3 else b, op, bits, count)
                        runner.call(name, a, b, expected, alias)
            for name, (bits, count, rounding) in high.items():
                for a, b in samples(bits, rng):
                    multiplier = int.from_bytes(b[:bits // 8], "little")
                    expected = high_oracle(a, multiplier, bits, count, rounding)
                    for alias in (0, 1):
                        runner.call(name, a, b, expected, alias, multiplier)
            print(f"native_integer_simd_arm64_test: {backend}: {runner.cases} NDK machine-code cases passed "
                  f"({len(integer)} integer / {len(high)} saturating-multiply forms)")


if __name__ == "__main__":
    main()
