use crate::diag::eligibility::evaluate_comparison_eligibility;
use crate::diag::models::*;
use std::collections::BTreeMap;

pub struct FrameSample {
    pub frame_time_ms: f64,
    pub gpu_time_ms: Option<f64>,
    pub is_held: bool,
}

pub fn compute_timing_stats(values: &[f64]) -> Option<TimingStats> {
    if values.is_empty() {
        return None;
    }
    let mut sorted = values.to_vec();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    let n = sorted.len();
    let min = sorted[0];
    let max = sorted[n - 1];
    let pct = |q: f64| -> f64 {
        sorted[(n - 1).min((q / 100.0 * n as f64) as usize)]
    };
    let median = pct(50.0);
    let p95 = pct(95.0);
    let p99 = pct(99.0);
    let sum: f64 = sorted.iter().sum();
    let mean = sum / n as f64;
    let var: f64 = sorted.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / n as f64;
    let stddev = var.sqrt();
    Some(TimingStats {
        min,
        max,
        median,
        p95,
        p99,
        mean,
        stddev,
    })
}

/// Computes deterministic per-segment statistics with phase-specific budgets (30 vs 60 fps)
/// and distinct held vs progress presents.
pub fn compute_segment_statistics(
    segment_id: &str,
    target_fps: f64,
    samples: &[FrameSample],
    expected_sample_count: u64,
) -> SegmentStatistics {
    let sample_count = samples.len() as u64;
    let coverage = if expected_sample_count > 0 {
        (sample_count as f64 / expected_sample_count as f64).min(1.0)
    } else if sample_count > 0 {
        1.0
    } else {
        0.0
    };

    let duration_ms: f64 = samples.iter().map(|s| s.frame_time_ms).sum();
    let frame_times: Vec<f64> = samples.iter().map(|s| s.frame_time_ms).collect();
    let frame_stats = compute_timing_stats(&frame_times);

    let gpu_times: Vec<f64> = samples.iter().filter_map(|s| s.gpu_time_ms).collect();
    let (gpu_stats, gpu_absence_reason) = if gpu_times.is_empty() {
        (None, Some(reasons::MISSING_GPU_SAMPLES.to_string()))
    } else {
        (compute_timing_stats(&gpu_times), None)
    };

    let held_count = samples.iter().filter(|s| s.is_held).count() as u64;
    let progress_count = samples.len() as u64 - held_count;

    // Phase target budget (e.g. 30 fps = ~33.3ms, 60 fps = ~16.6ms) with 10% pacing tolerance
    let budget_ms = if target_fps > 0.0 {
        (1000.0 / target_fps) * 1.10
    } else {
        18.5
    };
    let dropped_count = samples
        .iter()
        .filter(|s| !s.is_held && s.frame_time_ms > budget_ms)
        .count() as u64;

    SegmentStatistics {
        segment_id: segment_id.to_string(),
        target_fps,
        sample_count,
        coverage,
        duration_ms,
        frame_time_ms: frame_stats,
        gpu_time_ms: gpu_stats,
        gpu_absence_reason,
        held_present_count: held_count,
        progress_present_count: progress_count,
        dropped_frame_count: dropped_count,
    }
}

#[derive(Debug, Clone)]
pub struct ComparisonPolicy {
    pub policy_id: String,
    pub policy_hash: String,
    pub noise_margin_median_ms: f64,
    pub noise_margin_p99_ms: f64,
}

impl Default for ComparisonPolicy {
    fn default() -> Self {
        Self {
            policy_id: "default_v1".to_string(),
            policy_hash: "sha256_default_policy".to_string(),
            noise_margin_median_ms: 0.25,
            noise_margin_p99_ms: 0.50,
        }
    }
}

