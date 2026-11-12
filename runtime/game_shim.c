/* Linked into game.dll together with the lifted game code (obj\*.obj).
 *
 * The lifted code is 700+ MB of objects that change only when the lifter changes; keeping it in a
 * DLL means the host runtime (the part edited all day) links in a second or two. The DLL cannot
 * import from the exe, so the six host functions the lifted code calls are forwarded through
 * pointers the exe hands over once at start-up (gamedll.c). */
#include "runtime.h"
#include "game_host.h"
#include "diag_shim.h"
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

#if defined(__x86_64__)
#include "soft_fmaf.h"
#include <stdlib.h>
/* [witchtime] every fmaf call of the lifted code (soft_fmaf.h). Hidden: on ELF a call from inside game.so would otherwise go to the C
 * library's. WT_NO_FMA3=1 (test): the path of a CPU without FMA3 on one that has it. */
__attribute__((visibility("hidden"))) float fmaf(float x, float y, float z) {
    static int hw = -1;     /* whichever thread comes first sets it, each to the same value */
    if (hw < 0) hw = wt_has_fma3() && !getenv("WT_NO_FMA3");
    return hw ? wt_fmaf_hw(x, y, z) : wt_fmaf_soft(x, y, z);
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

static GameDiagBinding g_active_diag_binding;
static bool g_diag_bound = false;

__declspec(dllexport) int game_diag_bind(const GameDiagBinding *in_bind, GameDiagBinding *out_bind) {
    if (!in_bind || !out_bind) return DIAG_SHIM_ERR_NULL_BINDING;
    if (in_bind->version != DIAG_SHIM_VERSION) return DIAG_SHIM_ERR_UNSUPPORTED_VERSION;
    if (in_bind->program_id != DIAG_SHIM_PROGRAM_SUPPORTED) return DIAG_SHIM_ERR_UNSUPPORTED_PROGRAM;
    if (!in_bind->game_hash || strcmp(in_bind->game_hash, DIAG_SHIM_SUPPORTED_GAME_HASH) != 0) return DIAG_SHIM_ERR_UNSUPPORTED_HASH;

    g_active_diag_binding = *in_bind;
    g_diag_bound = true;

    out_bind->version = DIAG_SHIM_VERSION;
    out_bind->program_id = DIAG_SHIM_PROGRAM_SUPPORTED;
    out_bind->game_hash = DIAG_SHIM_SUPPORTED_GAME_HASH;
    out_bind->capabilities = DIAG_SHIM_CAP_SEMANTIC_OBSERVATIONS | DIAG_SHIM_CAP_REPLAY_VALIDATION;
    out_bind->on_semantic_observation = in_bind->on_semantic_observation;

    return DIAG_SHIM_OK;
}
