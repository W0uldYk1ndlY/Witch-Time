use crate::diag::models::{SchemaVersion, CURRENT_SCHEMA_MAJOR, MAX_DIAGNOSTIC_JSON_BYTES};
use serde::de::DeserializeOwned;
use serde_json::Value;

#[derive(Debug, PartialEq)]
pub enum DiagError {
    SizeLimitExceeded { size: usize, limit: usize },
    UnsupportedSchemaMajor { found: u32, supported: u32 },
    NonFiniteNumber(String),
    ConflictingSequence { prev: u64, current: u64, reason: &'static str },
    JsonError(String),
}

impl std::fmt::Display for DiagError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DiagError::SizeLimitExceeded { size, limit } => {
                write!(f, "diagnostic payload size {} exceeds limit {}", size, limit)
            }
            DiagError::UnsupportedSchemaMajor { found, supported } => {
                write!(
                    f,
                    "unsupported schema major version {}: only major version {} is supported",
                    found, supported
                )
            }
            DiagError::NonFiniteNumber(msg) => write!(f, "non-finite number encountered: {}", msg),
            DiagError::ConflictingSequence { prev, current, reason } => {
                write!(
                    f,
                    "conflicting sequence (prev={}, current={}): {}",
                    prev, current, reason
                )
            }
            DiagError::JsonError(msg) => write!(f, "json parse error: {}", msg),
        }
    }
}

impl std::error::Error for DiagError {}

/// Validates that a schema version has a supported major version.
pub fn validate_schema_version(version: &SchemaVersion) -> Result<(), DiagError> {
    if version.major != CURRENT_SCHEMA_MAJOR {
        return Err(DiagError::UnsupportedSchemaMajor {
            found: version.major,
            supported: CURRENT_SCHEMA_MAJOR,
        });
    }
    Ok(())
}

/// Recursively inspects a JSON Value to ensure all numbers are finite.
pub fn validate_finite_numbers(val: &Value) -> Result<(), DiagError> {
    match val {
        Value::Number(num) => {
            if let Some(f) = num.as_f64() {
                if !f.is_finite() {
                    return Err(DiagError::NonFiniteNumber(format!("{}", f)));
                }
            }
        }
        Value::Array(arr) => {
            for item in arr {
                validate_finite_numbers(item)?;
            }
        }
        Value::Object(map) => {
            for (key, item) in map {
                // Also check if someone passed a string that pretends to be NaN/Infinity in a numeric field
                if let Value::String(s) = item {
                    if s.eq_ignore_ascii_case("nan")
                        || s.eq_ignore_ascii_case("infinity")
                        || s.eq_ignore_ascii_case("-infinity")
                        || s.eq_ignore_ascii_case("+infinity")
                    {
                        return Err(DiagError::NonFiniteNumber(format!("in field '{}': {}", key, s)));
                    }
                }
                validate_finite_numbers(item)?;
            }
        }
        _ => {}
    }
    Ok(())
}

/// Parses a JSON payload with a hard size limit, finite-number enforcement, and schema validation.
pub fn parse_json_bounded<T: DeserializeOwned>(bytes: &[u8], max_bytes: usize) -> Result<T, DiagError> {
    if bytes.len() > max_bytes {
        return Err(DiagError::SizeLimitExceeded {
            size: bytes.len(),
            limit: max_bytes,
        });
    }

    let val: Value = serde_json::from_slice(bytes).map_err(|e| DiagError::JsonError(e.to_string()))?;
    validate_finite_numbers(&val)?;

    let parsed: T = serde_json::from_value(val).map_err(|e| DiagError::JsonError(e.to_string()))?;
    Ok(parsed)
}

/// Helper that uses the default maximum size budget (16 MiB).
pub fn parse_diagnostic_json<T: DeserializeOwned>(bytes: &[u8]) -> Result<T, DiagError> {
    parse_json_bounded(bytes, MAX_DIAGNOSTIC_JSON_BYTES)
}

/// Validates strict monotonically increasing sequence IDs (seq = prev + 1).
/// Returns an error on duplicates, decreasing sequences, or gaps.
pub fn validate_strict_sequence<I: IntoIterator<Item = u64>>(seqs: I) -> Result<(), DiagError> {
    let mut prev_opt: Option<u64> = None;
    for curr in seqs {
        if let Some(prev) = prev_opt {
            if curr == prev {
                return Err(DiagError::ConflictingSequence {
                    prev,
                    current: curr,
                    reason: "duplicate sequence number",
                });
            } else if curr < prev {
                return Err(DiagError::ConflictingSequence {
                    prev,
                    current: curr,
                    reason: "out-of-order sequence number",
                });
            } else if curr > prev + 1 {
                return Err(DiagError::ConflictingSequence {
                    prev,
                    current: curr,
                    reason: "sequence gap detected",
                });
            }
        }
        prev_opt = Some(curr);
    }
    Ok(())
}

/// Validates monotonically non-decreasing sequences (curr >= prev), detecting out-of-order conflicts.
pub fn validate_monotonic_sequence<I: IntoIterator<Item = u64>>(seqs: I) -> Result<(), DiagError> {
    let mut prev_opt: Option<u64> = None;
    for curr in seqs {
        if let Some(prev) = prev_opt {
            if curr < prev {
                return Err(DiagError::ConflictingSequence {
                    prev,
                    current: curr,
                    reason: "out-of-order sequence number",
                });
            }
        }
        prev_opt = Some(curr);
    }
    Ok(())
}
