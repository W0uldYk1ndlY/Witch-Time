/* Host side of the exe / game.dll split (see game_shim.c).
 *
 * build.py compiles every host file with
 *     -Dfn_table=(*game_fn_table_p) -Dfn_table_count=(*game_fn_table_count_p)
 *     -Ddyn_syms=(*game_dyn_syms_p) -Ddyn_sym_count=(*game_dyn_sym_count_p)
 * so the tables declared in runtime.h become pointers into the DLL, filled in here before main(). */
#include "runtime.h"
#include "game_host.h"

typedef struct {
    const FnEntry *table;
    const int *table_count;
    const DynSym *syms;
    const int *sym_count;
} GameTables;

void sdk_dispatch(Ctx *c, uint64_t addr);     /* sdklift_rt.c */
void sdk_host_call(Ctx *c, int index);
void sdk_trap_call(Ctx *c, const char *name);
__declspec(dllimport) void game_bind(const GameHost *h, GameTables *t);

#undef fn_table
#undef fn_table_count
#undef dyn_syms
#undef dyn_sym_count
const FnEntry (*game_fn_table_p)[];
const int *game_fn_table_count_p;
const DynSym (*game_dyn_syms_p)[];
const int *game_dyn_sym_count_p;

__attribute__((constructor)) static void bind_game(void) {
    GameHost h = {call_indirect, host_import, guest_trap, guest_unimpl, guest_svc, stx_diag, sdk_dispatch, sdk_host_call, sdk_trap_call};
#ifdef __ANDROID__
    h.sdk_jmp_record = sdk_jmp_record;
    h.sdk_jmp_restore = sdk_jmp_restore;
    h.sdk_longjmp = sdk_longjmp;
#endif
    GameTables t;
    game_bind(&h, &t);
    game_fn_table_p = (const FnEntry (*)[])t.table;
    game_fn_table_count_p = t.table_count;
    game_dyn_syms_p = (const DynSym (*)[])t.syms;
    game_dyn_sym_count_p = t.sym_count;
}
