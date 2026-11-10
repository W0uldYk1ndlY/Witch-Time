#ifndef WITCHTIME_DIAG_SHIM_H
#define WITCHTIME_DIAG_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_SHIM_VERSION 1
#define DIAG_SHIM_PROGRAM_SUPPORTED 0
#define DIAG_SHIM_SUPPORTED_GAME_HASH "b3_switch_v1.0.0"

#define DIAG_SHIM_CAP_SEMANTIC_OBSERVATIONS (1U << 0)
#define DIAG_SHIM_CAP_REPLAY_VALIDATION      (1U << 1)

typedef enum diag_shim_error {
    DIAG_SHIM_OK = 0,
    DIAG_SHIM_ERR_NULL_BINDING = -1,
    DIAG_SHIM_ERR_UNSUPPORTED_VERSION = -2,
    DIAG_SHIM_ERR_UNSUPPORTED_PROGRAM = -3,
    DIAG_SHIM_ERR_UNSUPPORTED_HASH = -4,
} diag_shim_error_t;

typedef struct diag_semantic_observation {
    int32_t  chapter;
    int32_t  verse;
    int32_t  difficulty;
    int32_t  area;
    bool     movie_active;
    int32_t  draws_seen;
    uint64_t timestamp_ns;
} diag_semantic_observation_t;

typedef struct GameDiagBinding {
    uint32_t version;
    uint32_t program_id;
    const char *game_hash;
    uint32_t capabilities;
    void (*on_semantic_observation)(const diag_semantic_observation_t *obs);
} GameDiagBinding;

/* Exported shim binding entry point */
#ifdef _WIN32
__declspec(dllexport)
#endif
int game_diag_bind(const GameDiagBinding *in_bind, GameDiagBinding *out_bind);

/* Semantic observation publisher & query API */
void diag_publish_semantic_observation(int32_t chapter,
                                      int32_t verse,
                                      int32_t difficulty,
                                      int32_t area,
                                      bool movie_active,
                                      int32_t draws);

bool diag_get_last_semantic_observation(diag_semantic_observation_t *out_obs);

/* Host-side helper to attempt binding; returns true if bound, false if older library without shim */
bool diag_try_bind_shim(void *module_handle, const char *game_hash, uint32_t program_id);

/* Check if diagnostic shim is currently bound */
bool diag_is_shim_bound(void);

#ifdef __cplusplus
}
#endif

#endif /* WITCHTIME_DIAG_SHIM_H */
