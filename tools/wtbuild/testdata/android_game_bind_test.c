/* Host-executable regression of the Android game DSO callback boundary. */
#define __ANDROID__ 1
#define __declspec(x)
#include "../../../runtime/game_shim.c"
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

const FnEntry fn_table[] = {{0, 0}};
const int fn_table_count = 1;
const DynSym dyn_syms[] = {{0, 0, 0}};
const int dyn_sym_count = 1;
static int called;
struct SdkJmpRec { int value; } jump_record = {42};
static struct SdkJmpRec *record(Ctx *c) { assert(c->x[0] == 17); called |= 8; return &jump_record; }
static void restore(Ctx *c, struct SdkJmpRec *r) { assert(c->x[0] == 17 && r == &jump_record); called |= 16; }
static void jump(Ctx *c) { assert(c->x[0] == 17); called |= 32; }
static void dispatch(Ctx *c, uint64_t addr) { assert(c->x[0] == 17 && addr == 42); called |= 1; }
static void host_call(Ctx *c, int index) { assert(c->x[0] == 17 && index == 3); called |= 2; }
static void trap_call(Ctx *c, const char *name) { assert(c->x[0] == 17 && !strcmp(name, "test")); called |= 4; }
static void trap(const char *name, uint64_t addr) { (void)name; (void)addr; }
static void unimpl(uint64_t pc, uint32_t insn, const char *name) { (void)pc; (void)insn; (void)name; }
static void svc(Ctx *c, uint32_t n) { (void)c; (void)n; }
static void diag(Ctx *c, uint64_t a, uint64_t b, uint64_t d, uint64_t e, int n) { (void)c; (void)a; (void)b; (void)d; (void)e; (void)n; }

int main(void) {
    struct rlimit no_core = {0, 0}; setrlimit(RLIMIT_CORE, &no_core);
    Ctx c = {0}; c.x[0] = 17;
    GameTables tables;
    GameHost callbacks = {dispatch, host_call, trap, unimpl, svc, diag, dispatch, host_call, trap_call, record, restore, jump};
    game_bind(&callbacks, &tables);
    assert(tables.fn_table == fn_table && tables.fn_table_count == &fn_table_count);
    assert(tables.dyn_syms == dyn_syms && tables.dyn_sym_count == &dyn_sym_count);
    sdk_dispatch(&c, 42);
    sdk_host_call(&c, 3);
    sdk_trap_call(&c, "test");
    assert(sdk_jmp_record(&c) == &jump_record);
    sdk_jmp_restore(&c, &jump_record);
    sdk_longjmp(&c);
    assert(called == 63);
    callbacks.sdk_host_call = 0;
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { game_bind(&callbacks, &tables); _exit(0); }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    return 0;
}
