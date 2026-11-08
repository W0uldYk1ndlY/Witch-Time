/* Linked into game.dll together with the lifted game code (obj\*.obj).
 *
 * The lifted code is 700+ MB of objects that change only when the lifter changes; keeping it in a
 * DLL means the host runtime (the part edited all day) links in a second or two. The DLL cannot
 * import from the exe, so the six host functions the lifted code calls are forwarded through
 * pointers the exe hands over once at start-up (gamedll.c). */
#include "runtime.h"
#include "game_host.h"
#ifdef __ANDROID__
#include <stdlib.h>
#endif

typedef struct {
    const FnEntry *fn_table;
    const int *fn_table_count;
    const DynSym *dyn_syms;
    const int *dyn_sym_count;
} GameTables;

static GameHost host;

void call_indirect(Ctx *c, uint64_t addr) { host.call_indirect(c, addr); }
void host_import(Ctx *c, int sym) { host.host_import(c, sym); }
void guest_trap(const char *what, uint64_t addr) { host.guest_trap(what, addr); }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { host.guest_unimpl(pc, insn, what); }
void guest_svc(Ctx *c, uint32_t number) { host.guest_svc(c, number); }
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) { host.stx_diag(c, a, expected, seen, v, bits); }
#if defined(_WIN32) || defined(__ANDROID__)
/* [witchtime] the lifted sdk (gen/sdklift_gen.c) is linked into game.dll with the player's other tables (runtime/split);
 * on desktop Linux game.so finds these three in the runtime when it is loaded. Android cannot depend on a
 * reverse global lookup into the runtime that itself loads libgame.so, so it uses the same callback boundary. */
void sdk_dispatch(Ctx *c, uint64_t addr) { host.sdk_dispatch(c, addr); }
void sdk_host_call(Ctx *c, int index) { host.sdk_host_call(c, index); }
void sdk_trap_call(Ctx *c, const char *name) { host.sdk_trap_call(c, name); }
#endif
#ifdef __ANDROID__
struct SdkJmpRec *sdk_jmp_record(Ctx *c) { return host.sdk_jmp_record(c); }
void sdk_jmp_restore(Ctx *c, struct SdkJmpRec *record) { host.sdk_jmp_restore(c, record); }
void sdk_longjmp(Ctx *c) { host.sdk_longjmp(c); }
#endif
#ifdef WT_GAME_NOSTDLIB
/* [witchtime] game.dll as wtbuild links it on the player's PC: without the C runtime's start-up code (-nostdlib), so its
 * entry point is this; nothing in it needs initialising (no constructors, no thread-local data) */
int __stdcall DllMainCRTStartup(void *module, unsigned long reason, void *reserved) {
    (void)module; (void)reason; (void)reserved;
    return 1;
}
#endif

__declspec(dllexport) void game_bind(const GameHost *h, GameTables *t) {
#ifdef __ANDROID__
    /* Fail at the binding boundary rather than later through an uninitialized function pointer. */
    if (!h || !t || !h->call_indirect || !h->host_import || !h->guest_trap || !h->guest_unimpl ||
        !h->guest_svc || !h->stx_diag || !h->sdk_dispatch || !h->sdk_host_call || !h->sdk_trap_call ||
        !h->sdk_jmp_record || !h->sdk_jmp_restore || !h->sdk_longjmp) abort();
#endif
    host = *h;
    t->fn_table = fn_table;
    t->fn_table_count = &fn_table_count;
    t->dyn_syms = dyn_syms;
    t->dyn_sym_count = &dyn_sym_count;
}
