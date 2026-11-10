use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum AudioBackendKind {
    RealDevice {
        device_name: String,
        sample_rate: u32,
        channels: u16,
    },
    ClockFallback {
        reason: String,
    },
    DisabledOrMuted,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AudioGateConfig {
    pub require_real_backend: bool,
    pub max_allowed_underruns: u64,
    pub max_allowed_starvations: u64,
    pub max_av_drift_ms: f64,
}

impl Default for AudioGateConfig {
    fn default() -> Self {
        Self {
            require_real_backend: true,
            max_allowed_underruns: 0,
            max_allowed_starvations: 0,
            max_av_drift_ms: 100.0, // 100 ms max A/V drift
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum AudioGateStatus {
    Pass,
    FailedGuestSilent {
        appended_frames: u64,
    },
    FailedRealBackendRequired {
        actual_backend: String,
    },
    FailedUnderrunsExceeded {
        underruns: u64,
        max_allowed: u64,
    },
    FailedStarvationsExceeded {
        starvations: u64,
        max_allowed: u64,
    },
    FailedAvDriftExceeded {
        drift_ms: f64,
        max_allowed_ms: f64,
    },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AudioGateResult {
    pub status: AudioGateStatus,
    pub guest_pcm_passed: bool,
    pub host_backend_passed: bool,
    pub starvation_passed: bool,
    pub av_sync_passed: bool,
    /// Physical acoustic emission is never fabricated by software assertions
    pub physical_sound: String,
    pub total_appended_frames: u64,
    pub total_released_frames: u64,
    pub underruns: u64,
    pub starvations: u64,
    pub av_drift_ms: Option<f64>,
}

/// Evaluates audio boundaries across guest PCM, host backend, starvation, and A/V sync.
pub fn evaluate_audio_gate(
    appended_frames: u64,
    released_frames: u64,
    underruns: u64,
    starvations: u64,
    backend: &AudioBackendKind,
    sample_rate: u32,
    elapsed_video_ns: Option<u64>,
    config: &AudioGateConfig,
) -> AudioGateResult {
    // 1. Boundary 1: Guest PCM output
    if appended_frames == 0 {
        return AudioGateResult {
            status: AudioGateStatus::FailedGuestSilent { appended_frames },
            guest_pcm_passed: false,
            host_backend_passed: false,
            starvation_passed: false,
            av_sync_passed: false,
            physical_sound: "not_checked".to_string(),
            total_appended_frames: appended_frames,
            total_released_frames: released_frames,
            underruns,
            starvations,
            av_drift_ms: None,
        };
    }

    // 2. Boundary 2: Real host output backend vs clock fallback
    let host_backend_passed = match backend {
        AudioBackendKind::RealDevice { .. } => true,
        AudioBackendKind::ClockFallback { reason } => {
            if config.require_real_backend {
                // Invariant: Guest PCM alone cannot pass the output/speaker gate!
                return AudioGateResult {
                    status: AudioGateStatus::FailedRealBackendRequired {
                        actual_backend: format!("ClockFallback: {}", reason),
                    },
                    guest_pcm_passed: true,
                    host_backend_passed: false,
                    starvation_passed: false,
                    av_sync_passed: false,
                    physical_sound: "not_checked".to_string(),
                    total_appended_frames: appended_frames,
                    total_released_frames: released_frames,
                    underruns,
                    starvations,
                    av_drift_ms: None,
                };
            } else {
                true
            }
        }
        AudioBackendKind::DisabledOrMuted => {
            if config.require_real_backend {
                return AudioGateResult {
                    status: AudioGateStatus::FailedRealBackendRequired {
                        actual_backend: "DisabledOrMuted".to_string(),
                    },
                    guest_pcm_passed: true,
                    host_backend_passed: false,
                    starvation_passed: false,
                    av_sync_passed: false,
                    physical_sound: "not_checked".to_string(),
                    total_appended_frames: appended_frames,
                    total_released_frames: released_frames,
                    underruns,
                    starvations,
                    av_drift_ms: None,
                };
            } else {
                true
            }
        }
    };

    // 3. Boundary 3: Underruns and Starvation
    if underruns > config.max_allowed_underruns {
        return AudioGateResult {
            status: AudioGateStatus::FailedUnderrunsExceeded {
                underruns,
                max_allowed: config.max_allowed_underruns,
            },
            guest_pcm_passed: true,
            host_backend_passed,
            starvation_passed: false,
            av_sync_passed: false,
            physical_sound: "not_checked".to_string(),
            total_appended_frames: appended_frames,
            total_released_frames: released_frames,
            underruns,
            starvations,
            av_drift_ms: None,
        };
    }

    if starvations > config.max_allowed_starvations {
        return AudioGateResult {
            status: AudioGateStatus::FailedStarvationsExceeded {
                starvations,
                max_allowed: config.max_allowed_starvations,
            },
            guest_pcm_passed: true,
            host_backend_passed,
            starvation_passed: false,
            av_sync_passed: false,
            physical_sound: "not_checked".to_string(),
            total_appended_frames: appended_frames,
            total_released_frames: released_frames,
            underruns,
            starvations,
            av_drift_ms: None,
        };
    }

    // 4. Boundary 4: A/V Sync Drift
    let mut av_drift_ms = None;
    if let Some(video_ns) = elapsed_video_ns {
        if sample_rate > 0 {
            let audio_ns = (released_frames as u128 * 1_000_000_000u128) / sample_rate as u128;
            let drift_ns = if audio_ns > video_ns as u128 {
                audio_ns - video_ns as u128
            } else {
                video_ns as u128 - audio_ns
            };
            let drift_ms_calc = drift_ns as f64 / 1_000_000.0;
            av_drift_ms = Some(drift_ms_calc);

            if drift_ms_calc > config.max_av_drift_ms {
                return AudioGateResult {
                    status: AudioGateStatus::FailedAvDriftExceeded {
                        drift_ms: drift_ms_calc,
                        max_allowed_ms: config.max_av_drift_ms,
                    },
                    guest_pcm_passed: true,
                    host_backend_passed,
                    starvation_passed: true,
                    av_sync_passed: false,
                    physical_sound: "not_checked".to_string(),
                    total_appended_frames: appended_frames,
                    total_released_frames: released_frames,
                    underruns,
                    starvations,
                    av_drift_ms,
                };
            }
        }
    }

    AudioGateResult {
        status: AudioGateStatus::Pass,
        guest_pcm_passed: true,
        host_backend_passed,
        starvation_passed: true,
        av_sync_passed: true,
        physical_sound: "not_checked".to_string(),
        total_appended_frames: appended_frames,
        total_released_frames: released_frames,
        underruns,
        starvations,
        av_drift_ms,
    }
}
