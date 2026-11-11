"""Exercise the production upscaler and variant parsers without a game or device."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "runtime" / "gfx_options.c").read_text(encoding="utf-8")
mode = re.search(r'static int upscaler_of\(.*?\n\}', source, re.S)
variant = re.search(r'int gfx_sgsr_variant\(void\) \{.*?\n\}', source, re.S)
assert mode and variant
assert source.index('else if (!_stricmp(key, "upscaler"))') < source.index('if (getenv("WT_UPSCALER"))'), "environment override must follow file parsing"
fsr = (root / "runtime" / "nvn_vk_fsr.c").read_text(encoding="utf-8")
assert "g_rs_mode >= 1 && g_rs_mode <= 5" in fsr, "SGSR values must not use FSR routing"
harness = r'''
#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <string.h>
static const char *env_value;
static int insensitive(const char *a, const char *b) {
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
#define _stricmp insensitive
#define getenv(x) (env_value)
''' + mode.group(0) + '\n' + variant.group(0) + r'''
int main(void) {
    const char *modes[] = {"off", "fsr3", "fsr3_quality", "fsr3_balanced", "fsr3_performance", "fsr3_ultra_performance", "sgsr2", "sgsr2_quality", "sgsr2_performance"};
    for (int i = 0; i < 9; i++) assert(upscaler_of(modes[i]) == i);
    assert(upscaler_of("SGSR2_QUALITY") == 7);
    assert(upscaler_of("not-a-mode") == 0);
    env_value = NULL; assert(gfx_sgsr_variant() == 0);
    env_value = "2-pass-fs"; assert(gfx_sgsr_variant() == 0);
    env_value = "2-PASS-CS"; assert(gfx_sgsr_variant() == 1);
    env_value = "3-pass-cs"; assert(gfx_sgsr_variant() == 2);
    env_value = ""; assert(gfx_sgsr_variant() == -1);
    env_value = "other"; assert(gfx_sgsr_variant() == -1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="upscaler-config-") as temp:
    temp = Path(temp)
    c_file, exe = temp / "test.c", temp / ("test.exe" if os.name == "nt" else "test")
    c_file.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_file), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("upscaler parser checks passed; menu integration and device execution not checked")
