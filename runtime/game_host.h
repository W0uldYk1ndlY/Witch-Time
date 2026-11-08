/* The callback ABI shared by the runtime and the separately compiled game library. */
#ifndef WT_GAME_HOST_H
#define WT_GAME_HOST_H
#include "runtime.h"
#ifdef __ANDROID__
struct SdkJmpRec;
struct SdkJmpRec *sdk_jmp_record(Ctx *c);
void sdk_jmp_restore(Ctx *c, struct SdkJmpRec *record);
void sdk_longjmp(Ctx *c);
#endif
typedef struct {
    void (*call_indirect)(Ctx *, uint64_t);
    void (*host_import)(Ctx *, int);
    void (*guest_trap)(const char *, uint64_t);
    void (*guest_unimpl)(uint64_t, uint32_t, const char *);
    void (*guest_svc)(Ctx *, uint32_t);
    void (*stx_diag)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
    void (*sdk_dispatch)(Ctx *, uint64_t);
    void (*sdk_host_call)(Ctx *, int);
    void (*sdk_trap_call)(Ctx *, const char *);
#ifdef __ANDROID__
    struct SdkJmpRec *(*sdk_jmp_record)(Ctx *);
    void (*sdk_jmp_restore)(Ctx *, struct SdkJmpRec *);
    void (*sdk_longjmp)(Ctx *);
#endif
} GameHost;
#endif
