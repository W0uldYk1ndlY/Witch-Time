#!/usr/bin/env python3
"""Reject wrong-ABI, misaligned, or incompletely packaged Android libraries."""
import pathlib
import re
import subprocess
import sys

SYSTEM = {'libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libandroid.so',
          'libvulkan.so', 'libEGL.so', 'libGLESv1_CM.so', 'libGLESv2.so',
          'libOpenSLES.so', 'libaaudio.so', 'libz.so', 'libmediandk.so'}

def verify(directory, readelf):
    libraries = {p.name: p for p in pathlib.Path(directory).glob('lib*.so')}
    if not libraries:
        raise ValueError('No shared libraries to verify')
    for name, path in sorted(libraries.items()):
        text = subprocess.check_output([readelf, '-h', '-l', '-d', str(path)], text=True)
        if not re.search(r'Machine:\s+AArch64', text):
            raise ValueError(f'{name}: not AArch64')
        loads = re.findall(r'^\s*LOAD\s+.*$', text, re.MULTILINE)
        if not loads or any(int(line.split()[-1], 16) < 16384 for line in loads):
            raise ValueError(f'{name}: LOAD alignment is below 16 KiB')
        sonames = re.findall(r'\(SONAME\).*?\[(.*?)\]', text)
        if sonames and sonames != [name]:
            raise ValueError(f'{name}: incompatible SONAME {sonames}')
        missing = set(re.findall(r'\(NEEDED\).*?\[(.*?)\]', text)) - libraries.keys() - SYSTEM
        if missing:
            raise ValueError(f'{name}: unpackaged dependencies: {sorted(missing)}')
    return len(libraries)

if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: android-verify-libs.py DIRECTORY NDK_LLVM_READELF')
    try:
        print(f'Validated {verify(sys.argv[1], sys.argv[2])} Android ARM64 libraries')
    except (ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
