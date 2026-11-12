#include "diag_replay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *diag_replay_error_str(diag_replay_error_t err) {
    switch (err) {
        case DIAG_REPLAY_OK: return "OK";
        case DIAG_REPLAY_ERR_FILE_NOT_FOUND: return "File not found";
        case DIAG_REPLAY_ERR_EMPTY_FILE: return "File is empty";
        case DIAG_REPLAY_ERR_FILE_TOO_LARGE: return "File exceeds maximum size ceiling";
        case DIAG_REPLAY_ERR_BAD_HEADER: return "Invalid replay magic or corrupt header";
        case DIAG_REPLAY_ERR_UNSUPPORTED_VERSION: return "Unsupported schema major version";
        case DIAG_REPLAY_ERR_UNSUPPORTED_SOURCE_POLICY: return "Unsupported controller source policy";
        case DIAG_REPLAY_ERR_INCOMPATIBLE_MAPPING: return "Incompatible controller button mapping";
        case DIAG_REPLAY_ERR_INCOMPATIBLE_CONFIRM: return "Incompatible menu confirm policy";
        case DIAG_REPLAY_ERR_CORRUPT_RECORD: return "Corrupted record or footer count mismatch";
        case DIAG_REPLAY_ERR_ORDER_VIOLATION: return "Poll sequence order violation or duplicate";
        case DIAG_REPLAY_ERR_OUT_OF_RANGE_AXIS: return "Analog stick axis value out of valid range";
        case DIAG_REPLAY_ERR_INVALID_BUTTONS: return "Invalid or unsupported button bits set";
        case DIAG_REPLAY_ERR_INVALID_ATTRIBUTES: return "Invalid connection attributes";
        case DIAG_REPLAY_ERR_MISSING_END_MARKER: return "Missing or corrupt EOF marker";
        case DIAG_REPLAY_ERR_SIZE_MISMATCH: return "File size does not match record count";
        default: return "Unknown replay error";
    }
}

