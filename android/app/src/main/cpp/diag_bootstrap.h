#ifndef DIAG_BOOTSTRAP_H
#define DIAG_BOOTSTRAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_BOOTSTRAP_MAX_RUN_ID_LEN 64
#define DIAG_BOOTSTRAP_SHA256_HEX_LEN 64
#define DIAG_BOOTSTRAP_MAX_REQUEST_BYTES 65536

#define DIAG_BOOTSTRAP_OK 0
#define DIAG_BOOTSTRAP_ERR_INVALID_ARGS -1
#define DIAG_BOOTSTRAP_ERR_NOT_DIAGNOSTIC -2
#define DIAG_BOOTSTRAP_ERR_DIGEST_MISMATCH -3
#define DIAG_BOOTSTRAP_ERR_OVERSIZED -4
#define DIAG_BOOTSTRAP_ERR_TRAVERSAL -5
#define DIAG_BOOTSTRAP_ERR_INVALID_JSON -6
#define DIAG_BOOTSTRAP_ERR_UNSUPPORTED_SCHEMA -7
#define DIAG_BOOTSTRAP_ERR_IO -8

/* Returns 1 if WT_DIAGNOSTIC_ENABLED is defined, 0 otherwise */
int diag_bootstrap_is_diagnostic_variant(void);
/* Typed flag, only for a successfully validated diagnostic request; never exported as an environment variable. */
int diag_bootstrap_temporal_source_requested(void);
int diag_bootstrap_save_export_requested(void);
/* Same validator for a descriptor-read request; no pathname reopen by export-only callers. */
int diag_bootstrap_validate_request_data(const uint8_t *data, size_t size, const char *run_id,
                                         const char *digest, char *error, size_t error_cap);

/* Validates run_id: alphanumeric, '_', '-', length 1..64. No '..', '/', '\' */
int diag_bootstrap_validate_run_id(const char *run_id);

/* Computes SHA-256 hex string (64 chars + null terminator). Returns 0 on success, -1 on failure. */
int diag_bootstrap_compute_sha256(const uint8_t *data, size_t len, char *out_hex);

/* Parses command-line arguments for diagnostic flags:
 * Returns:
 *   1 if diagnostic flags found and valid format
 *   0 if diagnostic flags not present
 *  -1 if malformed arguments or invalid run_id/digest format
 */
int diag_bootstrap_parse_args(int argc, char **argv, char *out_run_id, size_t run_id_cap, char *out_digest, size_t digest_cap);

/* Validates request.json located at <run_dir>/request.json:
 * - Checks file size <= 64KB
 * - Computes SHA-256 and compares to expected_digest
 * - Validates schema_version major is 1
 * - Validates run_id matches
 * - Validates allowlisted build variant ("diagnostic"), target ("android-arm64"), native_optimization ("release")
 * - Validates that paths do not contain traversal ("..")
 * Returns DIAG_BOOTSTRAP_OK on success, negative error code on failure.
 */
int diag_bootstrap_validate_request(const char *run_dir, const char *run_id, const char *expected_digest, char *out_error_reason, size_t error_cap);

/* Writes startup acknowledgement to <run_dir>/manifest.json before dlopen:
 * Returns DIAG_BOOTSTRAP_OK on success, -1 on failure.
 */
int diag_bootstrap_write_acknowledgement(const char *run_dir, const char *run_id, const char *digest);

/* Writes rejection outcome to <run_dir>/outcome.json:
 * Returns DIAG_BOOTSTRAP_OK on success, -1 on failure.
 */
int diag_bootstrap_write_outcome_rejection(const char *run_dir, const char *run_id, const char *reason, const char *details);

/* Creates session-owned directories and sets environment variables:
 * Returns DIAG_BOOTSTRAP_OK on success, -1 on failure.
 */
int diag_bootstrap_apply_session_environment(const char *run_dir, const char *run_id);

#ifdef __cplusplus
}
#endif

#endif /* DIAG_BOOTSTRAP_H */