/// Evaluates a comparison between a baseline and candidate session, separating
/// execution, validity, correctness, performance, hypotheses, and overall verdict.
pub fn evaluate_comparison(
    policy: &ComparisonPolicy,
    comparison_id: &str,
    baseline_req: Option<&DiagnosticRequest>,
    baseline_man: &DiagnosticManifest,
    baseline_out: &DiagnosticOutcome,
    baseline_sum: &DiagnosticSummary,
    candidate_req: Option<&DiagnosticRequest>,
    candidate_man: &DiagnosticManifest,
    candidate_out: &DiagnosticOutcome,
    candidate_sum: &DiagnosticSummary,
) -> DiagnosticComparison {
    let eligibility = evaluate_comparison_eligibility(
        baseline_req,
        baseline_man,
        baseline_out,
        candidate_req,
        candidate_man,
        candidate_out,
    );

    let mut verdict_reasons = Vec::new();
    let mut metric_comparisons = Vec::new();
    let mut trade_offs = Vec::new();
    let mut hypotheses = Vec::new();

    // 1. Correctness dimension evaluation
    // Examine candidate correctness assertions
    let mut correctness_verdict = "passed".to_string();
    let mut required_not_checked = false;
    let mut visual_failed = false;

    for assertion in &candidate_out.correctness_assertions {
        if assertion.status == "failed" {
            if assertion.required {
                correctness_verdict = "candidate_regression".to_string();
                if assertion.kind == "visual" {
                    visual_failed = true;
                }
                verdict_reasons.push(format!(
                    "Required correctness assertion '{}' failed",
                    assertion.assertion_id
                ));
            }
        } else if assertion.status == "not_checked" {
            if assertion.required {
                required_not_checked = true;
                verdict_reasons.push(format!(
                    "Required correctness assertion '{}' was not_checked",
                    assertion.assertion_id
                ));
            }
            // Optional not_checked does not block correctness
        }
    }

    if correctness_verdict != "candidate_regression" && required_not_checked {
        correctness_verdict = "not_checked".to_string();
    }

    // 2. Performance dimension evaluation across matched segments
    let mut performance_verdict = "inconclusive".to_string();
    let mut had_improvement = false;
    let mut had_regression = false;
    let mut had_trade_off = false;

    for (seg_id, base_seg) in &baseline_sum.segments {
        if let Some(cand_seg) = candidate_sum.segments.get(seg_id) {
            if let (Some(b_ft), Some(c_ft)) = (&base_seg.frame_time_ms, &cand_seg.frame_time_ms) {
                let delta_median = c_ft.median - b_ft.median;
                let pct_median = (delta_median / b_ft.median) * 100.0;
                let delta_p99 = c_ft.p99 - b_ft.p99;
                let pct_p99 = (delta_p99 / b_ft.p99) * 100.0;

                let median_improved = delta_median < -policy.noise_margin_median_ms;
                let median_regressed = delta_median > policy.noise_margin_median_ms;
                let p99_regressed = delta_p99 > policy.noise_margin_p99_ms;
                let p99_improved = delta_p99 < -policy.noise_margin_p99_ms;

                metric_comparisons.push(MetricComparison {
                    metric: "frame_time_median_ms".to_string(),
                    segment: seg_id.clone(),
                    baseline_value: Some(b_ft.median),
                    candidate_value: Some(c_ft.median),
                    delta: Some(delta_median),
                    pct_change: Some(pct_median),
                    direction: if median_improved {
                        "improved".to_string()
                    } else if median_regressed {
                        "regressed".to_string()
                    } else {
                        "unchanged".to_string()
                    },
                    noise_margin: policy.noise_margin_median_ms,
                    uncertainty: policy.noise_margin_median_ms / 2.0,
                });

                metric_comparisons.push(MetricComparison {
                    metric: "frame_time_p99_ms".to_string(),
                    segment: seg_id.clone(),
                    baseline_value: Some(b_ft.p99),
                    candidate_value: Some(c_ft.p99),
                    delta: Some(delta_p99),
                    pct_change: Some(pct_p99),
                    direction: if p99_improved {
                        "improved".to_string()
                    } else if p99_regressed {
                        "regressed".to_string()
                    } else {
                        "unchanged".to_string()
                    },
                    noise_margin: policy.noise_margin_p99_ms,
                    uncertainty: policy.noise_margin_p99_ms / 2.0,
                });

                // Conflicting median and tail movement constitutes a trade-off
                if median_improved && p99_regressed {
                    had_trade_off = true;
                    trade_offs.push(ComparisonTradeOff {
                        improved_metric: format!("{}.frame_time_median_ms", seg_id),
                        regressed_metric: format!("{}.frame_time_p99_ms", seg_id),
                        details: format!(
                            "Median improved by {:.2} ms ({:.1}%), but p99 tail latency regressed by {:.2} ms ({:.1}%)",
                            -delta_median, -pct_median, delta_p99, pct_p99
                        ),
                    });
                } else if median_improved {
                    had_improvement = true;
                } else if median_regressed || p99_regressed {
                    had_regression = true;
                }
            }

            // Hypotheses: Check GPU data absence rule
            // Spec requirement: absent GPU samples NEVER independently produce a CPU-bound conclusion
            if cand_seg.gpu_time_ms.is_none() || base_seg.gpu_time_ms.is_none() {
                hypotheses.push(DiagnosticHypothesis {
                    hypothesis_id: "UNSUPPORTED_METRIC".to_string(),
                    status: "inconclusive".to_string(),
                    rationale: format!(
                        "Segment '{}' lacks GPU timing samples; absent GPU data cannot deduce a CPU-bound workload.",
                        seg_id
                    ),
                    evidence_refs: vec![format!("segments/{}/gpu_absence", seg_id)],
                });
            } else if let (Some(b_gpu), Some(c_gpu)) = (&base_seg.gpu_time_ms, &cand_seg.gpu_time_ms) {
                let delta_gpu = c_gpu.median - b_gpu.median;
                if delta_gpu < -policy.noise_margin_median_ms {
                    hypotheses.push(DiagnosticHypothesis {
                        hypothesis_id: "GPU_LATENCY_REDUCTION".to_string(),
                        status: "supported".to_string(),
                        rationale: format!(
                            "GPU median timing reduced from {:.2} ms to {:.2} ms.",
                            b_gpu.median, c_gpu.median
                        ),
                        evidence_refs: vec![format!("segments/{}/gpu_time", seg_id)],
                    });
                }
            }
        }
    }

    if had_trade_off {
        performance_verdict = "trade_off".to_string();
    } else if had_improvement && !had_regression {
        performance_verdict = "improved".to_string();
    } else if had_regression && !had_improvement {
        performance_verdict = "regressed".to_string();
    } else if !had_improvement && !had_regression {
        performance_verdict = "equivalent".to_string();
    }

    // 3. Overall Verdict determination
    let overall_verdict = if !eligibility.is_eligible {
        verdict_reasons.extend(eligibility.ineligibility_reasons.clone());
        "not_comparable".to_string()
    } else if correctness_verdict == "candidate_regression" || visual_failed {
        verdict_reasons.push(
            "Overall result is a regression due to correctness failure, regardless of timing improvements".to_string(),
        );
        "regression".to_string()
    } else if correctness_verdict == "not_checked" {
        verdict_reasons.push(
            "Overall improvement withheld: required correctness assertion was not_checked".to_string(),
        );
        "inconclusive".to_string()
    } else if performance_verdict == "improved" {
        verdict_reasons.push("Performance improved across all checked segments without correctness regression".to_string());
        "improvement".to_string()
    } else if performance_verdict == "trade_off" {
        verdict_reasons.push("Performance exhibits a trade-off between median and tail metrics".to_string());
        "trade_off".to_string()
    } else if performance_verdict == "regressed" {
        verdict_reasons.push("Performance regressed beyond declared noise margins".to_string());
        "regression".to_string()
    } else if performance_verdict == "equivalent" {
        verdict_reasons.push("Performance is equivalent within declared noise margins".to_string());
        "equivalence".to_string()
    } else {
        "inconclusive".to_string()
    };

    DiagnosticComparison {
        schema_version: SchemaVersion::CURRENT,
        comparison_id: comparison_id.to_string(),
        baseline_run_id: baseline_man.run_id.clone(),
        candidate_run_id: candidate_man.run_id.clone(),
        policy_id: policy.policy_id.clone(),
        policy_hash: policy.policy_hash.clone(),
        eligibility,
        dimensions: ComparisonDimensions {
            execution: if baseline_out.execution_outcome == candidate_out.execution_outcome {
                "equivalent".to_string()
            } else {
                "different_coverage".to_string()
            },
            correctness: correctness_verdict,
            performance: performance_verdict,
            overall_verdict,
            verdict_reasons,
        },
        metric_comparisons,
        trade_offs,
        hypotheses,
        extra: BTreeMap::new(),
    }
}

