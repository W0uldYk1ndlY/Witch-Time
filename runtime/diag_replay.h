#ifndef WITCHTIME_DIAG_REPLAY_H
#define WITCHTIME_DIAG_REPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_REPLAY_MAGIC 0x50525457ULL       /* "WTRP" in little-endian */
#define DIAG_REPLAY_VERSION_MAJOR 1
#define DIAG_REPLAY_VERSION_MINOR 0
#define DIAG_REPLAY_MAX_RECORDS 500000
#define DIAG_REPLAY_MAX_FILE_BYTES (16 * 1024 * 1024) /* 16 MiB ceiling */
#define DIAG_REPLAY_EOF_MARKER 0x444E455FULL  /* "_END" in little-endian */

/* Switch Pro Controller FullKey button bits */
#define DIAG_REPLAY_BTN_A        (1ULL << 0)
#define DIAG_REPLAY_BTN_B        (1ULL << 1)
#define DIAG_REPLAY_BTN_X        (1ULL << 2)
#define DIAG_REPLAY_BTN_Y        (1ULL << 3)
#define DIAG_REPLAY_BTN_STICK_L  (1ULL << 4)
#define DIAG_REPLAY_BTN_STICK_R  (1ULL << 5)
#define DIAG_REPLAY_BTN_L        (1ULL << 6)
#define DIAG_REPLAY_BTN_R        (1ULL << 7)
#define DIAG_REPLAY_BTN_ZL       (1ULL << 8)
#define DIAG_REPLAY_BTN_ZR       (1ULL << 9)
#define DIAG_REPLAY_BTN_PLUS     (1ULL << 10)
#define DIAG_REPLAY_BTN_MINUS    (1ULL << 11)
#define DIAG_REPLAY_BTN_D_LEFT   (1ULL << 12)
#define DIAG_REPLAY_BTN_D_UP     (1ULL << 13)
#define DIAG_REPLAY_BTN_D_RIGHT  (1ULL << 14)
#define DIAG_REPLAY_BTN_D_DOWN   (1ULL << 15)

#define DIAG_REPLAY_VALID_BUTTON_MASK 0x000000000000FFFFULL

/* Attributes mask */
#define DIAG_REPLAY_ATTR_CONNECTED (1U << 0)
#define DIAG_REPLAY_ATTR_WIRED     (1U << 1)
#define DIAG_REPLAY_VALID_ATTR_MASK (DIAG_REPLAY_ATTR_CONNECTED | DIAG_REPLAY_ATTR_WIRED)

typedef enum diag_replay_source_policy {
    DIAG_REPLAY_SOURCE_UNKNOWN = 0,
    DIAG_REPLAY_SOURCE_SYNTHETIC_FULLKEY_P1 = 1,
} diag_replay_source_policy_t;

typedef enum diag_replay_mapping_policy {
    DIAG_REPLAY_MAPPING_UNKNOWN = 0,
    DIAG_REPLAY_MAPPING_DEFAULT_TYPE_A = 1,
} diag_replay_mapping_policy_t;

typedef enum diag_replay_confirm_policy {
    DIAG_REPLAY_CONFIRM_UNKNOWN = 0,
    DIAG_REPLAY_CONFIRM_A = 1,
    DIAG_REPLAY_CONFIRM_B = 2,
} diag_replay_confirm_policy_t;

#pragma pack(push, 1)

/* 64-byte file header */
typedef struct diag_replay_header {
    uint32_t magic;               /* DIAG_REPLAY_MAGIC */
    uint16_t version_major;       /* 1 */
    uint16_t version_minor;       /* 0 */
    uint32_t source_policy;       /* diag_replay_source_policy_t */
    uint32_t mapping_policy;      /* diag_replay_mapping_policy_t */
    uint32_t confirm_policy;      /* diag_replay_confirm_policy_t */
    uint32_t record_count;        /* Number of records */
    uint64_t total_polls;         /* Total polls represented */
    uint32_t flags;               /* Reserved flags */
    uint32_t crc32;               /* Optional CRC32 */
    uint8_t  reserved[28];        /* Zeroed padding to 64 bytes */
} diag_replay_header_t;

/* 32-byte fixed-size record */
typedef struct diag_replay_record {
    uint64_t poll;                /* Strictly monotonic poll sequence */
    int32_t  anchor;              /* Anchor class (-1 if none) */
    uint32_t anchor_poll;         /* Polls elapsed since anchor */
    uint64_t buttons;             /* Button bitmask */
    int16_t  lx;                  /* Left stick X [-32768..32767] */
    int16_t  ly;                  /* Left stick Y [-32768..32767] */
    int16_t  rx;                  /* Right stick X [-32768..32767] */
    int16_t  ry;                  /* Right stick Y [-32768..32767] */
    uint32_t attributes;          /* Connection attributes */
} diag_replay_record_t;

/* 16-byte footer */
typedef struct diag_replay_footer {
    uint32_t eof_marker;          /* DIAG_REPLAY_EOF_MARKER */
    uint32_t record_count_check;  /* Must equal header.record_count */
    uint64_t checksum;            /* Verification checksum */
} diag_replay_footer_t;

#pragma pack(pop)

