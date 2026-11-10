use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ThermalEligibilityCheck {
    pub baseline_start_headroom: Option<f32>,
    pub candidate_start_headroom: Option<f32>,
    pub max_headroom_delta: f32,
    pub is_eligible: bool,
    pub reason: Option<String>,
}

pub fn check_thermal_eligibility(
    baseline_start: Option<f32>,
    candidate_start: Option<f32>,
    max_delta: f32,
) -> ThermalEligibilityCheck {
    match (baseline_start, candidate_start) {
        (Some(b), Some(c)) => {
            let delta = (b - c).abs();
            if delta > max_delta {
                ThermalEligibilityCheck {
                    baseline_start_headroom: Some(b),
                    candidate_start_headroom: Some(c),
                    max_headroom_delta: max_delta,
                    is_eligible: false,
                    reason: Some(format!(
                        "Starting thermal headroom mismatch: delta {:.3} > max allowed {:.3}",
                        delta, max_delta
                    )),
                }
            } else {
                ThermalEligibilityCheck {
                    baseline_start_headroom: Some(b),
                    candidate_start_headroom: Some(c),
                    max_headroom_delta: max_delta,
                    is_eligible: true,
                    reason: None,
                }
            }
        }
        _ => {
            // Unsupported thermal data on older APIs / emulators
            ThermalEligibilityCheck {
                baseline_start_headroom: baseline_start,
                candidate_start_headroom: candidate_start,
                max_headroom_delta: max_delta,
                is_eligible: true,
                reason: Some("Thermal headroom unsupported on device; power/thermal conclusions unverified".to_string()),
            }
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TrajectoryWindow {
    pub window_index: usize,
    pub start_sec: f64,
    pub end_sec: f64,
    pub median_fps: f64,
    pub p99_frame_time_ms: f64,
    pub mean_thermal_headroom: Option<f32>,
    pub is_excluded: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SustainedPlayTrajectory {
    pub total_duration_secs: f64,
    pub windows: Vec<TrajectoryWindow>,
    pub initial_steady_fps: f64,
    pub final_steady_fps: f64,
    pub time_to_degradation_secs: Option<f64>,
    pub throttling_observed: bool,
    pub min_thermal_headroom: Option<f32>,
    pub excluded_intervals_count: usize,
}

pub fn compute_sustained_trajectory(
    windows: &[TrajectoryWindow],
    degradation_threshold_pct: f64,
) -> SustainedPlayTrajectory {
    let excluded_count = windows.iter().filter(|w| w.is_excluded).count();
    let valid_windows: Vec<&TrajectoryWindow> = windows.iter().filter(|w| !w.is_excluded).collect();

    if valid_windows.is_empty() {
        return SustainedPlayTrajectory {
            total_duration_secs: 0.0,
            windows: windows.to_vec(),
            initial_steady_fps: 0.0,
            final_steady_fps: 0.0,
            time_to_degradation_secs: None,
            throttling_observed: false,
            min_thermal_headroom: None,
            excluded_intervals_count: excluded_count,
        };
    }

    let total_duration_secs = windows.last().map(|w| w.end_sec).unwrap_or(0.0);
    let initial_steady_fps = valid_windows[0].median_fps;
    let final_steady_fps = valid_windows.last().unwrap().median_fps;

    let mut time_to_degradation = None;
    let mut min_headroom: Option<f32> = None;

    for w in &valid_windows {
        if let Some(hr) = w.mean_thermal_headroom {
            min_headroom = Some(min_headroom.map_or(hr, |m| m.min(hr)));
        }

        if time_to_degradation.is_none() && initial_steady_fps > 0.0 {
            let drop_pct = ((initial_steady_fps - w.median_fps) / initial_steady_fps) * 100.0;
            if drop_pct >= degradation_threshold_pct {
                time_to_degradation = Some(w.start_sec);
            }
        }
    }

    let throttling_observed = time_to_degradation.is_some()
        || min_headroom.map_or(false, |hr| hr < 0.10);

    SustainedPlayTrajectory {
        total_duration_secs,
        windows: windows.to_vec(),
        initial_steady_fps,
        final_steady_fps,
        time_to_degradation_secs: time_to_degradation,
        throttling_observed,
        min_thermal_headroom: min_headroom,
        excluded_intervals_count: excluded_count,
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum TrajectoryComparisonVerdict {
    CandidateCausedThrottling {
        degradation_sec: f64,
        late_fps_drop_pct: f64,
    },
    CandidateLateDegradation {
        late_fps_drop_pct: f64,
    },
    ComparableSustainedPlay,
    CandidateImprovedSustainedPlay,
}

pub fn compare_thermal_trajectories(
    baseline: &SustainedPlayTrajectory,
    candidate: &SustainedPlayTrajectory,
) -> TrajectoryComparisonVerdict {
    // If baseline maintained steady FPS, but candidate suffered throttling degradation:
    if !baseline.throttling_observed && candidate.throttling_observed {
        let deg_sec = candidate.time_to_degradation_secs.unwrap_or(0.0);
        let drop_pct = if candidate.initial_steady_fps > 0.0 {
            ((candidate.initial_steady_fps - candidate.final_steady_fps) / candidate.initial_steady_fps) * 100.0
        } else {
            0.0
        };
        return TrajectoryComparisonVerdict::CandidateCausedThrottling {
            degradation_sec: deg_sec,
            late_fps_drop_pct: drop_pct,
        };
    }

    if candidate.final_steady_fps < baseline.final_steady_fps * 0.95 {
        let drop_pct = ((baseline.final_steady_fps - candidate.final_steady_fps) / baseline.final_steady_fps) * 100.0;
        return TrajectoryComparisonVerdict::CandidateLateDegradation {
            late_fps_drop_pct: drop_pct,
        };
    }

    if candidate.final_steady_fps > baseline.final_steady_fps * 1.03 {
        return TrajectoryComparisonVerdict::CandidateImprovedSustainedPlay;
    }

    TrajectoryComparisonVerdict::ComparableSustainedPlay
}