/// Renders a deterministic human-readable Markdown summary report.
pub fn render_markdown_report(
    comp: &DiagnosticComparison,
    baseline_man: &DiagnosticManifest,
    candidate_man: &DiagnosticManifest,
) -> String {
    let mut out = String::new();
    out.push_str(&format!("# Diagnostic Comparison Report: {}\n\n", comp.comparison_id));
    out.push_str(&format!("- **Overall Verdict:** `{}`\n", comp.dimensions.overall_verdict));
    out.push_str(&format!("- **Correctness:** `{}`\n", comp.dimensions.correctness));
    out.push_str(&format!("- **Performance:** `{}`\n", comp.dimensions.performance));
    out.push_str(&format!(
        "- **Baseline Run ID:** `{}` (variant: `{}`)\n",
        comp.baseline_run_id, baseline_man.build_identity.build_variant
    ));
    out.push_str(&format!(
        "- **Candidate Run ID:** `{}` (variant: `{}`)\n\n",
        comp.candidate_run_id, candidate_man.build_identity.build_variant
    ));

    if !comp.dimensions.verdict_reasons.is_empty() {
        out.push_str("## Verdict Reasons\n");
        for r in &comp.dimensions.verdict_reasons {
            out.push_str(&format!("- {}\n", r));
        }
        out.push('\n');
    }

    if !comp.metric_comparisons.is_empty() {
        out.push_str("## Metric Movements\n\n");
        out.push_str("| Metric | Segment | Baseline | Candidate | Delta | % Change | Direction |\n");
        out.push_str("|---|---|---|---|---|---|---|\n");
        for m in &comp.metric_comparisons {
            let b_val = m
                .baseline_value
                .map(|v| format!("{:.2}", v))
                .unwrap_or_else(|| "N/A".to_string());
            let c_val = m
                .candidate_value
                .map(|v| format!("{:.2}", v))
                .unwrap_or_else(|| "N/A".to_string());
            let delta = m
                .delta
                .map(|v| format!("{:.2}", v))
                .unwrap_or_else(|| "N/A".to_string());
            let pct = m
                .pct_change
                .map(|v| format!("{:.1}%", v))
                .unwrap_or_else(|| "N/A".to_string());
            out.push_str(&format!(
                "| {} | {} | {} | {} | {} | {} | {} |\n",
                m.metric, m.segment, b_val, c_val, delta, pct, m.direction
            ));
        }
        out.push('\n');
    }

    if !comp.trade_offs.is_empty() {
        out.push_str("## Trade-offs\n");
        for t in &comp.trade_offs {
            out.push_str(&format!("- {}\n", t.details));
        }
        out.push('\n');
    }

    if !comp.hypotheses.is_empty() {
        out.push_str("## Diagnostic Hypotheses\n");
        for h in &comp.hypotheses {
            out.push_str(&format!(
                "- **{}** (`{}`): {}\n",
                h.hypothesis_id, h.status, h.rationale
            ));
        }
        out.push('\n');
    }

    out
}
