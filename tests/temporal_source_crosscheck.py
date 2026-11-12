"""Android object checks with existing NDK/assets; never changes generated input or build objects."""
import argparse
import importlib.util
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--clang", required=True)
args = parser.parse_args()
staged = ROOT / "build/obj_runtime_android-arm64/android_sources"
if not (staged / "nvn_vk.c").is_file():
    parser.error("requires existing expanded Android assets; does not build or fetch them")
flags = ["--target=aarch64-linux-android29", "-c", "-O1", "-fPIC", "-D__declspec(x)=",
         "-D__stdcall=", "-D__cdecl=", "-D_GNU_SOURCE", "-DWT_VULKAN_ONLY=1", "-Wno-everything",
         "-Werror=implicit-function-declaration", "-Werror=incompatible-function-pointer-types", "-Werror=int-conversion"]
for directory in [ROOT / "runtime/posix", ROOT / "runtime/split", staged, ROOT / "runtime",
                  ROOT / "runtime/gen_static",
                  ROOT / "deps/android-arm64/include", ROOT / "build/obj_runtime_android-arm64/inc"]:
    flags += ["-I", str(directory)]
temp = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode" if os.name == "nt" else None
with tempfile.TemporaryDirectory(prefix="temporal_source_cross_", dir=temp) as directory:
    tmp = Path(directory)
    shutil.copy2(staged / "nvn_vk.c", tmp / "nvn_vk.c")
    shutil.copy2(ROOT / "runtime/nvn_vk_draw.c", tmp / "nvn_vk_draw.c")
    for file in [ROOT / "runtime/temporal_source_capture.c", ROOT / "runtime/game_shim_eh.c",
                 ROOT / "runtime/nvn_gl.c", ROOT / "runtime/boot.c", ROOT / "runtime/hostrt.c",
                 ROOT / "android/app/src/main/cpp/main.c", tmp / "nvn_vk.c"]:
        subprocess.run([args.clang, *flags, str(file), "-o", str(tmp / (file.stem + ".o"))], check=True)
    spec = importlib.util.spec_from_file_location("instrument", ROOT / "tools/temporal_source_instrument.py")
    instrument = importlib.util.module_from_spec(spec); spec.loader.exec_module(instrument)
    for name in instrument.SOURCE_SHA:
        # Private instrumented text lives only in approved temporary storage, never in Git.
        data = (ROOT / "build/gen_eh" / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != instrument.INSTRUMENTED_SHA[name]:
            data = instrument.instrument(name, data)
        (tmp / name).write_bytes(data)
        subprocess.run([args.clang, "--target=aarch64-linux-android29", "-c", "-O0", "-fPIC", "-nostdlibinc",
                        "-isystem", str(ROOT / "runtime/freestanding"), "-I", str(ROOT / "runtime"),
                        "-I", str(ROOT / "build/gen_eh"), "-w", str(tmp / name), "-o", str(tmp / (name + ".o"))], check=True)
    for obj in tmp.glob("*.o"):
        header = obj.read_bytes()[:20]
        assert header[:6] == b"\x7fELF\x02\x01" and header[18:20] == b"\xb7\0"
print("Android object crosschecks passed: module, shim, frontend, native entry, boot/exit, Vulkan and two private instrumented buckets")
print("Uses existing expanded Android assets; not a full relink or device test")
