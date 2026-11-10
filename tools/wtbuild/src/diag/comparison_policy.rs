use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PairedRunPolicy {
    pub policy_id: String,
    pub policy_hash: String,
    pub min_pairs: usize,
    pub max_pairs: usize,
    pub practical_margin_pct: f64,
    pub noise_margin_pct: f64,
    pub equivalence_threshold_pct: f64,
    pub confidence_level: f64,
    pub fixed_seed: u64,
    pub is_frozen: bool,
}

impl Default for PairedRunPolicy {
    fn default() -> Self {
        Self {
            policy_id: "paired_policy_v1".to_string(),
            policy_hash: "sha256_predeclared_v1".to_string(),
            min_pairs: 3,
            max_pairs: 5,
            practical_margin_pct: 2.0,    // 2.0% practical significance threshold
            noise_margin_pct: 1.0,        // 1.0% noise ceiling
            equivalence_threshold_pct: 1.5, // +/- 1.5% is considered equivalent
            confidence_level: 0.95,
            fixed_seed: 0x5EEDF1A6E,
            is_frozen: true,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PairedRunObservation {
    pub pair_index: usize,
    pub baseline_run_id: String,
    pub candidate_run_id: String,
    pub baseline_median_ms: f64,
    pub candidate_median_ms: f64,
    pub baseline_p99_ms: f64,
    pub candidate_p99_ms: f64,
    pub baseline_gpu_ms: Option<f64>,
    pub candidate_gpu_ms: Option<f64>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum PairedClassification {
    Improvement {
        median_gain_pct: f64,
        tail_gain_pct: f64,
    },
    Regression {
        median_loss_pct: f64,
        tail_loss_pct: f64,
    },
    Equivalence {
        mean_delta_pct: f64,
    },
    TradeOff {
        median_delta_pct: f64,
        tail_delta_pct: f64,
    },
    InsufficientPairs {
        actual_pairs: usize,
        min_required: usize,
    },
    PolicyNotFrozen {
        policy_id: String,
    },
    MissingRequiredMetric {
        metric: String,
        details: String,
    },
    InconclusiveNoise {
        mean_delta_pct: f64,
        noise_margin_pct: f64,
    },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PairedComparisonResult {
    pub policy_id: String,
    pub policy_frozen: bool,
    pub pair_count: usize,
    pub classification: PairedClassification,
    pub mean_pct_median_change: f64,
    pub mean_pct_tail_change: f64,
    pub median_std_error: f64,
    pub tail_std_error: f64,
    pub is_provisional_screening: bool,
}

/// Evaluates paired baseline/candidate runs with run-level uncertainty.
/// Prevents frame-level significance peeking from inflating confidence.
pub fn evaluate_paired_runs(
    policy: &PairedRunPolicy,
    pairs: &[PairedRunObservation],
    is_provisional_screening: bool,
) -> PairedComparisonResult {
    // 1. Policy must be frozen before candidate testing
    if !policy.is_frozen {
        return PairedComparisonResult {
            policy_id: policy.policy_id.clone(),
            policy_frozen: false,
            pair_count: pairs.len(),
            classification: PairedClassification::PolicyNotFrozen {
                policy_id: policy.policy_id.clone(),
            },
            mean_pct_median_change: 0.0,
            mean_pct_tail_change: 0.0,
            median_std_error: 0.0,
            tail_std_error: 0.0,
            is_provisional_screening,
        };
    }

    // 2. Bounded repetition and stopping rules: Insufficient pairs
    if pairs.len() < policy.min_pairs {
        return PairedComparisonResult {
            policy_id: policy.policy_id.clone(),
            policy_frozen: true,
            pair_count: pairs.len(),
            classification: PairedClassification::InsufficientPairs {
                actual_pairs: pairs.len(),
                min_required: policy.min_pairs,
            },
            mean_pct_median_change: 0.0,
            mean_pct_tail_change: 0.0,
            median_std_error: 0.0,
            tail_std_error: 0.0,
            is_provisional_screening,
        };
    }

    // 3. Metric presence check
    for p in pairs {
        if p.baseline_gpu_ms.is_some() && p.candidate_gpu_ms.is_none() {
            return PairedComparisonResult {
                policy_id: policy.policy_id.clone(),
                policy_frozen: true,
                pair_count: pairs.len(),
                classification: PairedClassification::MissingRequiredMetric {
                    metric: "gpu_time_ms".to_string(),
                    details: format!("Candidate in pair {} is missing required GPU measurements", p.pair_index),
                },
                mean_pct_median_change: 0.0,
                mean_pct_tail_change: 0.0,
                median_std_error: 0.0,
                tail_std_error: 0.0,
                is_provisional_screening,
            };
        }
    }

    // 4. Run-level paired differences (NOT frame-level peeking)
    let n = pairs.len();
    let mut median_deltas_pct = Vec::with_capacity(n);
    let mut tail_deltas_pct = Vec::with_capacity(n);

    for p in pairs {
        let med_pct = ((p.candidate_median_ms - p.baseline_median_ms) / p.baseline_median_ms) * 100.0;
        let tail_pct = ((p.candidate_p99_ms - p.baseline_p99_ms) / p.baseline_p99_ms) * 100.0;
        median_deltas_pct.push(med_pct);
        tail_deltas_pct.push(tail_pct);
    }

    let mean_pct_median = median_deltas_pct.iter().sum::<f64>() / n as f64;
    let mean_pct_tail = tail_deltas_pct.iter().sum::<f64>() / n as f64;

    let var_median = median_deltas_pct
        .iter()
        .map(|x| (x - mean_pct_median).powi(2))
        .sum::<f64>()
        / (n - 1).max(1) as f64;
    let median_std_error = (var_median / n as f64).sqrt();

    let var_tail = tail_deltas_pct
        .iter()
        .map(|x| (x - mean_pct_tail).powi(2))
        .sum::<f64>()
        / (n - 1).max(1) as f64;
    let tail_std_error = (var_tail / n as f64).sqrt();

    // 5. Classification
    let classification = if mean_pct_median < -policy.practical_margin_pct
        && mean_pct_tail > policy.practical_margin_pct
    {
        // Conflicting median and tail movement
        PairedClassification::TradeOff {
            median_delta_pct: mean_pct_median,
            tail_delta_pct: mean_pct_tail,
        }
    } else if mean_pct_median < -policy.practical_margin_pct
        && mean_pct_tail <= policy.practical_margin_pct
    {
        // Improvement: median improved and tail did not regress
        PairedClassification::Improvement {
            median_gain_pct: -mean_pct_median,
            tail_gain_pct: -mean_pct_tail,
        }
    } else if mean_pct_median > policy.practical_margin_pct
        || mean_pct_tail > policy.practical_margin_pct
    {
        // Regression: median or tail regressed beyond practical margin
        PairedClassification::Regression {
            median_loss_pct: mean_pct_median,
            tail_loss_pct: mean_pct_tail,
        }
    } else if mean_pct_median.abs() <= policy.equivalence_threshold_pct
        && mean_pct_tail.abs() <= policy.equivalence_threshold_pct
    {
        // Equivalence: both median and tail within equivalence bound
        PairedClassification::Equivalence {
            mean_delta_pct: mean_pct_median,
        }
    } else {
        PairedClassification::InconclusiveNoise {
            mean_delta_pct: mean_pct_median,
            noise_margin_pct: policy.noise_margin_pct,
        }
    };

    PairedComparisonResult {
        policy_id: policy.policy_id.clone(),
        policy_frozen: true,
        pair_count: n,
        classification,
        mean_pct_median_change: mean_pct_median,
        mean_pct_tail_change: mean_pct_tail,
        median_std_error,
        tail_std_error,
        is_provisional_screening,
    }
}
