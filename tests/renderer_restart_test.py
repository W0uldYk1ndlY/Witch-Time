"""Isolated host gate test: production admission/prepare/cancel, fake driver/queue."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RendererRestartTest(unittest.TestCase):
    def test_entry_coverage(self):
        source = (ROOT / "runtime/nvn_gl.c").read_text()
        table = source[source.index("k_nvn[]"):source.index("#define WG(fn)")]
        entries = set(re.findall(r'\{"nvn[^"]*",\s*(\w+)\}', table))
        wrapped = re.findall(r"WG\((\w+)\)", (ROOT / "runtime/nvn_restart_entries.inc").read_text())
        self.assertTrue(entries)
        self.assertEqual(entries, set(wrapped))
        self.assertEqual(len(wrapped), len(set(wrapped)))
        self.assertIn("return restart_entry(k_nvn[i].fn)", source)
        for name in ("gl_run", "gl_post", "vk_shutdown_at_exit"):
            match = re.search(r"static void " + name + r"\([^;]*?\)\s*\{", source)
            self.assertIsNotNone(match)
            body = source[match.start():]
            body = body[:body.index("\n}")]
            self.assertIn("wt_gpu_restart_enter();", body)
            self.assertIn("wt_gpu_restart_leave();", body)

    @unittest.skipUnless(os.name == "nt" and shutil.which("gcc"), "requires Windows host gcc")
    def test_production_gate(self):
        source = (ROOT / "runtime/nvn_gl.c").read_text()
        admission = source[source.index("/* No FS locks"):source.index("static HWND g_hwnd;")]
        barrier = source[source.index("static Job g_restart_job;"):source.index("/* ---------------------------------------------------------------- statistics")]
        post = source[source.index("static void gl_post("):source.index("static void gl_barrier(")]
        shutdown = source[source.index("static void job_vk_shutdown("):source.index("/* nvnBufferFlushMappedRange / nvnMemoryPoolFlushMappedRange")]
        vk_source = (ROOT / "runtime/nvn_vk.c").read_text()
        vk_idle = vk_source[vk_source.index("int nvn_vk_driver_restart_idle(void) {"):]
        vk_idle = vk_idle[:vk_idle.index("\n}") + 2]
        harness = r'''
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define _Thread_local __thread
typedef struct Job { void (*fn)(void *); void *arg; HANDLE done; struct Job *next; LONG64 t0,t1; } Job;
static DWORD g_render_tid;
static CRITICAL_SECTION g_job_lock;
static Job *g_job_head,*g_job_tail;
static HANDLE g_job_event;
static LONG64 g_job_posted_n;
static volatile LONG g_async_pending;
static int joblog_on(void) { return 0; }
static LONG64 pf_now(void) { return 0; }
static void joblog(const char *s, void (*fn)(void *), const void *a, LONG64 t) { (void)s; (void)fn; (void)a; (void)t; }
static int rb_vk;
static int *g_rb=&rb_vk;
static HANDLE driver_entered,driver_release;
static volatile LONG idle_count,cancel_done;
static void glFinish(void) { assert(0); }
#define VK_SUCCESS 0
static struct { int alive,device; } V={1,1};
static int driver_result;
static int driver_idle(int device) {
    assert(device==1);
    InterlockedIncrement(&idle_count);
    SetEvent(driver_entered);
    WaitForSingleObject(driver_release,INFINITE);
    return driver_result;
}
static int (*vkDeviceWaitIdle)(int)=driver_idle;
VKIDLE
ADMISSION
POST
BARRIER
static volatile LONG shutdown_count;
static void nvn_vk_shutdown(void) { assert(!g_restart_closed); InterlockedIncrement(&shutdown_count); }
SHUTDOWN
static DWORD WINAPI render(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_job_event,INFINITE);
        EnterCriticalSection(&g_job_lock);
        Job *j=g_job_head;
        if(j) { g_job_head=j->next; if(!g_job_head) g_job_tail=NULL; }
        LeaveCriticalSection(&g_job_lock);
        if(j) { j->fn(j->arg); if(j->done) SetEvent(j->done); else { free(j); InterlockedDecrement(&g_async_pending); } SetEvent(g_job_event); }
    }
    return 0;
}
static DWORD WINAPI cancel_thread(LPVOID p) {
    (void)p; wt_gpu_restart_cancel(); InterlockedExchange(&cancel_done,1); return 0;
}
static DWORD WINAPI shutdown_thread(LPVOID p) { (void)p; vk_shutdown_at_exit(); return 0; }
static DWORD WINAPI blocked_entry(LPVOID p) {
    wt_gpu_restart_enter(); SetEvent((HANDLE)p); wt_gpu_restart_leave(); return 0;
}
static volatile LONG published;
static void publish_job(void *p) { (void)p; InterlockedIncrement(&published); }
static DWORD WINAPI admitted_callback(LPVOID p) {
    wt_gpu_restart_enter(); SetEvent((HANDLE)p);
    for (;;) {
        AcquireSRWLockExclusive(&g_restart_lock);
        int closed=g_restart_closed;
        ReleaseSRWLockExclusive(&g_restart_lock);
        if(closed) break;
        Sleep(1);
    }
    /* Guest callback reentry and external publish must finish despite closure. */
    wt_gpu_restart_enter(); gl_post(publish_job,NULL,0); wt_gpu_restart_leave();
    wt_gpu_restart_leave(); return 0;
}
static HANDLE lease_release;
static DWORD WINAPI held_entry(LPVOID p) {
    wt_gpu_restart_enter(); SetEvent((HANDLE)p);
    WaitForSingleObject(lease_release,INFINITE);
    wt_gpu_restart_leave(); return 0;
}
int main(void) {
    InitializeCriticalSection(&g_job_lock);
    g_job_event=CreateEventA(NULL,FALSE,FALSE,NULL);
    driver_entered=CreateEventA(NULL,TRUE,FALSE,NULL);
    driver_release=CreateEventA(NULL,TRUE,TRUE,NULL);
    CreateThread(NULL,0,render,NULL,0,&g_render_tid);
    /* Nested admission is one outer lease; a closing gate cannot strand nesting. */
    wt_gpu_restart_enter(); wt_gpu_restart_enter();
    assert(g_restart_active==1); wt_gpu_restart_leave(); assert(g_restart_active==1);
    wt_gpu_restart_leave(); assert(g_restart_active==0);
    HANDLE admitted=CreateEventA(NULL,TRUE,FALSE,NULL);
    HANDLE callback=CreateThread(NULL,0,admitted_callback,admitted,0,NULL);
    assert(WaitForSingleObject(admitted,1000)==WAIT_OBJECT_0);
    assert(wt_gpu_restart_prepare(1000)==1); assert(idle_count==1);
    assert(WaitForSingleObject(callback,1000)==WAIT_OBJECT_0); assert(published==1);
    HANDLE entered=CreateEventA(NULL,TRUE,FALSE,NULL);
    HANDLE blocked=CreateThread(NULL,0,blocked_entry,entered,0,NULL);
    assert(WaitForSingleObject(entered,30)==WAIT_TIMEOUT);
    wt_gpu_restart_cancel(); assert(WaitForSingleObject(blocked,1000)==WAIT_OBJECT_0);
    /* Driver idle outlives prepare timeout: cancel must not reopen under it. */
    ResetEvent(driver_entered); ResetEvent(driver_release);
    assert(wt_gpu_restart_prepare(20)==0);
    assert(WaitForSingleObject(driver_entered,1000)==WAIT_OBJECT_0);
    HANDLE cancel=CreateThread(NULL,0,cancel_thread,NULL,0,NULL);
    assert(WaitForSingleObject(cancel,30)==WAIT_TIMEOUT);
    assert(g_restart_closed && !cancel_done && g_restart_pending);
    SetEvent(driver_release);
    assert(WaitForSingleObject(cancel,1000)==WAIT_OBJECT_0);
    assert(!g_restart_closed && !g_restart_pending);
    assert(wt_gpu_restart_prepare(1000)==1); wt_gpu_restart_cancel();
    driver_result=-4;
    assert(wt_gpu_restart_prepare(1000)==0); assert(g_restart_closed);
    assert(V.alive && V.device==1); wt_gpu_restart_cancel();
    ResetEvent(admitted); lease_release=CreateEventA(NULL,TRUE,FALSE,NULL);
    HANDLE held=CreateThread(NULL,0,held_entry,admitted,0,NULL);
    assert(WaitForSingleObject(admitted,1000)==WAIT_OBJECT_0);
    LONG before=idle_count;
    assert(wt_gpu_restart_prepare(10)==0); assert(idle_count==before);
    assert(g_restart_closed && !g_restart_pending);
    SetEvent(lease_release); assert(WaitForSingleObject(held,1000)==WAIT_OBJECT_0);
    wt_gpu_restart_cancel(); assert(!g_restart_closed);
    driver_result=0; assert(wt_gpu_restart_prepare(1000)==1);
    HANDLE shutdown=CreateThread(NULL,0,shutdown_thread,NULL,0,NULL);
    assert(WaitForSingleObject(shutdown,30)==WAIT_TIMEOUT); assert(!shutdown_count);
    wt_gpu_restart_cancel();
    assert(WaitForSingleObject(shutdown,1000)==WAIT_OBJECT_0); assert(shutdown_count==1);
    before=idle_count; assert(wt_gpu_restart_prepare(1000)==0);
    assert(idle_count==before); wt_gpu_restart_cancel();
    return 0;
}
'''.replace("ADMISSION", admission).replace("POST", post).replace("BARRIER", barrier).replace("SHUTDOWN", shutdown).replace("VKIDLE", vk_idle)
        temp_root = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode"
        with tempfile.TemporaryDirectory(prefix="renderer-gate-", dir=temp_root) as tmp:
            c = Path(tmp) / "gate.c"
            exe = Path(tmp) / "gate.exe"
            c.write_text(harness)
            subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