diag_replay_error_t diag_replay_load(const char *path,
                                    diag_replay_source_policy_t required_source,
                                    diag_replay_mapping_policy_t required_mapping,
                                    diag_replay_confirm_policy_t required_confirm,
                                    diag_replay_session_t *out_session) {
    if (!out_session) return DIAG_REPLAY_ERR_BAD_HEADER;
    memset(out_session, 0, sizeof(diag_replay_session_t));

    if (!path || path[0] == '\0') {
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_FILE_NOT_FOUND;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "%s", diag_replay_error_str(DIAG_REPLAY_ERR_FILE_NOT_FOUND));
        return DIAG_REPLAY_ERR_FILE_NOT_FOUND;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_FILE_NOT_FOUND;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "%s: %s", diag_replay_error_str(DIAG_REPLAY_ERR_FILE_NOT_FOUND), path);
        return DIAG_REPLAY_ERR_FILE_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_EMPTY_FILE;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "%s", diag_replay_error_str(DIAG_REPLAY_ERR_EMPTY_FILE));
        return DIAG_REPLAY_ERR_EMPTY_FILE;
    }

    if (file_size > DIAG_REPLAY_MAX_FILE_BYTES) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_FILE_TOO_LARGE;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "%s (%ld bytes)", diag_replay_error_str(DIAG_REPLAY_ERR_FILE_TOO_LARGE), file_size);
        return DIAG_REPLAY_ERR_FILE_TOO_LARGE;
    }

    if ((size_t)file_size < sizeof(diag_replay_header_t) + sizeof(diag_replay_footer_t)) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_BAD_HEADER;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "%s", diag_replay_error_str(DIAG_REPLAY_ERR_BAD_HEADER));
        return DIAG_REPLAY_ERR_BAD_HEADER;
    }

    /* Read header */
    diag_replay_header_t header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_BAD_HEADER;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "%s", diag_replay_error_str(DIAG_REPLAY_ERR_BAD_HEADER));
        return DIAG_REPLAY_ERR_BAD_HEADER;
    }

    if (header.magic != DIAG_REPLAY_MAGIC) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_BAD_HEADER;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Magic 0x%08X != expected 0x%08X", (unsigned int)header.magic, (unsigned int)DIAG_REPLAY_MAGIC);
        return DIAG_REPLAY_ERR_BAD_HEADER;
    }

    if (header.version_major != DIAG_REPLAY_VERSION_MAJOR) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_UNSUPPORTED_VERSION;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Major version %u != %u", header.version_major, DIAG_REPLAY_VERSION_MAJOR);
        return DIAG_REPLAY_ERR_UNSUPPORTED_VERSION;
    }

    if (header.record_count > DIAG_REPLAY_MAX_RECORDS) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_FILE_TOO_LARGE;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Record count %u exceeds max %u", header.record_count, DIAG_REPLAY_MAX_RECORDS);
        return DIAG_REPLAY_ERR_FILE_TOO_LARGE;
    }

    size_t expected_size = sizeof(diag_replay_header_t) +
                           ((size_t)header.record_count * sizeof(diag_replay_record_t)) +
                           sizeof(diag_replay_footer_t);
    if ((size_t)file_size != expected_size) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_SIZE_MISMATCH;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "File size %ld != expected %zu", file_size, expected_size);
        return DIAG_REPLAY_ERR_SIZE_MISMATCH;
    }

    /* Policy assertions */
    if (required_source != DIAG_REPLAY_SOURCE_UNKNOWN && header.source_policy != (uint32_t)required_source) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_UNSUPPORTED_SOURCE_POLICY;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Source policy %u != required %u", header.source_policy, required_source);
        return DIAG_REPLAY_ERR_UNSUPPORTED_SOURCE_POLICY;
    }

    if (required_mapping != DIAG_REPLAY_MAPPING_UNKNOWN && header.mapping_policy != (uint32_t)required_mapping) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_INCOMPATIBLE_MAPPING;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Mapping policy %u != required %u", header.mapping_policy, required_mapping);
        return DIAG_REPLAY_ERR_INCOMPATIBLE_MAPPING;
    }

    if (required_confirm != DIAG_REPLAY_CONFIRM_UNKNOWN && header.confirm_policy != (uint32_t)required_confirm) {
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_INCOMPATIBLE_CONFIRM;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Confirm policy %u != required %u", header.confirm_policy, required_confirm);
        return DIAG_REPLAY_ERR_INCOMPATIBLE_CONFIRM;
    }

    /* Allocate and read records */
    diag_replay_record_t *records = NULL;
    if (header.record_count > 0) {
        records = (diag_replay_record_t *)malloc((size_t)header.record_count * sizeof(diag_replay_record_t));
        if (!records) {
            fclose(f);
            out_session->has_failed = true;
            out_session->error_code = DIAG_REPLAY_ERR_FILE_TOO_LARGE;
            snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Out of memory allocating %u records", header.record_count);
            return DIAG_REPLAY_ERR_FILE_TOO_LARGE;
        }

        size_t read_records = fread(records, sizeof(diag_replay_record_t), header.record_count, f);
        if (read_records != header.record_count) {
            free(records);
            fclose(f);
            out_session->has_failed = true;
            out_session->error_code = DIAG_REPLAY_ERR_CORRUPT_RECORD;
            snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Read only %zu of %u records", read_records, header.record_count);
            return DIAG_REPLAY_ERR_CORRUPT_RECORD;
        }

        /* Strictly validate each record */
        for (uint32_t i = 0; i < header.record_count; i++) {
            /* Monotonic poll order */
            if (i > 0 && records[i].poll <= records[i - 1].poll) {
                out_session->has_failed = true;
                out_session->error_code = DIAG_REPLAY_ERR_ORDER_VIOLATION;
                snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Record %u poll %llu <= previous %llu", i,
                         (unsigned long long)records[i].poll, (unsigned long long)records[i - 1].poll);
                free(records);
                fclose(f);
                return DIAG_REPLAY_ERR_ORDER_VIOLATION;
            }

            /* Valid button bits */
            if (records[i].buttons & ~DIAG_REPLAY_VALID_BUTTON_MASK) {
                out_session->has_failed = true;
                out_session->error_code = DIAG_REPLAY_ERR_INVALID_BUTTONS;
                snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Record %u buttons 0x%llX contains invalid bits", i,
                         (unsigned long long)records[i].buttons);
                free(records);
                fclose(f);
                return DIAG_REPLAY_ERR_INVALID_BUTTONS;
            }

            /* Valid connection attributes */
            if (records[i].attributes & ~DIAG_REPLAY_VALID_ATTR_MASK) {
                out_session->has_failed = true;
                out_session->error_code = DIAG_REPLAY_ERR_INVALID_ATTRIBUTES;
                snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Record %u attributes 0x%X contains invalid bits", i,
                         (unsigned int)records[i].attributes);
                free(records);
                fclose(f);
                return DIAG_REPLAY_ERR_INVALID_ATTRIBUTES;
            }
        }
    }

    /* Read and validate footer */
    diag_replay_footer_t footer;
    if (fread(&footer, 1, sizeof(footer), f) != sizeof(footer)) {
        free(records);
        fclose(f);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_MISSING_END_MARKER;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "%s", diag_replay_error_str(DIAG_REPLAY_ERR_MISSING_END_MARKER));
        return DIAG_REPLAY_ERR_MISSING_END_MARKER;
    }
    fclose(f);

    if (footer.eof_marker != DIAG_REPLAY_EOF_MARKER) {
        free(records);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_MISSING_END_MARKER;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Footer marker 0x%08X != expected 0x%08X", (unsigned int)footer.eof_marker, (unsigned int)DIAG_REPLAY_EOF_MARKER);
        return DIAG_REPLAY_ERR_MISSING_END_MARKER;
    }

    if (footer.record_count_check != header.record_count) {
        free(records);
        out_session->has_failed = true;
        out_session->error_code = DIAG_REPLAY_ERR_CORRUPT_RECORD;
        snprintf(out_session->error_msg, sizeof(out_session->error_msg), "Footer record count %u != header %u", footer.record_count_check, header.record_count);
        return DIAG_REPLAY_ERR_CORRUPT_RECORD;
    }

    /* Fully validated */
    for (uint32_t i = 0; i < header.record_count; i++) {
        if (records[i].anchor < 0 && records[i].poll >= header.total_polls) {
            free(records);
            out_session->has_failed = true;
            out_session->error_code = DIAG_REPLAY_ERR_CORRUPT_RECORD;
            snprintf(out_session->error_msg, sizeof(out_session->error_msg),
                     "Unanchored record %u is outside playback duration", i);
            return DIAG_REPLAY_ERR_CORRUPT_RECORD;
        }
    }
    out_session->header = header;
    out_session->records = records;
    out_session->record_count = header.record_count;
    out_session->current_pos = 0;
    out_session->is_loaded = true;
    out_session->has_failed = false;
    out_session->is_cancelled = false;
    out_session->is_eof = false;
    out_session->error_code = DIAG_REPLAY_OK;
    out_session->error_msg[0] = '\0';

    return DIAG_REPLAY_OK;
}

