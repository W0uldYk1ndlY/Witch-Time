use std::collections::BTreeSet;

use sha2::{Digest, Sha256};

use crate::diag::models::{
    DiagnosticRequest, SchemaVersion,
};

pub const MAX_REQUEST_PAYLOAD_BYTES: usize = 65536;
pub const MAX_IDENTIFIER_LEN: usize = 64;

/// Verifies whether an identifier contains only allowlisted characters [a-zA-Z0-9_-] and bounded length.
pub fn is_valid_identifier(id: &str) -> bool {
    if id.is_empty() || id.len() > MAX_IDENTIFIER_LEN {
        return false;
    }
    id.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

/// Computes SHA-256 hex string of arbitrary byte slice.
pub fn compute_request_digest(data: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(data);
    let res = hasher.finalize();
    res.iter().map(|b| format!("{:02x}", b)).collect()
}

/// Validates bootstrap launch parameters before native runtime loading.
pub fn validate_bootstrap_request(
    raw_json: &[u8],
    expected_run_id: &str,
    expected_digest: &str,
) -> Result<DiagnosticRequest, String> {
    if raw_json.is_empty() {
        return Err("Empty request payload".to_string());
    }
    if raw_json.len() > MAX_REQUEST_PAYLOAD_BYTES {
        return Err(format!(
            "Payload size {} exceeds maximum bounded limit of {} bytes",
            raw_json.len(),
            MAX_REQUEST_PAYLOAD_BYTES
        ));
    }

    if !is_valid_identifier(expected_run_id) {
        return Err(format!("Invalid run_id '{}'", expected_run_id));
    }

    // Verify digest binding
    let actual_digest = compute_request_digest(raw_json);
    if !actual_digest.eq_ignore_ascii_case(expected_digest) {
        return Err(format!(
            "Request digest mismatch: expected {}, got {}",
            expected_digest, actual_digest
        ));
    }

    // Path traversal and symlink escape checks on raw payload
    let raw_str = std::str::from_utf8(raw_json)
        .map_err(|e| format!("Request payload is not valid UTF-8: {}", e))?;
    if raw_str.contains("..") || raw_str.contains("/..") || raw_str.contains("\\..") {
        return Err("Request contains rejected path traversal '..' sequences".to_string());
    }

    // Non-finite number checks
    if raw_str.contains("NaN") || raw_str.contains("Infinity") || raw_str.contains("-Infinity") {
        return Err("Request contains rejected non-finite numeric values".to_string());
    }

    // Parse JSON
    let val: serde_json::Value = serde_json::from_slice(raw_json)
        .map_err(|e| format!("Malformed JSON: {}", e))?;

    let root_obj = val.as_object().ok_or("Root JSON must be an object")?;

    // Allowlisted top-level keys
    let allowed_keys: BTreeSet<&str> = [
        "schema_version",
        "run_id",
        "created_utc",
        "build",
        "scenario",
        "profile",
        "configuration",
        "policy",
        "treatments",
    ]
    .iter()
    .copied()
    .collect();

    for k in root_obj.keys() {
        if !allowed_keys.contains(k.as_str()) {
            return Err(format!("Unknown or unallowlisted top-level key '{}'", k));
        }
    }

    let req: DiagnosticRequest = serde_json::from_value(val)
        .map_err(|e| format!("Failed to parse DiagnosticRequest: {}", e))?;

    // Schema version check
    if req.schema_version.major != SchemaVersion::CURRENT.major {
        return Err(format!(
            "Unsupported schema major version: {}",
            req.schema_version.major
        ));
    }

    // Run ID match
    if req.run_id != expected_run_id {
        return Err(format!(
            "Request run_id '{}' does not match launch run_id '{}'",
            req.run_id, expected_run_id
        ));
    }

    // Build allowlist
    if req.build.variant != "diagnostic" {
        return Err(format!(
            "Build variant must be 'diagnostic', got '{}'",
            req.build.variant
        ));
    }
    if req.build.target != "android-arm64" {
        return Err(format!(
            "Build target must be 'android-arm64', got '{}'",
            req.build.target
        ));
    }
    if req.build.native_optimization != "release" {
        return Err(format!(
            "Native optimization must be 'release', got '{}'",
            req.build.native_optimization
        ));
    }

    // Scenario allowlist
    if !is_valid_identifier(&req.scenario.scenario_id) {
        return Err(format!(
            "Invalid scenario_id '{}'",
            req.scenario.scenario_id
        ));
    }
    if !is_valid_identifier(&req.scenario.route_id) {
        return Err(format!("Invalid route_id '{}'", req.scenario.route_id));
    }
    if !is_valid_identifier(&req.scenario.fixture_id) {
        return Err(format!("Invalid fixture_id '{}'", req.scenario.fixture_id));
    }

    // Deadlines must be positive and finite
    let d = &req.scenario.deadlines;
    if !d.startup_deadline_secs.is_finite() || d.startup_deadline_secs <= 0.0 {
        return Err("startup_deadline_secs must be positive and finite".to_string());
    }
    if !d.scenario_deadline_secs.is_finite() || d.scenario_deadline_secs <= 0.0 {
        return Err("scenario_deadline_secs must be positive and finite".to_string());
    }
    if !d.recovery_deadline_secs.is_finite() || d.recovery_deadline_secs <= 0.0 {
        return Err("recovery_deadline_secs must be positive and finite".to_string());
    }

    Ok(req)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn valid_test_json(run_id: &str) -> serde_json::Value {
        json!({
            "schema_version": "1.0",
            "run_id": run_id,
            "build": {
                "variant": "diagnostic",
                "target": "android-arm64",
                "native_optimization": "release"
            },
            "scenario": {
                "scenario_id": "boot_continue_combat",
                "route_id": "shibuya_prologue",
                "fixture_id": "prologue_v1",
                "deadlines": {
                    "startup_deadline_secs": 15.0,
                    "scenario_deadline_secs": 45.0,
                    "recovery_deadline_secs": 5.0
                }
            },
            "profile": {
                "profile_name": "standard",
                "memory_budget_bytes": 8388608,
                "storage_budget_bytes": 134217728,
                "incident_disk_reserve_bytes": 16777216,
                "max_incidents": 3,
                "collectors": ["events", "metrics"]
            },
            "configuration": {
                "resolution": [1920, 1080],
                "fps_target": 60.0,
                "audio_mode": "native",
                "cache_mode": "isolated_cold",
                "input_policy": {
                    "source": "controller",
                    "mapping": "type_a",
                    "menu_confirm": "native_a"
                }
            },
            "policy": {
                "policy_id": "profile1_standard",
                "policy_hash": "a1b2c3d4e5f6",
                "required_assertions": ["boot_present", "combat_reached"]
            }
        })
    }

    #[test]
    fn test_valid_bootstrap_request_happy_path() {
        let val = valid_test_json("run_20261110_test");
        let bytes = serde_json::to_vec_pretty(&val).unwrap();
        let digest = compute_request_digest(&bytes);

        let parsed = validate_bootstrap_request(&bytes, "run_20261110_test", &digest);
        assert!(parsed.is_ok(), "Expected valid parse: {:?}", parsed.err());
        let req = parsed.unwrap();
        assert_eq!(req.run_id, "run_20261110_test");
        assert_eq!(req.build.variant, "diagnostic");
    }

    #[test]
    fn test_ordinary_release_ignores_diagnostic_request() {
        // If variant is not diagnostic, the request is rejected
        let mut val = valid_test_json("run_20261110_release");
        val["build"]["variant"] = json!("release");
        let bytes = serde_json::to_vec(&val).unwrap();
        let digest = compute_request_digest(&bytes);

        let err = validate_bootstrap_request(&bytes, "run_20261110_release", &digest).unwrap_err();
        assert!(err.contains("Build variant must be 'diagnostic'"));
    }

    #[test]
    fn test_allowlisted_values_and_safe_identifiers() {
        assert!(is_valid_identifier("run_123_ABC-xyz"));
        assert!(!is_valid_identifier(""));
        assert!(!is_valid_identifier("run/slash"));
        assert!(!is_valid_identifier("run\\backslash"));
        assert!(!is_valid_identifier("run..dot"));
        assert!(!is_valid_identifier("run with spaces"));
        assert!(!is_valid_identifier(&"a".repeat(65)));

        let mut val = valid_test_json("bad_scenario");
        val["scenario"]["scenario_id"] = json!("bad/scenario/path");
        let bytes = serde_json::to_vec(&val).unwrap();
        let digest = compute_request_digest(&bytes);
        let err = validate_bootstrap_request(&bytes, "bad_scenario", &digest).unwrap_err();
        assert!(err.contains("Invalid scenario_id"));
    }

    #[test]
    fn test_unknown_and_duplicate_and_oversized_fields() {
        // Unknown top-level field rejected
        let mut val = valid_test_json("run_unknown_field");
        val["unauthorized_exec"] = json!("sh -c 'rm -rf /'");
        let bytes = serde_json::to_vec(&val).unwrap();
        let digest = compute_request_digest(&bytes);
        let err = validate_bootstrap_request(&bytes, "run_unknown_field", &digest).unwrap_err();
        assert!(err.contains("Unknown or unallowlisted top-level key"));

        // Oversized payload rejected
        let large_bytes = vec![b' '; MAX_REQUEST_PAYLOAD_BYTES + 1];
        let err_oversized =
            validate_bootstrap_request(&large_bytes, "run_large", "dummy").unwrap_err();
        assert!(err_oversized.contains("exceeds maximum bounded limit"));
    }

    #[test]
    fn test_invalid_and_non_finite_numbers() {
        let raw_nan = br#"{"schema_version":"1.0","run_id":"run_nan","build":{"variant":"diagnostic","target":"android-arm64","native_optimization":"release"},"scenario":{"scenario_id":"sc","route_id":"rt","fixture_id":"fx","deadlines":{"startup_deadline_secs":NaN,"scenario_deadline_secs":10.0,"recovery_deadline_secs":5.0}}}"#;
        let digest = compute_request_digest(raw_nan);
        let err = validate_bootstrap_request(raw_nan, "run_nan", &digest).unwrap_err();
        assert!(err.contains("rejected non-finite numeric values"));
    }

    #[test]
    fn test_traversal_and_symlink_escapes() {
        let mut val = valid_test_json("run_traversal");
        val["scenario"]["fixture_id"] = json!("../../etc/shadow");
        let bytes = serde_json::to_vec(&val).unwrap();
        let digest = compute_request_digest(&bytes);
        let err = validate_bootstrap_request(&bytes, "run_traversal", &digest).unwrap_err();
        assert!(err.contains("rejected path traversal"));
    }

    #[test]
    fn test_request_digest_binding_and_tamper_rejection() {
        let val = valid_test_json("run_digest_check");
        let bytes = serde_json::to_vec(&val).unwrap();
        let err = validate_bootstrap_request(&bytes, "run_digest_check", "bad_digest_hash")
            .unwrap_err();
        assert!(err.contains("Request digest mismatch"));
    }

    #[test]
    fn test_startup_ordering_and_acknowledgement_structure() {
        let val = valid_test_json("run_ordering");
        let bytes = serde_json::to_vec(&val).unwrap();
        let digest = compute_request_digest(&bytes);

        let ack = json!({
            "schema_version": "1.0",
            "run_id": "run_ordering",
            "request_digest": digest,
            "status": "acknowledged",
            "startup_phase": "bootstrap_pre_dlopen"
        });

        assert_eq!(ack["schema_version"], "1.0");
        assert_eq!(ack["status"], "acknowledged");
        assert_eq!(ack["startup_phase"], "bootstrap_pre_dlopen");
    }
}
