"""Run the actual Vulkan block allocator against a deterministic memory driver.

The C harness includes the allocator section extracted unchanged from the current
production source. Only Vulkan calls and host metadata allocations are replaced.
Each scenario runs in a fresh process so production environment caches stay real.
"""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")
START = SOURCE.index("typedef struct { int block; uint64_t off, size;")
END = SOURCE.index("static VkBuffer vbuf_new(", START)
SCENARIOS = (
    "fragmented_frees",
    "metadata_oom",
    "empty_blocks",
    "pool_isolation",
    "uma_trim",
    "uma_shrink",
    "budget_advisory",
    "oom_retry",
    "oom_trim",
    "discrete_fallback",
    "discrete_budget_advisory",
    "discrete_cached_budget_advisory",
    "discrete_cached_failure_bounded",
    "discrete_fallback_reuse",
    "alignment",
)

with tempfile.TemporaryDirectory(prefix="vulkan-allocator-") as directory:
    directory = Path(directory)
    (directory / "allocator_production.inc").write_text(SOURCE[START:END], encoding="utf-8")
    exe = directory / ("allocator.exe" if os.name == "nt" else "allocator")
    command = shlex.split(os.environ.get("CC", "cc")) + shlex.split(os.environ.get("CFLAGS", "")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
        "-I", str(directory), str(ROOT / "tests/vulkan_allocator_test.c"), "-o", str(exe),
    ]
    subprocess.run(command, check=True)
    failures = []
    for scenario in SCENARIOS:
        env = os.environ.copy()
        env.pop("WT_VK_BUDGET", None)
        env.pop("WT_VK_VRAM_MB", None)
        result = subprocess.run([str(exe), scenario], env=env, text=True, capture_output=True, timeout=30)
        print(result.stdout, end="", flush=True)
        if result.returncode:
            failures.append(scenario)
            print(result.stderr, end="", flush=True)
            print(f"FAIL {scenario}: exit {result.returncode}", flush=True)
    if failures:
        raise SystemExit("Vulkan allocator failures: " + ", ".join(failures))
print(f"Vulkan allocator: {len(SCENARIOS)} production-unit scenarios passed")