void diag_replay_free(diag_replay_session_t *session) {
    if (!session) return;
    if (session->records) {
        free(session->records);
        session->records = NULL;
    }
    session->record_count = 0;
    session->current_pos = 0;
    session->is_loaded = false;
}

bool diag_replay_poll(diag_replay_session_t *session,
                      uint64_t global_poll,
                      int anchor,
                      uint32_t anchor_poll,
                      uint64_t *out_buttons,
                      int16_t *out_lx,
                      int16_t *out_ly,
                      int16_t *out_rx,
                      int16_t *out_ry,
                      uint32_t *out_attributes) {
    if (session && session->is_loaded && !session->has_failed &&
        global_poll >= session->header.total_polls) session->is_eof = true;

    /* If session failed, is not loaded, or corrupt: default to safe neutral release, never live keys */
    if (!session || !session->is_loaded || session->has_failed || session->record_count == 0) {
        if (out_buttons) *out_buttons = 0;
        if (out_lx) *out_lx = 0;
        if (out_ly) *out_ly = 0;
        if (out_rx) *out_rx = 0;
        if (out_ry) *out_ry = 0;
        if (out_attributes) *out_attributes = 0;
        return false;
    }

    /* Neutral release on cancellation or explicit EOF */
    if (session->is_cancelled || session->is_eof) {
        if (out_buttons) *out_buttons = 0;
        if (out_lx) *out_lx = 0;
        if (out_ly) *out_ly = 0;
        if (out_rx) *out_rx = 0;
        if (out_ry) *out_ry = 0;
        if (out_attributes) *out_attributes = DIAG_REPLAY_ATTR_CONNECTED;
        return true;
    }

    /* Advance current position while records are due */
    while (session->current_pos < session->record_count) {
        const diag_replay_record_t *r = &session->records[session->current_pos];
        bool is_due = false;

        if (r->anchor < 0) {
            is_due = r->poll <= global_poll;
        } else if (anchor > r->anchor) {
            is_due = true;
        } else if (anchor == r->anchor && anchor_poll >= r->anchor_poll) {
            is_due = true;
        }

        if (!is_due) {
            break;
        }
        session->current_pos++;
    }

    if (session->current_pos > 0) {
        const diag_replay_record_t *active = &session->records[session->current_pos - 1];
        if (out_buttons) *out_buttons = active->buttons;
        if (out_lx) *out_lx = active->lx;
        if (out_ly) *out_ly = active->ly;
        if (out_rx) *out_rx = active->rx;
        if (out_ry) *out_ry = active->ry;
        if (out_attributes) *out_attributes = active->attributes;
        return true;
    }

    /* Neutral resting state before first event */
    if (out_buttons) *out_buttons = 0;
    if (out_lx) *out_lx = 0;
    if (out_ly) *out_ly = 0;
    if (out_rx) *out_rx = 0;
    if (out_ry) *out_ry = 0;
    if (out_attributes) *out_attributes = DIAG_REPLAY_ATTR_CONNECTED;
    return true;
}