/* Error codes */
typedef enum diag_replay_error {
    DIAG_REPLAY_OK = 0,
    DIAG_REPLAY_ERR_FILE_NOT_FOUND = -1,
    DIAG_REPLAY_ERR_EMPTY_FILE = -2,
    DIAG_REPLAY_ERR_FILE_TOO_LARGE = -3,
    DIAG_REPLAY_ERR_BAD_HEADER = -4,
    DIAG_REPLAY_ERR_UNSUPPORTED_VERSION = -5,
    DIAG_REPLAY_ERR_UNSUPPORTED_SOURCE_POLICY = -6,
    DIAG_REPLAY_ERR_INCOMPATIBLE_MAPPING = -7,
    DIAG_REPLAY_ERR_INCOMPATIBLE_CONFIRM = -8,
    DIAG_REPLAY_ERR_CORRUPT_RECORD = -9,
    DIAG_REPLAY_ERR_ORDER_VIOLATION = -10,
    DIAG_REPLAY_ERR_OUT_OF_RANGE_AXIS = -11,
    DIAG_REPLAY_ERR_INVALID_BUTTONS = -12,
    DIAG_REPLAY_ERR_INVALID_ATTRIBUTES = -13,
    DIAG_REPLAY_ERR_MISSING_END_MARKER = -14,
    DIAG_REPLAY_ERR_SIZE_MISMATCH = -15,
} diag_replay_error_t;

typedef struct diag_replay_session {
    diag_replay_header_t header;
    diag_replay_record_t *records;
    size_t record_count;
    size_t current_pos;
    bool is_loaded;
    bool has_failed;
    bool is_cancelled;
    bool is_eof;
    diag_replay_error_t error_code;
    char error_msg[128];
} diag_replay_session_t;

/* Parse and strictly validate a replay file */
diag_replay_error_t diag_replay_load(const char *path,
                                    diag_replay_source_policy_t required_source,
                                    diag_replay_mapping_policy_t required_mapping,
                                    diag_replay_confirm_policy_t required_confirm,
                                    diag_replay_session_t *out_session);

/* Free allocated session resources */
void diag_replay_free(diag_replay_session_t *session);

/* Query input for current poll and anchor. Returns true if valid replay sample provided */
bool diag_replay_poll(diag_replay_session_t *session,
                      int anchor,
                      uint32_t anchor_poll,
                      uint64_t *out_buttons,
                      int16_t *out_lx,
                      int16_t *out_ly,
                      int16_t *out_rx,
                      int16_t *out_ry,
                      uint32_t *out_attributes);

/* Serialize replay file (for testing, fixture generation, recording) */
diag_replay_error_t diag_replay_write(const char *path,
                                     const diag_replay_header_t *header,
                                     const diag_replay_record_t *records,
                                     size_t count);

const char *diag_replay_error_str(diag_replay_error_t err);

/* Input policy verification for Profile 1 */
typedef struct diag_input_policy_check {
    const char *source_policy;    /* e.g. "synthetic_fullkey_player1" */
    const char *mapping_policy;   /* e.g. "default_type_a" */
    const char *confirm_policy;   /* e.g. "native_a" */
    bool is_keyboard_origin;      /* true if keyboard is player 1 */
    bool is_custom_binding;       /* true if non-Type-A binding is used */
    bool is_menu_swapped;         /* true if swap_confirm == on */
} diag_input_policy_check_t;

typedef struct diag_input_policy_result {
    bool accepted;
    const char *reason_code;      /* stable reason code */
    char details[128];
} diag_input_policy_result_t;

diag_input_policy_result_t diag_verify_input_policy(const diag_input_policy_check_t *check);

#define DIAG_CONFIRM_BIT_A DIAG_REPLAY_BTN_A
#define DIAG_CANCEL_BIT_B  DIAG_REPLAY_BTN_B

/* Synthetic controller discovery & isolation API */
void diag_input_enable_synthetic_player1(bool enable);
bool diag_input_is_synthetic_player1_enabled(void);
void diag_input_set_active_replay(diag_replay_session_t *sess);
diag_replay_session_t *diag_input_get_active_replay(void);
bool diag_input_is_player_connected(uint32_t id,
                                   uint32_t supported_styles,
                                   const uint32_t *supported_ids,
                                   int supported_id_count,
                                   bool has_physical_controller);
uint64_t diag_replay_get_confirm_button(diag_replay_confirm_policy_t policy);
uint64_t diag_replay_get_cancel_button(diag_replay_confirm_policy_t policy);

/* Delivered state tracking and assertion API */
typedef struct diag_delivered_pad_state {
    uint64_t sampling_number;
    uint64_t raw_buttons;
    uint64_t composed_buttons;
    int32_t  lx;
    int32_t  ly;
    int32_t  rx;
    int32_t  ry;
    uint32_t attributes;
    bool     is_gated;
} diag_delivered_pad_state_t;

void diag_record_delivered_pad_state(uint32_t id, const diag_delivered_pad_state_t *st);
bool diag_get_last_delivered_pad_state(uint32_t id, diag_delivered_pad_state_t *out_st);
void diag_replay_cancel(diag_replay_session_t *session);
void diag_replay_set_eof(diag_replay_session_t *session, bool is_eof);

/* Canonical composition helper for tests and verification */
void diag_compose_fullkey_state(uint64_t raw_buttons,
                               int16_t lx, int16_t ly,
                               int16_t rx, int16_t ry,
                               uint32_t attributes,
                               uint64_t *out_composed_buttons,
                               uint32_t *out_attributes);

#ifdef __cplusplus
}
#endif

#endif /* WITCHTIME_DIAG_REPLAY_H */
