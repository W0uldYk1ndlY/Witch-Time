use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct OverheadMeasurement {
    pub recorder_off_median_ms: f64,
    pub recorder_on_median_ms: f64,
    pub recorder_off_cpu_ms: f64,
    pub recorder_on_cpu_ms: f64,
    pub recorder_off_resident_bytes: u64,
    pub recorder_on_resident_bytes: u64,
    pub is_capped_fps: bool,
    pub frame_rate_cap_fps: Option<f64>,
    pub provenance: String,
    pub hardware_calibrated: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct OverheadAssessment {
    pub frame_overhead_ms: f64,
    pub frame_overhead_pct: f64,
    pub cpu_overhead_ms: f64,
    pub cpu_overhead_pct: f64,
    pub memory_overhead_bytes: u64,
    pub memory_overhead_mb: f64,
    pub is_capped_fps: bool,
    pub within_frame_budget: bool,
    pub within_memory_budget: bool,
    pub within_overall_budget: bool,
    pub hardware_status: String,
}

/// Evaluates lightweight recorder overhead against declared design budget.
/// Ensures capped-FPS runs are accompanied by genuine CPU and memory costs.
pub fn evaluate_overhead_budget(
    m: &OverheadMeasurement,
    max_frame_overhead_ms: f64,
    max_memory_overhead_bytes: u64,
) -> OverheadAssessment {
    let frame_overhead_ms = (m.recorder_on_median_ms - m.recorder_off_median_ms).max(0.0);
    let frame_overhead_pct = if m.recorder_off_median_ms > 0.0 {
        (frame_overhead_ms / m.recorder_off_median_ms) * 100.0
    } else {
        0.0
    };

    let cpu_overhead_ms = (m.recorder_on_cpu_ms - m.recorder_off_cpu_ms).max(0.0);
    let cpu_overhead_pct = if m.recorder_off_cpu_ms > 0.0 {
        (cpu_overhead_ms / m.recorder_off_cpu_ms) * 100.0
    } else {
        0.0
    };

    let memory_overhead_bytes = m
        .recorder_on_resident_bytes
        .saturating_sub(m.recorder_off_resident_bytes);
    let memory_overhead_mb = memory_overhead_bytes as f64 / (1024.0 * 1024.0);

    let within_frame_budget = frame_overhead_ms <= max_frame_overhead_ms;
    let within_memory_budget = memory_overhead_bytes <= max_memory_overhead_bytes;
    let within_overall_budget = within_frame_budget && within_memory_budget;

    let hardware_status = if m.hardware_calibrated {
        "device_calibrated".to_string()
    } else {
        "unverified_hardware_overhead".to_string()
    };

    OverheadAssessment {
        frame_overhead_ms,
        frame_overhead_pct,
        cpu_overhead_ms,
        cpu_overhead_pct,
        memory_overhead_bytes,
        memory_overhead_mb,
        is_capped_fps: m.is_capped_fps,
        within_frame_budget,
        within_memory_budget,
        within_overall_budget,
        hardware_status,
    }
}