void diag_replay_cancel(diag_replay_session_t *session) {
    if (!session) return;
    session->is_cancelled = true;
}

void diag_replay_set_eof(diag_replay_session_t *session, bool is_eof) {
    if (!session) return;
    session->is_eof = is_eof;
}

static diag_delivered_pad_state_t g_diag_last_delivered_state[8];
static bool g_diag_has_delivered_state[8];

void diag_record_delivered_pad_state(uint32_t id, const diag_delivered_pad_state_t *st) {
    if (id > 7 || !st) return;
    g_diag_last_delivered_state[id] = *st;
    g_diag_has_delivered_state[id] = true;
}

bool diag_get_last_delivered_pad_state(uint32_t id, diag_delivered_pad_state_t *out_st) {
    if (id > 7 || !g_diag_has_delivered_state[id]) return false;
    if (out_st) *out_st = g_diag_last_delivered_state[id];
    return true;
}

void diag_compose_fullkey_state(uint64_t raw_buttons,
                               int16_t lx, int16_t ly,
                               int16_t rx, int16_t ry,
                               uint32_t attributes,
                               uint64_t *out_composed_buttons,
                               uint32_t *out_attributes) {
    uint64_t composed = raw_buttons & DIAG_REPLAY_VALID_BUTTON_MASK;
    /* Derived left stick bits (16..19) */
    if (lx < -16383) composed |= (1ULL << 16); /* BTN_SL_LEFT */
    if (lx > 16383)  composed |= (1ULL << 18); /* BTN_SL_RIGHT */
    if (ly > 16383)  composed |= (1ULL << 17); /* BTN_SL_UP */
    if (ly < -16383) composed |= (1ULL << 19); /* BTN_SL_DOWN */

    /* Derived right stick bits (20..23) */
    if (rx < -16383) composed |= (1ULL << 20); /* Right Stick Left */
    if (ry > 16383)  composed |= (1ULL << 21); /* Right Stick Up */
    if (rx > 16383)  composed |= (1ULL << 22); /* Right Stick Right */
    if (ry < -16383) composed |= (1ULL << 23); /* Right Stick Down */

    if (out_composed_buttons) *out_composed_buttons = composed;
    if (out_attributes) *out_attributes = attributes & DIAG_REPLAY_VALID_ATTR_MASK;
}

diag_replay_error_t diag_replay_write(const char *path,
                                     const diag_replay_header_t *header,
                                     const diag_replay_record_t *records,
                                     size_t count) {
    if (!path || !header || (count > 0 && !records)) {
        return DIAG_REPLAY_ERR_BAD_HEADER;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        return DIAG_REPLAY_ERR_FILE_NOT_FOUND;
    }

    diag_replay_header_t h = *header;
    h.magic = DIAG_REPLAY_MAGIC;
    h.version_major = DIAG_REPLAY_VERSION_MAJOR;
    h.version_minor = DIAG_REPLAY_VERSION_MINOR;
    h.record_count = (uint32_t)count;

    if (fwrite(&h, 1, sizeof(h), f) != sizeof(h)) {
        fclose(f);
        return DIAG_REPLAY_ERR_CORRUPT_RECORD;
    }

    if (count > 0) {
        if (fwrite(records, sizeof(diag_replay_record_t), count, f) != count) {
            fclose(f);
            return DIAG_REPLAY_ERR_CORRUPT_RECORD;
        }
    }

    diag_replay_footer_t footer;
    footer.eof_marker = DIAG_REPLAY_EOF_MARKER;
    footer.record_count_check = (uint32_t)count;
    footer.checksum = 0;

    if (fwrite(&footer, 1, sizeof(footer), f) != sizeof(footer)) {
        fclose(f);
        return DIAG_REPLAY_ERR_CORRUPT_RECORD;
    }

    fclose(f);
    return DIAG_REPLAY_OK;
}

static bool g_diag_synthetic_player1_enabled = false;
static diag_replay_session_t *g_diag_active_replay_session = NULL;

