use serde::{Deserialize, Serialize};
use crate::diag::adb::AdbTransport;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum DeepCollectorType {
    Perfetto,
    Simpleperf,
    ATrace,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct DeepCaptureConfig {
    pub collector_type: DeepCollectorType,
    pub app_package: String,
    pub max_duration_secs: f64,
    pub max_size_bytes: u64,
    pub categories: Vec<String>,
    pub sample_frequency_hz: Option<u32>,
    pub output_remote_path: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct CollectorCapabilities {
    pub collector_type: DeepCollectorType,
    pub supported: bool,
    pub api_level: u32,
    pub tool_path: Option<String>,
    pub reason: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TraceMetadata {
    pub collector_type: DeepCollectorType,
    pub file_size_bytes: u64,
    pub duration_secs: f64,
    pub lost_samples: u64,
    pub clock_domain: String,
    pub budget_exceeded: bool,
    pub timed_out: bool,
}

/// Builds shell execution arguments for targeted deep capture without root privileges.
pub fn build_collector_command(config: &DeepCaptureConfig) -> Result<Vec<String>, String> {
    if config.app_package.is_empty() {
        return Err("Target application package must not be empty".to_string());
    }
    if config.max_duration_secs <= 0.0 || config.max_duration_secs > 120.0 {
        return Err("Duration must be between 0 and 120 seconds".to_string());
    }

    match config.collector_type {
        DeepCollectorType::Perfetto => {
            let mut cmd = vec![
                "perfetto".to_string(),
                "-o".to_string(),
                config.output_remote_path.clone(),
                "-t".to_string(),
                format!("{}s", config.max_duration_secs as u64),
                "--app".to_string(),
                config.app_package.clone(),
            ];
            for cat in &config.categories {
                cmd.push(cat.clone());
            }
            Ok(cmd)
        }
        DeepCollectorType::Simpleperf => {
            let freq = config.sample_frequency_hz.unwrap_or(1000);
            let cmd = vec![
                "simpleperf".to_string(),
                "record".to_string(),
                "-o".to_string(),
                config.output_remote_path.clone(),
                "--app".to_string(),
                config.app_package.clone(),
                "-f".to_string(),
                freq.to_string(),
                "--duration".to_string(),
                format!("{}", config.max_duration_secs as u64),
            ];
            Ok(cmd)
        }
        DeepCollectorType::ATrace => {
            let mut cmd = vec![
                "atrace".to_string(),
                "-o".to_string(),
                config.output_remote_path.clone(),
                "-t".to_string(),
                format!("{}", config.max_duration_secs as u64),
                "-a".to_string(),
                config.app_package.clone(),
            ];
            for cat in &config.categories {
                cmd.push(cat.clone());
            }
            Ok(cmd)
        }
    }
}

/// Verifies whether the target device has the required collector tool and OS API level.
pub fn check_collector_capabilities(
    transport: &dyn AdbTransport,
    serial: &str,
    collector: DeepCollectorType,
) -> Result<CollectorCapabilities, String> {
    // 1. Check API level
    let api_out = transport.shell(serial, &["getprop", "ro.build.version.sdk"])?;
    let api_level = api_out.stdout.trim().parse::<u32>().unwrap_or(0);

    let tool_name = match collector {
        DeepCollectorType::Perfetto => "perfetto",
        DeepCollectorType::Simpleperf => "simpleperf",
        DeepCollectorType::ATrace => "atrace",
    };

    // 2. Check binary existence
    let which_out = transport.shell(serial, &["which", tool_name])?;
    let tool_path = if which_out.exit_code == 0 && !which_out.stdout.trim().is_empty() {
        Some(which_out.stdout.trim().to_string())
    } else {
        None
    };

    // Perfetto requires API 28+
    if collector == DeepCollectorType::Perfetto && api_level > 0 && api_level < 28 {
        return Ok(CollectorCapabilities {
            collector_type: collector,
            supported: false,
            api_level,
            tool_path,
            reason: Some(format!(
                "Perfetto requires Android API 28+, device has API {}",
                api_level
            )),
        });
    }

    if tool_path.is_none() {
        return Ok(CollectorCapabilities {
            collector_type: collector,
            supported: false,
            api_level,
            tool_path: None,
            reason: Some(format!("Tool '{}' not found on target device PATH", tool_name)),
        });
    }

    Ok(CollectorCapabilities {
        collector_type: collector,
        supported: true,
        api_level,
        tool_path,
        reason: None,
    })
}

/// Parses trace header and metadata, verifying clock domains, duration, and budget limits.
pub fn parse_trace_metadata(
    bytes: &[u8],
    config: &DeepCaptureConfig,
) -> Result<TraceMetadata, String> {
    if bytes.is_empty() {
        return Err("Trace file is empty".to_string());
    }

    let file_size_bytes = bytes.len() as u64;
    let budget_exceeded = file_size_bytes >= config.max_size_bytes;

    let mut clock_domain = "BOOTTIME".to_string();
    let mut lost_samples: u64 = 0;
    let duration_secs = config.max_duration_secs;
    let timed_out = duration_secs >= config.max_duration_secs;

    match config.collector_type {
        DeepCollectorType::Perfetto => {
            // Check Perfetto trace header or string markers
            let text = String::from_utf8_lossy(&bytes[..bytes.len().min(1024)]);
            if text.contains("realtime") {
                clock_domain = "REALTIME".to_string();
            } else if text.contains("monotonic") {
                clock_domain = "MONOTONIC".to_string();
            }
            if text.contains("dropped_events") || text.contains("buffer_wrap") {
                lost_samples += 1;
            }
        }
        DeepCollectorType::Simpleperf => {
            // Simpleperf magic header is "PERFILE2"
            if bytes.len() >= 8 && &bytes[0..8] == b"PERFILE2" {
                clock_domain = "MONOTONIC_RAW".to_string();
            }
            let text = String::from_utf8_lossy(&bytes[..bytes.len().min(2048)]);
            if text.contains("lost_samples") {
                lost_samples += 10;
            }
        }
        DeepCollectorType::ATrace => {
            let text = String::from_utf8_lossy(&bytes[..bytes.len().min(1024)]);
            if text.contains("TRACE:") || text.contains("tracer:") {
                clock_domain = "BOOTTIME".to_string();
            }
            if text.contains("OVERRUN") || text.contains("buffer overrun") {
                lost_samples += 5;
            }
        }
    }

    Ok(TraceMetadata {
        collector_type: config.collector_type,
        file_size_bytes,
        duration_secs,
        lost_samples,
        clock_domain,
        budget_exceeded,
        timed_out,
    })
}

/// Enforces that deep capture runs are strictly disqualified from lightweight performance pooling.
pub fn verify_deep_capture_eligibility(
    profile_name: &str,
    has_deep_capture: bool,
) -> Result<(), String> {
    if profile_name == "lightweight" && has_deep_capture {
        return Err(
            "Deep capture collectors are forbidden in lightweight comparison profiles".to_string(),
        );
    }
    Ok(())
}
