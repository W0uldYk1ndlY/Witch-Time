#!/usr/bin/env python3
"""Exercise the production prompt-origin decisions with mocked pad configuration."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "runtime/nnpad_sdl.c").read_text(encoding="utf-8")
origin = source[source.index("static volatile int g_prompts_kb;"):source.index("/* swap_confirm in force:")]
swap = source[source.index("int nnpad_menu_swapped(void) {"):source.index("int nnpad_button_prompts(void) {")]
auto = source[source.index("int nnpad_button_prompts(void) {"):source.index('    const char *seq = getenv("WT_SEQ")', source.index("int nnpad_button_prompts(void) {"))]
letter = source[source.index("int nnpad_prompts_by_letter(void) {"):source.index("    lock();", source.index("int nnpad_prompts_by_letter(void) {"))]
code = r'''
#include <assert.h>
#include <stdio.h>
static struct { int prompts, input, menu_swap; char path[4]; } g_cfg = {0, 0, 1, "ok"};
static void lock(void) { } static void unlock(void) { } static void cfg_load(int x) { (void)x; }
int nnpad_button_prompts(void);
''' + origin + swap + auto + '    return 77;\n}\n' + letter + '    return 77;\n}\n' + r'''
int main(void) {
    nnpad_prompts_used(2);
    assert(nnpad_button_prompts()==1 && nnpad_bind_live() && nnpad_menu_swapped() && !nnpad_prompts_by_letter());
    g_cfg.prompts=3; assert(nnpad_button_prompts()==3); /* explicit family wins */
    g_cfg.prompts=0; g_cfg.input=1; assert(nnpad_button_prompts()==1); /* touch still wins auto */
    nnpad_prompts_used(1); assert(nnpad_button_prompts()==4 && !nnpad_bind_live() && !nnpad_menu_swapped());
    g_cfg.input=0; nnpad_prompts_used(0); assert(nnpad_button_prompts()==77 && nnpad_bind_live());
    puts("touch prompt-origin production snippets passed"); return 0;
}
'''
cc = os.environ.get("HOST_CC") or shutil.which("gcc") or shutil.which("cc")
if not cc: raise RuntimeError("C compiler required")
with tempfile.TemporaryDirectory(prefix="touch-prompts-") as temp:
    path = Path(temp); (path / "test.c").write_text(code, encoding="utf-8")
    exe = path / ("test.exe" if os.name == "nt" else "test")
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", str(path / "test.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