void diag_input_enable_synthetic_player1(bool enable) {
    g_diag_synthetic_player1_enabled = enable;
}

bool diag_input_is_synthetic_player1_enabled(void) {
    return g_diag_synthetic_player1_enabled || (g_diag_active_replay_session && g_diag_active_replay_session->is_loaded);
}

void diag_input_set_active_replay(diag_replay_session_t *sess) {
    g_diag_active_replay_session = sess;
    if (sess && sess->is_loaded) {
        g_diag_synthetic_player1_enabled = true;
    }
}

diag_replay_session_t *diag_input_get_active_replay(void) {
    return g_diag_active_replay_session;
}

bool diag_input_is_player_connected(uint32_t id,
                                   uint32_t supported_styles,
                                   const uint32_t *supported_ids,
                                   int supported_id_count,
                                   bool has_physical_controller) {
    if (id > 7) return false;
    /* STYLE_FULLKEY bit is 1 (1u << 0) */
    if (!(supported_styles & 1u)) return false;
    if (supported_id_count >= 0 && supported_ids) {
        bool id_found = false;
        for (int i = 0; i < supported_id_count; i++) {
            if (supported_ids[i] == id) {
                id_found = true;
                break;
            }
        }
        if (!id_found) return false;
    }
    if (diag_input_is_synthetic_player1_enabled() && id == 0) {
        return true; /* Decoupled: synthetic player 1 discovered as FullKey before first query */
    }
    return has_physical_controller;
}

uint64_t diag_replay_get_confirm_button(diag_replay_confirm_policy_t policy) {
    if (policy == DIAG_REPLAY_CONFIRM_B) return DIAG_CANCEL_BIT_B;
    return DIAG_CONFIRM_BIT_A;
}

uint64_t diag_replay_get_cancel_button(diag_replay_confirm_policy_t policy) {
    if (policy == DIAG_REPLAY_CONFIRM_B) return DIAG_CONFIRM_BIT_A;
    return DIAG_CANCEL_BIT_B;
}

diag_input_policy_result_t diag_verify_input_policy(const diag_input_policy_check_t *check) {
    diag_input_policy_result_t res = {true, NULL, ""};
    if (!check) {
        res.accepted = false;
        res.reason_code = "INCOMPATIBLE_PROFILES";
        snprintf(res.details, sizeof(res.details), "Null policy check struct");
        return res;
    }

    /* Reject keyboard origin in Profile 1 */
    if (check->is_keyboard_origin) {
        res.accepted = false;
        res.reason_code = "DIFFERENT_MAPPING_POLICY";
        snprintf(res.details, sizeof(res.details), "Keyboard origin rejected in Profile 1 (synthetic FullKey required)");
        return res;
    }

    /* Reject custom button bindings in Profile 1 */
    if (check->is_custom_binding) {
        res.accepted = false;
        res.reason_code = "DIFFERENT_MAPPING_POLICY";
        snprintf(res.details, sizeof(res.details), "Custom controller bindings rejected in Profile 1 (Default Type-A required)");
        return res;
    }

    /* Reject menu swap confirm in Profile 1 */
    if (check->is_menu_swapped) {
        res.accepted = false;
        res.reason_code = "DIFFERENT_MAPPING_POLICY";
        snprintf(res.details, sizeof(res.details), "Menu B-confirm rejected in Profile 1 (Native A confirm required)");
        return res;
    }

    /* Verify source policy string */
    if (check->source_policy && strcmp(check->source_policy, "synthetic_fullkey_player1") != 0) {
        res.accepted = false;
        res.reason_code = "INCOMPATIBLE_PROFILES";
        snprintf(res.details, sizeof(res.details), "Source policy '%s' != 'synthetic_fullkey_player1'", check->source_policy);
        return res;
    }

    /* Verify mapping policy string */
    if (check->mapping_policy && strcmp(check->mapping_policy, "default_type_a") != 0) {
        res.accepted = false;
        res.reason_code = "DIFFERENT_MAPPING_POLICY";
        snprintf(res.details, sizeof(res.details), "Mapping policy '%s' != 'default_type_a'", check->mapping_policy);
        return res;
    }

    /* Verify confirm policy string */
    if (check->confirm_policy && strcmp(check->confirm_policy, "native_a") != 0) {
        res.accepted = false;
        res.reason_code = "DIFFERENT_MAPPING_POLICY";
        snprintf(res.details, sizeof(res.details), "Confirm policy '%s' != 'native_a'", check->confirm_policy);
        return res;
    }

    return res;
}
