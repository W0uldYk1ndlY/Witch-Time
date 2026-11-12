use super::eligibility::*;
use super::evaluation::*;
use super::models::*;
use super::scenario::*;
use super::validation::*;
use serde_json::Value;

#[test]
fn schema_version_parsing_and_display() {
    let v = SchemaVersion::parse("1.0").unwrap();
    assert_eq!(v.major, 1);
    assert_eq!(v.minor, 0);
    assert_eq!(v.to_string(), "1.0");

    assert!(SchemaVersion::parse("invalid").is_err());
    assert!(SchemaVersion::parse("1.2.3").is_err());
}

#[test]
fn schema_version_validation() {
    let current = SchemaVersion::new(1, 0);
    assert!(validate_schema_version(&current).is_ok());

    let forward_compatible_minor = SchemaVersion::new(1, 4);
    assert!(validate_schema_version(&forward_compatible_minor).is_ok());

    let unsupported_major = SchemaVersion::new(2, 0);
    assert_eq!(
        validate_schema_version(&unsupported_major),
        Err(DiagError::UnsupportedSchemaMajor {
            found: 2,
            supported: 1
        })
    );

    let unsupported_zero = SchemaVersion::new(0, 9);
    assert_eq!(
        validate_schema_version(&unsupported_zero),
        Err(DiagError::UnsupportedSchemaMajor {
            found: 0,
            supported: 1
        })
    );
}

#[test]
fn measurement_null_versus_zero() {
    let measured_zero = DiagnosticMeasurement::new_valid("dropped_frames", 0.0, "count", "counter", 100, 1.0);
    assert!(measured_zero.is_measured_zero());
    assert!(!measured_zero.is_null());

    let null_measurement = DiagnosticMeasurement::new_unavailable("gpu_time", "ms", "gpu_query", reasons::MISSING_GPU_SAMPLES);
    assert!(null_measurement.is_null());
    assert!(!null_measurement.is_measured_zero());
    assert_eq!(null_measurement.reason_code.as_deref(), Some(reasons::MISSING_GPU_SAMPLES));

    // Serialize and inspect raw JSON output to verify "null" vs "0.0"
    let zero_json = serde_json::to_string(&measured_zero).unwrap();
    assert!(zero_json.contains(r#""value":0.0"#));

    let null_json = serde_json::to_string(&null_measurement).unwrap();
    assert!(null_json.contains(r#""value":null"#));

    // Deserialization round-trip preserves both distinctly
    let deserialized_zero: DiagnosticMeasurement = serde_json::from_str(&zero_json).unwrap();
    assert_eq!(deserialized_zero.value, Some(0.0));

    let deserialized_null: DiagnosticMeasurement = serde_json::from_str(&null_json).unwrap();
    assert_eq!(deserialized_null.value, None);
}

#[test]
fn sequence_conflict_detection() {
    // Valid strictly increasing sequence
    let valid_seq = vec![1, 2, 3, 4, 5];
    assert!(validate_strict_sequence(valid_seq).is_ok());

    // Duplicate sequence ID
    let dup_seq = vec![1, 2, 2, 3];
    assert_eq!(
        validate_strict_sequence(dup_seq),
        Err(DiagError::ConflictingSequence {
            prev: 2,
            current: 2,
            reason: "duplicate sequence number"
        })
    );

    // Out-of-order sequence ID
    let ooo_seq = vec![1, 2, 4, 3];
    assert_eq!(
        validate_strict_sequence(ooo_seq),
        Err(DiagError::ConflictingSequence {
            prev: 2,
            current: 4,
            reason: "sequence gap detected"
        })
    );

    let reverse_seq = vec![5, 4, 3];
    assert_eq!(
        validate_strict_sequence(reverse_seq),
        Err(DiagError::ConflictingSequence {
            prev: 5,
            current: 4,
            reason: "out-of-order sequence number"
        })
    );

    // Monotonic sequence allows duplicates but forbids reversal
    assert!(validate_monotonic_sequence(vec![1, 2, 2, 3, 4]).is_ok());
    assert_eq!(
        validate_monotonic_sequence(vec![1, 3, 2]),
        Err(DiagError::ConflictingSequence {
            prev: 3,
            current: 2,
            reason: "out-of-order sequence number"
        })
    );
}

#[test]
fn non_finite_number_rejection() {
    let raw = include_bytes!("../../testdata/diagnostics/invalid_non_finite.json");
    let res: Result<DiagnosticMeasurement, DiagError> = parse_diagnostic_json(raw);
    assert!(matches!(res, Err(DiagError::NonFiniteNumber(_))));
}

#[test]
fn excessive_payload_size_rejection() {
    let dummy = vec![b' '; 2048];
    let res: Result<Value, DiagError> = parse_json_bounded(&dummy, 1024);
    assert!(matches!(res, Err(DiagError::SizeLimitExceeded { size: 2048, limit: 1024 })));
}

#[test]
fn unknown_optional_fields_preservation() {
    let raw = include_bytes!("../../testdata/diagnostics/unknown_optional_fields.json");
    let req: DiagnosticRequest = parse_diagnostic_json(raw).unwrap();
    assert_eq!(req.run_id, "run_20261110_extra_test");
    assert!(req.extra.contains_key("future_unknown_setting"));
    assert!(req.extra.contains_key("experimental_feature_flags"));

    // Verify round-trip preserves the unknown optional fields
    let serialized = serde_json::to_string(&req).unwrap();
    assert!(serialized.contains("future_unknown_setting"));
    assert!(serialized.contains("experimental_feature_flags"));
}

#[test]
fn invalid_schema_major_rejection() {
    let raw = include_bytes!("../../testdata/diagnostics/invalid_schema_major.json");
    let req: DiagnosticRequest = parse_diagnostic_json(raw).unwrap();
    let err = validate_schema_version(&req.schema_version);
    assert_eq!(
        err,
        Err(DiagError::UnsupportedSchemaMajor {
            found: 2,
            supported: CURRENT_SCHEMA_MAJOR
        })
    );
}

#[test]
fn deterministic_fixtures_round_trip() {
    // Request
    let req_bytes = include_bytes!("../../testdata/diagnostics/valid_request.json");
    let req: DiagnosticRequest = parse_diagnostic_json(req_bytes).unwrap();
    assert_eq!(validate_schema_version(&req.schema_version), Ok(()));
    let req_json1 = serde_json::to_string_pretty(&req).unwrap();
    let req_de: DiagnosticRequest = parse_diagnostic_json(req_json1.as_bytes()).unwrap();
    let req_json2 = serde_json::to_string_pretty(&req_de).unwrap();
    assert_eq!(req_json1, req_json2);
    assert_eq!(req, req_de);

    // Manifest
    let man_bytes = include_bytes!("../../testdata/diagnostics/valid_manifest.json");
    let man: DiagnosticManifest = parse_diagnostic_json(man_bytes).unwrap();
    assert_eq!(validate_schema_version(&man.schema_version), Ok(()));
    let man_json1 = serde_json::to_string_pretty(&man).unwrap();
    let man_de: DiagnosticManifest = parse_diagnostic_json(man_json1.as_bytes()).unwrap();
    let man_json2 = serde_json::to_string_pretty(&man_de).unwrap();
    assert_eq!(man_json1, man_json2);
    assert_eq!(man, man_de);

    // Outcome
    let out_bytes = include_bytes!("../../testdata/diagnostics/valid_outcome.json");
    let out: DiagnosticOutcome = parse_diagnostic_json(out_bytes).unwrap();
    assert_eq!(validate_schema_version(&out.schema_version), Ok(()));
    let out_json1 = serde_json::to_string_pretty(&out).unwrap();
    let out_de: DiagnosticOutcome = parse_diagnostic_json(out_json1.as_bytes()).unwrap();
    let out_json2 = serde_json::to_string_pretty(&out_de).unwrap();
    assert_eq!(out_json1, out_json2);
    assert_eq!(out, out_de);

    // Summary
    let sum_bytes = include_bytes!("../../testdata/diagnostics/valid_summary.json");
    let sum: DiagnosticSummary = parse_diagnostic_json(sum_bytes).unwrap();
    assert_eq!(validate_schema_version(&sum.schema_version), Ok(()));
    let sum_json1 = serde_json::to_string_pretty(&sum).unwrap();
    let sum_de: DiagnosticSummary = parse_diagnostic_json(sum_json1.as_bytes()).unwrap();
    let sum_json2 = serde_json::to_string_pretty(&sum_de).unwrap();
    assert_eq!(sum_json1, sum_json2);
    assert_eq!(sum, sum_de);

    // Comparison
    let cmp_bytes = include_bytes!("../../testdata/diagnostics/valid_comparison.json");
    let cmp: DiagnosticComparison = parse_diagnostic_json(cmp_bytes).unwrap();
    assert_eq!(validate_schema_version(&cmp.schema_version), Ok(()));
    let cmp_json1 = serde_json::to_string_pretty(&cmp).unwrap();
    let cmp_de: DiagnosticComparison = parse_diagnostic_json(cmp_json1.as_bytes()).unwrap();
    let cmp_json2 = serde_json::to_string_pretty(&cmp_de).unwrap();
    assert_eq!(cmp_json1, cmp_json2);
    assert_eq!(cmp, cmp_de);
}

#[test]
fn comparison_eligibility_matched_candidate_with_different_binaries_is_eligible() {
    let req_bytes = include_bytes!("../../testdata/diagnostics/valid_request.json");
    let man_bytes = include_bytes!("../../testdata/diagnostics/valid_manifest.json");
    let out_bytes = include_bytes!("../../testdata/diagnostics/valid_outcome.json");

    let b_req: DiagnosticRequest = parse_diagnostic_json(req_bytes).unwrap();
    let b_man: DiagnosticManifest = parse_diagnostic_json(man_bytes).unwrap();
    let b_out: DiagnosticOutcome = parse_diagnostic_json(out_bytes).unwrap();

    let mut c_req = b_req.clone();
    c_req.run_id = "run_candidate_treatment".to_string();

    let mut c_man = b_man.clone();
    c_man.run_id = "run_candidate_treatment".to_string();
    // Candidate has an intentionally different build (different commit, APK hash, and native library hash)
    c_man.source.git_commit = "9999999999999999999999999999999999999999".to_string();
    c_man.build_identity.apk_hash = Some("sha256_candidate_new_apk_hash".to_string());
    c_man.build_identity.runtime_lib_hash = Some("sha256_candidate_opt_lib_hash".to_string());

    let mut c_out = b_out.clone();
    c_out.run_id = "run_candidate_treatment".to_string();

    let eligibility = evaluate_comparison_eligibility(
        Some(&b_req), &b_man, &b_out,
        Some(&c_req), &c_man, &c_out,
    );

    assert!(eligibility.is_eligible, "Matched candidate with new binary must be eligible, got: {:?}", eligibility.ineligibility_reasons);
    assert!(eligibility.ineligibility_reasons.is_empty());
}

#[test]
fn comparison_eligibility_failed_runs_cannot_yield_improvement() {
    let man: DiagnosticManifest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_manifest.json")).unwrap();
    let valid_out: DiagnosticOutcome = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_outcome.json")).unwrap();

    // 1. Failed baseline
    let mut failed_baseline_out = valid_out.clone();
    failed_baseline_out.execution_outcome = "crashed".to_string();
    failed_baseline_out.exit_status.clean_exit = false;
    failed_baseline_out.exit_status.exit_code = Some(139);

    let res_base = evaluate_comparison_eligibility(None, &man, &failed_baseline_out, None, &man, &valid_out);
    assert!(!res_base.is_eligible);
    assert!(res_base.ineligibility_reasons.contains(&reasons::FAILED_BASELINE.to_string()));

    // 2. Failed candidate
    let mut failed_cand_out = valid_out.clone();
    failed_cand_out.execution_outcome = "timed_out".to_string();
    failed_cand_out.exit_status.clean_exit = false;

    let res_cand = evaluate_comparison_eligibility(None, &man, &valid_out, None, &man, &failed_cand_out);
    assert!(!res_cand.is_eligible);
    assert!(res_cand.ineligibility_reasons.contains(&reasons::FAILED_CANDIDATE.to_string()));
}

#[test]
fn comparison_eligibility_partial_and_different_routes_ineligible() {
    let man: DiagnosticManifest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_manifest.json")).unwrap();
    let out: DiagnosticOutcome = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_outcome.json")).unwrap();

    // Partial route (candidate stopped before combat segment)
    let mut partial_out = out.clone();
    partial_out.covered_segments.pop(); // removed "shibuya_fight"
    partial_out.checkpoints.pop();      // removed "cp_combat_end"

    let res_partial = evaluate_comparison_eligibility(None, &man, &out, None, &man, &partial_out);
    assert!(!res_partial.is_eligible);
    assert!(res_partial.ineligibility_reasons.contains(&reasons::PARTIAL_ROUTE.to_string()));

    // Different scenario / route
    let mut diff_route_man = man.clone();
    diff_route_man.fixtures.scenario_hash = "sha256_different_scenario_route".to_string();

    let res_diff = evaluate_comparison_eligibility(None, &man, &out, None, &diff_route_man, &out);
    assert!(!res_diff.is_eligible);
    assert!(res_diff.ineligibility_reasons.contains(&reasons::DIFFERENT_SCENARIOS.to_string()));
}

#[test]
fn comparison_eligibility_lower_resolution_ineligible() {
    let man: DiagnosticManifest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_manifest.json")).unwrap();
    let out: DiagnosticOutcome = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_outcome.json")).unwrap();

    let mut lower_res_man = man.clone();
    lower_res_man.effective_settings.resolution = [1280, 720]; // Baseline was [1600, 900]

    let res = evaluate_comparison_eligibility(None, &man, &out, None, &lower_res_man, &out);
    assert!(!res.is_eligible);
    assert!(res.ineligibility_reasons.contains(&reasons::MISMATCHED_RESOLUTION.to_string()));
}

#[test]
fn comparison_eligibility_missing_acknowledgement_ineligible() {
    let req: DiagnosticRequest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_request.json")).unwrap();
    let man: DiagnosticManifest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_manifest.json")).unwrap();
    let out: DiagnosticOutcome = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_outcome.json")).unwrap();

    let mut unack_man = man.clone();
    unack_man.mismatches.push(crate::diag::models::DiagnosticMismatch {
        field: "resolution".to_string(),
        requested: "1600x900".to_string(),
        effective: "1280x720".to_string(),
        reason_code: reasons::MISMATCHED_RESOLUTION.to_string(),
    });

    let res = evaluate_comparison_eligibility(Some(&req), &man, &out, Some(&req), &unack_man, &out);
    assert!(!res.is_eligible);
    assert!(res.ineligibility_reasons.contains(&reasons::MISSING_ACKNOWLEDGEMENT.to_string()));
}

#[test]
fn segment_statistics_phase_budgets_and_presents() {
    // 30 fps segment: frame time 25ms is within budget; frame time 38ms drops a frame (> 33.33 * 1.10 = 36.66ms)
    let samples_30fps = vec![
        FrameSample { frame_time_ms: 25.0, gpu_time_ms: Some(15.0), is_held: false },
        FrameSample { frame_time_ms: 32.0, gpu_time_ms: Some(16.0), is_held: false },
        FrameSample { frame_time_ms: 38.0, gpu_time_ms: Some(20.0), is_held: false }, // dropped
        FrameSample { frame_time_ms: 33.0, gpu_time_ms: None, is_held: true },        // held present
    ];

    let stats_30 = compute_segment_statistics("cutscene_30", 30.0, &samples_30fps, 4);
    assert_eq!(stats_30.sample_count, 4);
    assert_eq!(stats_30.held_present_count, 1);
    assert_eq!(stats_30.progress_present_count, 3);
    assert_eq!(stats_30.dropped_frame_count, 1);
    assert!(stats_30.gpu_time_ms.is_some());

    // 60 fps segment: frame time 25ms drops a frame (> 16.67 * 1.10 = 18.33ms)
    let samples_60fps = vec![
        FrameSample { frame_time_ms: 16.0, gpu_time_ms: Some(10.0), is_held: false },
        FrameSample { frame_time_ms: 25.0, gpu_time_ms: Some(12.0), is_held: false }, // dropped at 60 fps!
    ];
    let stats_60 = compute_segment_statistics("combat_60", 60.0, &samples_60fps, 2);
    assert_eq!(stats_60.dropped_frame_count, 1);
}

#[test]
fn missing_gpu_data_never_deduces_cpu_bound() {
    let samples_no_gpu = vec![
        FrameSample { frame_time_ms: 25.0, gpu_time_ms: None, is_held: false },
        FrameSample { frame_time_ms: 30.0, gpu_time_ms: None, is_held: false },
    ];
    let seg_stats = compute_segment_statistics("slow_seg", 60.0, &samples_no_gpu, 2);
    assert!(seg_stats.gpu_time_ms.is_none());
    assert_eq!(seg_stats.gpu_absence_reason.as_deref(), Some(reasons::MISSING_GPU_SAMPLES));

    // Construct baseline and candidate summaries with missing GPU data
    let man: DiagnosticManifest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_manifest.json")).unwrap();
    let out: DiagnosticOutcome = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_outcome.json")).unwrap();
    let mut sum: DiagnosticSummary = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_summary.json")).unwrap();
    sum.segments.insert("slow_seg".to_string(), seg_stats);

    let policy = ComparisonPolicy::default();
    let cmp = evaluate_comparison(
        &policy, "cmp_test", None, &man, &out, &sum, None, &man, &out, &sum
    );

    // Verify absent GPU samples NEVER produce CPU_BOUND hypothesis
    assert!(!cmp.hypotheses.iter().any(|h| h.hypothesis_id == "CPU_BOUND"));
    let unsupported_metric = cmp.hypotheses.iter().find(|h| h.hypothesis_id == "UNSUPPORTED_METRIC");
    assert!(unsupported_metric.is_some());
    assert_eq!(unsupported_metric.unwrap().status, "inconclusive");
}

#[test]
fn checked_visual_failure_plus_faster_timing_yields_regression() {
    let man: DiagnosticManifest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_manifest.json")).unwrap();
    let out: DiagnosticOutcome = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_outcome.json")).unwrap();
    let base_sum: DiagnosticSummary = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_summary.json")).unwrap();

    // Candidate has faster timing (improvement)
    let mut cand_sum = base_sum.clone();
    let mut faster_seg = cand_sum.segments.get("shibuya_fight").unwrap().clone();
    if let Some(ft) = &mut faster_seg.frame_time_ms {
        ft.median = 12.0; // Baseline was 16.6 ms (significant improvement!)
        ft.p99 = 15.0;    // Baseline was 21.0 ms
    }
    cand_sum.segments.insert("shibuya_fight".to_string(), faster_seg);

    // BUT candidate has a required visual check failure
    let mut cand_out = out.clone();
    cand_out.correctness_assertions = vec![
        DiagnosticAssertionResult {
            assertion_id: "visual_no_black_screen".to_string(),
            kind: "visual".to_string(),
            required: true,
            status: "failed".to_string(),
            observation_boundary: "framebuffer_texture".to_string(),
            tolerance: None,
            reason_code: Some("BLACK_SCREEN_DETECTED".to_string()),
            evidence_ref: Some("metrics-001.jsonl:100".to_string()),
        }
    ];

    let policy = ComparisonPolicy::default();
    let cmp = evaluate_comparison(
        &policy, "cmp_visual_fail", None, &man, &out, &base_sum, None, &man, &cand_out, &cand_sum
    );

    // Performance is improved, but overall verdict MUST be regression!
    assert_eq!(cmp.dimensions.performance, "improved");
    assert_eq!(cmp.dimensions.correctness, "candidate_regression");
    assert_eq!(cmp.dimensions.overall_verdict, "regression");
}

#[test]
fn optional_versus_required_not_checked_gates() {
    let man: DiagnosticManifest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_manifest.json")).unwrap();
    let out: DiagnosticOutcome = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_outcome.json")).unwrap();
    let base_sum: DiagnosticSummary = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_summary.json")).unwrap();

    let mut cand_sum = base_sum.clone();
    let mut faster_seg = cand_sum.segments.get("shibuya_fight").unwrap().clone();
    if let Some(ft) = &mut faster_seg.frame_time_ms {
        ft.median = 12.0;
        ft.p99 = 15.0;
    }
    cand_sum.segments.insert("shibuya_fight".to_string(), faster_seg);

    // 1. Optional not_checked gate does NOT withhold improvement
    let mut cand_out_optional = out.clone();
    cand_out_optional.correctness_assertions = vec![
        DiagnosticAssertionResult {
            assertion_id: "optional_brightness_heuristic".to_string(),
            kind: "visual".to_string(),
            required: false, // OPTIONAL
            status: "not_checked".to_string(),
            observation_boundary: "framebuffer_texture".to_string(),
            tolerance: None,
            reason_code: None,
            evidence_ref: None,
        }
    ];

    let policy = ComparisonPolicy::default();
    let cmp_opt = evaluate_comparison(
        &policy, "cmp_opt", None, &man, &out, &base_sum, None, &man, &cand_out_optional, &cand_sum
    );
    assert_eq!(cmp_opt.dimensions.overall_verdict, "improvement");

    // 2. Required not_checked gate WITHHOLDS improvement
    let mut cand_out_required = out.clone();
    cand_out_required.correctness_assertions = vec![
        DiagnosticAssertionResult {
            assertion_id: "audio_output_active".to_string(),
            kind: "audio".to_string(),
            required: true, // REQUIRED
            status: "not_checked".to_string(),
            observation_boundary: "output_backend".to_string(),
            tolerance: None,
            reason_code: Some("AUDIO_MUTED".to_string()),
            evidence_ref: None,
        }
    ];

    let cmp_req = evaluate_comparison(
        &policy, "cmp_req", None, &man, &out, &base_sum, None, &man, &cand_out_required, &cand_sum
    );
    assert_eq!(cmp_req.dimensions.overall_verdict, "inconclusive");
    assert_eq!(cmp_req.dimensions.correctness, "not_checked");
}

#[test]
fn conflicting_median_and_tail_movement_yields_trade_off() {
    let man: DiagnosticManifest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_manifest.json")).unwrap();
    let out: DiagnosticOutcome = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_outcome.json")).unwrap();
    let base_sum: DiagnosticSummary = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_summary.json")).unwrap();

    let mut cand_sum = base_sum.clone();
    let mut trade_off_seg = cand_sum.segments.get("shibuya_fight").unwrap().clone();
    if let Some(ft) = &mut trade_off_seg.frame_time_ms {
        ft.median = 14.0; // Baseline was 16.6 ms (improved by 2.6 ms)
        ft.p99 = 26.0;    // Baseline was 21.0 ms (regressed by 5.0 ms!)
    }
    cand_sum.segments.insert("shibuya_fight".to_string(), trade_off_seg);

    let policy = ComparisonPolicy::default();
    let cmp = evaluate_comparison(
        &policy, "cmp_tradeoff", None, &man, &out, &base_sum, None, &man, &out, &cand_sum
    );

    assert_eq!(cmp.dimensions.performance, "trade_off");
    assert_eq!(cmp.dimensions.overall_verdict, "trade_off");
    assert_eq!(cmp.trade_offs.len(), 1);
    assert!(cmp.trade_offs[0].improved_metric.contains("median"));
    assert!(cmp.trade_offs[0].regressed_metric.contains("p99"));
}

#[test]
fn treatments_omitted_old_fields_leave_candidates_off() {
    let req_bytes = include_bytes!("../../testdata/diagnostics/valid_request.json");
    let req: DiagnosticRequest = parse_diagnostic_json(req_bytes).unwrap();
    assert_eq!(req.treatments, None);
    let eff = req.effective_treatments();
    assert_eq!(eff.ww_soft, false);
    assert_eq!(eff.vk_writerwait, false);
    assert_eq!(eff.vk_loadclear, false);
    assert_eq!(eff.vk_split, None);
    assert!(eff.are_candidates_off());

    // Also verify partial treatments specification leaves other candidates off
    let partial_json = r#"{
        "schema_version": "1.0",
        "run_id": "run_partial",
        "build": { "variant": "diagnostic", "target": "android-arm64", "native_optimization": "release" },
        "scenario": { "scenario_id": "s", "route_id": "r", "fixture_id": "f", "deadlines": { "startup_deadline_secs": 1.0, "scenario_deadline_secs": 1.0, "recovery_deadline_secs": 1.0 } },
        "profile": { "profile_name": "p", "memory_budget_bytes": 100, "storage_budget_bytes": 100, "incident_disk_reserve_bytes": 100, "max_incidents": 1, "collectors": [] },
        "configuration": { "resolution": [1600, 900], "fps_target": 60.0, "audio_mode": "output", "cache_mode": "app_cache_cold", "input_policy": { "source": "s", "mapping": "m", "menu_confirm": "c" } },
        "policy": { "policy_id": "p", "policy_hash": "h", "required_assertions": [] },
        "treatments": {
            "vk_writerwait": true
        }
    }"#;
    let partial_req: DiagnosticRequest = parse_diagnostic_json(partial_json.as_bytes()).unwrap();
    let p_eff = partial_req.effective_treatments();
    assert_eq!(p_eff.ww_soft, false);
    assert_eq!(p_eff.vk_writerwait, true);
    assert_eq!(p_eff.vk_loadclear, false);
    assert_eq!(p_eff.vk_split, None);
}

#[test]
fn treatments_invalid_values_are_rejected() {
    let base_req_json = include_str!("../../testdata/diagnostics/valid_request.json");
    let mut req_val: serde_json::Value = serde_json::from_str(base_req_json).unwrap();

    // 1. Unknown / unallowlisted treatment key is rejected
    req_val["treatments"] = serde_json::json!({
        "unsupported_env_control": true
    });
    let err_unknown = parse_diagnostic_json::<DiagnosticRequest>(&serde_json::to_vec(&req_val).unwrap());
    assert!(err_unknown.is_err(), "Unknown treatment key must be rejected");

    // 2. Invalid value type (string instead of boolean) is rejected
    req_val["treatments"] = serde_json::json!({
        "ww_soft": "1"
    });
    let err_type = parse_diagnostic_json::<DiagnosticRequest>(&serde_json::to_vec(&req_val).unwrap());
    assert!(err_type.is_err(), "String value for boolean treatment must be rejected");

    // 3. Negative split value is rejected
    req_val["treatments"] = serde_json::json!({
        "vk_split": -10
    });
    let err_neg = parse_diagnostic_json::<DiagnosticRequest>(&serde_json::to_vec(&req_val).unwrap());
    assert!(err_neg.is_err(), "Negative split value must be rejected");
}

#[test]
fn treatments_unknown_legacy_provenance_stays_unknown() {
    let man_bytes = include_bytes!("../../testdata/diagnostics/valid_manifest.json");
    let man: DiagnosticManifest = parse_diagnostic_json(man_bytes).unwrap();
    assert_eq!(man.effective_treatments, None);
    assert!(!man.is_treatment_provenance_known());
}

#[test]
fn temporal_parser_is_bounded_and_legacy_default_is_absent() {
    let mut req: serde_json::Value = serde_json::from_slice(include_bytes!("../../testdata/diagnostics/valid_request.json")).unwrap();
    for selection in [
        serde_json::json!({"mode":"sgsr2_quality","variant":"unsupported"}),
        serde_json::json!({"mode":"sgsr2_quality"}),
        serde_json::json!({"mode":"fsr3","variant":"2_pass_cs"}),
        serde_json::json!({"mode":6}),
        serde_json::json!({"mode":"native","env":{"WT_ANY":"1"}}),
    ] {
        req["treatments"] = serde_json::json!({"temporal":{"selection":selection,"cohort":"equal_output"}});
        assert!(serde_json::from_value::<DiagnosticRequest>(req.clone()).is_err());
    }
    for variant in ["2_pass_fs", "2_pass_cs", "3_pass_cs"] {
        req["treatments"] = serde_json::json!({"temporal":{"selection":{"mode":"sgsr2_quality","variant":variant},"cohort":"equal_output"}});
        let parsed: DiagnosticRequest = serde_json::from_value(req.clone()).unwrap();
        assert!(parsed.effective_treatments().temporal.is_some());
    }
    req["treatments"]["temporal"]["cohort"] = serde_json::json!("any_resolution");
    assert!(serde_json::from_value::<DiagnosticRequest>(req).is_err());
    assert!(DiagnosticTreatments::default().temporal.is_none());
}

#[test]
fn gpu_component_payload_preserves_pass_serial_and_missing_time() {
    for state in 0..8 {
        let p = [41, (7i64 << 32) | state, 92, if state == 0 || state == 4 { 0 } else { -1 }];
        let decoded = DiagnosticGpuComponent::from_payload(p).unwrap();
        assert_eq!((decoded.frame, decoded.pass_id, decoded.submission_serial), (41, 7, 92));
        assert_eq!(decoded.duration_ns, if state == 0 || state == 4 { Some(0) } else { None });
        let json = serde_json::to_value(&decoded).unwrap();
        assert_eq!(json["duration_ns"].is_null(), state != 0 && state != 4);
        assert_eq!(serde_json::from_value::<DiagnosticGpuComponent>(json).unwrap(), decoded);
    }
    assert!(DiagnosticGpuComponent::from_payload([41, 7i64 << 32, 92, -1]).is_err());
    assert!(DiagnosticGpuComponent::from_payload([41, (7i64 << 32) | 6, 92, 0]).is_err());
    assert!(DiagnosticGpuComponent::from_payload([41, (7i64 << 32) | 99, 92, -1]).is_err());
}

#[test]
fn gpu_component_reduction_matches_submissions_and_preserves_unknown_tail() {
    let event = |frame, pass, serial, state, ns| [frame, ((pass as i64) << 32) | state, serial, ns];
    let events = [
        event(10, 20, 40, 1, -1), event(10, 20, 41, 1, -1),
        event(10, 20, 40, 0, 0), // measured zero replaces pending, not an extra sample
        event(10, 20, 41, 4, 200), event(10, 21, 41, 1, -1),
        event(11, 20, 42, 5, -1), event(11, 21, 42, 2, -1),
        event(11, 22, 42, 3, -1), event(11, 23, 42, 7, -1),
    ];
    let reduced = DiagnosticGpuComponent::reduce_payloads(events, true).unwrap();
    assert_eq!(reduced.len(), 7);
    assert_eq!(reduced[0].duration_ns, Some(0));
    assert_eq!(reduced[1].duration_ns, Some(200));
    assert_eq!((reduced[2].frame, reduced[2].pass_id, reduced[2].submission_serial), (10, 21, 41));
    assert_eq!(reduced[2].state, GpuComponentState::LostTail);
    assert!(reduced[2..].iter().all(|r| r.duration_ns.is_none()));
    let unfinished = DiagnosticGpuComponent::reduce_payloads([event(10, 21, 41, 1, -1)], false).unwrap();
    assert_eq!(unfinished[0].state, GpuComponentState::Pending);
    assert_eq!(unfinished[0].duration_ns, None);
    let pending_updates = DiagnosticGpuComponent::reduce_payloads([event(10, 21, 41, 1, -1), event(10, 21, 41, 1, -1)], true).unwrap();
    assert_eq!(pending_updates.len(), 1);
    assert_eq!(pending_updates[0].state, GpuComponentState::LostTail);
    assert!(DiagnosticGpuComponent::reduce_payloads([], true).unwrap().is_empty());
    for events in [
        vec![event(10, 20, 40, 0, 100), event(10, 20, 40, 0, 100)],
        vec![event(10, 20, 40, 0, 100), event(10, 20, 40, 1, -1)],
        vec![event(10, 20, 40, 1, -1), event(10, 20, 40, 99, -1)],
        vec![event(10, 20, 40, 0, -1)], vec![event(-1, 20, 40, 0, 0)],
    ] {
        assert!(DiagnosticGpuComponent::reduce_payloads(events, true).is_err());
    }
}

#[test]
fn gpu_component_collector_is_opt_in_bounded_and_separate_from_legacy_profiles() {
    let bytes = include_bytes!("../../testdata/diagnostics/valid_request.json");
    let legacy: DiagnosticRequest = parse_diagnostic_json(bytes).unwrap();
    assert!(!legacy.profile.collectors.iter().any(|c| c == "gpu_components"));
    let mut value: serde_json::Value = serde_json::from_slice(bytes).unwrap();
    for collectors in [serde_json::json!(true), serde_json::json!([true]),
        serde_json::json!(["gpu_components_unsupported"]),
        serde_json::json!(["gpu_components", "gpu_components"]),
        serde_json::json!([{"WT_VK_PASSTIME":1}])] {
        value["profile"]["collectors"] = collectors;
        assert!(serde_json::from_value::<DiagnosticRequest>(value.clone()).is_err());
    }
    value["profile"]["collectors"] = serde_json::json!(["gpu_timing", "gpu_components"]);
    value["profile"]["profile_name"] = serde_json::json!("component_timing");
    let components: DiagnosticRequest = serde_json::from_value(value).unwrap();
    let manifest: DiagnosticManifest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_manifest.json")).unwrap();
    let outcome: DiagnosticOutcome = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_outcome.json")).unwrap();
    let comparison = evaluate_comparison_eligibility(Some(&legacy), &manifest, &outcome,
        Some(&components), &manifest, &outcome);
    assert!(!comparison.is_eligible);
    assert!(comparison.ineligibility_reasons.contains(&reasons::INCOMPATIBLE_PROFILES.to_string()));
}

#[test]
fn temporal_cohorts_require_real_acknowledgement_and_matched_unrelated_settings() {
    let mut br: DiagnosticRequest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_request.json")).unwrap();
    let mut bm: DiagnosticManifest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_manifest.json")).unwrap();
    let out: DiagnosticOutcome = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_outcome.json")).unwrap();
    br.treatments = Some(DiagnosticTreatments { temporal: Some(DiagnosticTemporalTreatment {
        selection: TemporalSelection::Native {}, cohort: TemporalCohort::EqualOutput,
    }), ..Default::default() });
    bm.effective_treatments = br.treatments.clone();
    bm.temporal_observation = Some(DiagnosticTemporalObservation {
        effective: TemporalSelection::Native {},
        scene_dimensions: bm.effective_settings.resolution,
        output_dimensions: bm.effective_settings.resolution,
        output_grid: TemporalOutputGrid::Reconstruction, surface_dimensions: Some([1600, 900]),
        motion: TemporalSupport::Unavailable, reactive_consumed: false,
        supported: false, initialized: false, dispatched: false, consumed: false,
        skipped: false, reset_reason: None, fallback_reason: None,
        scene_frame: 1, surface_generation: 1, unrelated_settings_hash: "matched_lod_mips_fxaa_grain_dynamic_resolution".into(),
    });
    let mut cr = br.clone();
    cr.treatments.as_mut().unwrap().temporal.as_mut().unwrap().selection = TemporalSelection::Sgsr2Quality { variant: SgsrVariant::Compute2 };
    let mut cm = bm.clone();
    cm.effective_treatments = cr.treatments.clone();
    let co = cm.temporal_observation.as_mut().unwrap();
    co.effective = cr.treatments.as_ref().unwrap().temporal.as_ref().unwrap().selection.clone();
    co.scene_dimensions = [640, 360];
    co.supported = true; co.initialized = true; co.dispatched = true; co.consumed = true;
    let eligible = |b: &DiagnosticRequest, bm: &DiagnosticManifest, c: &DiagnosticRequest, cm: &DiagnosticManifest| {
        evaluate_comparison_eligibility(Some(b), bm, &out, Some(c), cm, &out).is_eligible
    };
    assert!(eligible(&br, &bm, &cr, &cm), "intentional equal-output treatment");
    let mut drs_bm = bm.clone(); let mut drs_cm = cm.clone();
    for m in [&mut drs_bm, &mut drs_cm] { m.temporal_observation.as_mut().unwrap().output_dimensions = [853, 479]; }
    assert!(eligible(&br, &drs_bm, &cr, &drs_cm), "matching reconstruction grid need not equal configured display or actual surface grid");
    drs_cm.temporal_observation.as_mut().unwrap().output_dimensions = [852, 479];
    assert!(!eligible(&br, &drs_bm, &cr, &drs_cm), "same display does not excuse different reconstruction grid");
    assert!(!evaluate_comparison_eligibility(None, &bm, &out, None, &cm, &out).is_eligible,
        "observed temporal treatment requires declared request cohorts");
    let mut missing_identity = cm.clone(); missing_identity.build_identity.runtime_lib_hash = None;
    assert!(!eligible(&br, &bm, &cr, &missing_identity));
    for split in [false, true] {
        let mut r = cr.clone();
        if split { r.treatments.as_mut().unwrap().vk_split = Some(64); }
        else { r.treatments.as_mut().unwrap().vk_writerwait = true; }
        let mut m = cm.clone(); m.effective_treatments = r.treatments.clone();
        assert!(!eligible(&br, &bm, &r, &m), "unrelated acknowledged treatment differs");
    }
    let mut zero_br = br.clone(); let mut zero_cr = cr.clone();
    let mut zero_bm = bm.clone(); let mut zero_cm = cm.clone();
    zero_br.configuration.resolution = [0, 720]; zero_cr.configuration.resolution = [0, 720];
    for m in [&mut zero_bm, &mut zero_cm] {
        m.effective_settings.resolution = [0, 720];
        m.temporal_observation.as_mut().unwrap().output_dimensions = [0, 720];
    }
    assert!(!eligible(&zero_br, &zero_bm, &zero_cr, &zero_cm), "zero output on both sides");
    let mut bad = cm.clone(); bad.temporal_observation = None;
    assert!(!eligible(&br, &bm, &cr, &bad));
    for field in ["effective", "dimensions", "fallback", "consumed", "grid", "unrelated"] {
        let mut bad = cm.clone(); let o = bad.temporal_observation.as_mut().unwrap();
        match field {
            "effective" => o.effective = TemporalSelection::Native {},
            "dimensions" => o.scene_dimensions = [0, 360],
            "fallback" => o.fallback_reason = Some("unsupported_format".into()),
            "consumed" => o.consumed = false,
            "grid" => o.output_grid = TemporalOutputGrid::Unknown,
            _ => o.unrelated_settings_hash = "different_mip_bias".into(),
        }
        assert!(!eligible(&br, &bm, &cr, &bad), "{field}");
    }
    cr.treatments.as_mut().unwrap().temporal.as_mut().unwrap().cohort = TemporalCohort::EqualRender;
    cm.effective_treatments = cr.treatments.clone();
    assert!(!eligible(&br, &bm, &cr, &cm), "different cohorts");
    br.treatments.as_mut().unwrap().temporal.as_mut().unwrap().cohort = TemporalCohort::EqualRender;
    bm.effective_treatments = br.treatments.clone();
    assert!(!eligible(&br, &bm, &cr, &cm), "different actual render extents");
    bm.temporal_observation.as_mut().unwrap().scene_dimensions = [640, 360];
    assert!(eligible(&br, &bm, &cr, &cm), "equal-render treatment");
    cm.effective_settings.fps_target += 10.0;
    assert!(!eligible(&br, &bm, &cr, &cm), "unrelated pacing mismatch");
}

#[test]
fn comparison_eligibility_treatment_requested_effective_mismatch_ineligible() {
    let req_bytes = include_bytes!("../../testdata/diagnostics/valid_request.json");
    let man_bytes = include_bytes!("../../testdata/diagnostics/valid_manifest.json");
    let out_bytes = include_bytes!("../../testdata/diagnostics/valid_outcome.json");

    let b_req: DiagnosticRequest = parse_diagnostic_json(req_bytes).unwrap();
    let b_man: DiagnosticManifest = parse_diagnostic_json(man_bytes).unwrap();
    let b_out: DiagnosticOutcome = parse_diagnostic_json(out_bytes).unwrap();

    // Candidate requests ww_soft: true
    let mut c_req = b_req.clone();
    c_req.treatments = Some(DiagnosticTreatments {
        temporal: None,
        ww_soft: true,
        vk_writerwait: false,
        vk_loadclear: false,
        vk_split: None,
    });

    // 1. Manifest does not acknowledge effective treatments (remains None / legacy)
    let c_man = b_man.clone();
    let res_missing = evaluate_comparison_eligibility(
        Some(&b_req), &b_man, &b_out,
        Some(&c_req), &c_man, &b_out,
    );
    assert!(!res_missing.is_eligible);
    assert!(res_missing.ineligibility_reasons.contains(&reasons::MISSING_ACKNOWLEDGEMENT.to_string()));

    // 2. Manifest acknowledges mismatched effective treatments (ww_soft: false)
    let mut c_man_mismatch = b_man.clone();
    c_man_mismatch.effective_treatments = Some(DiagnosticTreatments {
        temporal: None,
        ww_soft: false,
        vk_writerwait: false,
        vk_loadclear: false,
        vk_split: None,
    });
    let res_mismatch = evaluate_comparison_eligibility(
        Some(&b_req), &b_man, &b_out,
        Some(&c_req), &c_man_mismatch, &b_out,
    );
    assert!(!res_mismatch.is_eligible);
    assert!(res_mismatch.ineligibility_reasons.contains(&reasons::MISSING_ACKNOWLEDGEMENT.to_string()));

    // 3. Manifest acknowledges matching effective treatments (ww_soft: true) -> eligible!
    let mut c_man_match = b_man.clone();
    c_man_match.effective_treatments = Some(DiagnosticTreatments {
        temporal: None,
        ww_soft: true,
        vk_writerwait: false,
        vk_loadclear: false,
        vk_split: None,
    });
    let res_match = evaluate_comparison_eligibility(
        Some(&b_req), &b_man, &b_out,
        Some(&c_req), &c_man_match, &b_out,
    );
    assert!(res_match.is_eligible, "Matched treatments must be eligible: {:?}", res_match.ineligibility_reasons);
}

#[test]
fn extended_counters_units_and_no_physical_bandwidth_overclaim() {
    // 1. Valid hash_words_visited with unit 'words' passes validation
    let m_valid_words = DiagnosticMeasurement::new_valid(
        metrics::HASH_WORDS_VISITED, 120000.0, units::WORDS, "mem_hash", 100, 1.0
    );
    assert!(validate_metric_semantics(&m_valid_words).is_ok());

    // 2. Rejecting claiming physical bandwidth or bytes for hash_words_visited
    let mut m_invalid_traffic = m_valid_words.clone();
    m_invalid_traffic.units = units::BYTES.to_string();
    let err = validate_metric_semantics(&m_invalid_traffic);
    assert!(err.is_err());
    assert!(err.unwrap_err().contains("cannot claim physical traffic"));

    // 3. Valid snapshot_compare_requested_bytes with unit 'bytes' passes
    let m_snap = DiagnosticMeasurement::new_valid(
        metrics::SNAPSHOT_COMPARE_BYTES, 65536.0, units::BYTES, "cb_compute_snapshot", 4, 1.0
    );
    assert!(validate_metric_semantics(&m_snap).is_ok());

    let mut m_snap_bad = m_snap.clone();
    m_snap_bad.units = units::WORDS.to_string();
    assert!(validate_metric_semantics(&m_snap_bad).is_err());
}

#[test]
fn tracking_failure_cost_accounting_and_version_compatibility() {
    // Failed write-watch queries must be accounted in query count cost
    let mut summary: DiagnosticSummary = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_summary.json")).unwrap();

    // Add extended tracking measurements including both successful and failed queries
    summary.measurements.insert(
        metrics::TRACKER_QUERY_COUNT.to_string(),
        DiagnosticMeasurement::new_valid(metrics::TRACKER_QUERY_COUNT, 42.0, units::COUNT, "plat_write_watch", 42, 1.0)
    );
    summary.measurements.insert(
        metrics::TRACKER_FAULT_COUNT.to_string(),
        DiagnosticMeasurement::new_valid(metrics::TRACKER_FAULT_COUNT, 5.0, units::COUNT, "soft_ww_handler", 5, 1.0)
    );
    summary.measurements.insert(
        metrics::TRACKER_MEMORY_BYTES.to_string(),
        DiagnosticMeasurement::new_valid(metrics::TRACKER_MEMORY_BYTES, 1048576.0, units::BYTES, "soft_ww_tracker", 1, 1.0)
    );
    summary.measurements.insert(
        metrics::CLEAR_FOLDED_COUNT.to_string(),
        DiagnosticMeasurement::new_valid(metrics::CLEAR_FOLDED_COUNT, 12.0, units::COUNT, "nvn_vk_clear", 12, 1.0)
    );

    // Serialization and schema version 1.0 round-trip check
    let json_str = serde_json::to_string_pretty(&summary).unwrap();
    let round_tripped: DiagnosticSummary = parse_diagnostic_json(json_str.as_bytes()).unwrap();
    assert_eq!(round_tripped.schema_version, SchemaVersion::CURRENT);
    assert_eq!(
        round_tripped.measurements.get(metrics::TRACKER_QUERY_COUNT).unwrap().value,
        Some(42.0)
    );
    assert_eq!(
        round_tripped.measurements.get(metrics::TRACKER_MEMORY_BYTES).unwrap().value,
        Some(1048576.0)
    );
}

#[test]
fn heavy_captures_stay_out_of_lightweight_samples() {
    let req: DiagnosticRequest = parse_diagnostic_json(include_bytes!("../../testdata/diagnostics/valid_request.json")).unwrap();
    assert_eq!(req.profile.profile_name, "lightweight");
    assert!(validate_profile_collectors(&req.profile).is_ok());

    // Inject heavy collector into lightweight profile -> must be rejected
    let mut heavy_profile = req.profile.clone();
    heavy_profile.collectors.push("perfetto_trace".to_string());
    let err = validate_profile_collectors(&heavy_profile);
    assert!(err.is_err());
    assert!(err.unwrap_err().contains("Heavy capture collector 'perfetto_trace' is forbidden"));

    let mut dump_profile = req.profile.clone();
    dump_profile.collectors.push("deep_memory_dump".to_string());
    assert!(validate_profile_collectors(&dump_profile).is_err());
}

#[test]
fn scenario_checkpoint_state_machine_happy_path() {
    let scenario = ScenarioDefinition {
        scenario_id: "sc_prologue_test".to_string(),
        checkpoints: vec![
            CheckpointSpec {
                checkpoint_id: "cp_boot".to_string(),
                occurrence: 1,
                target_segment: "boot".to_string(),
                expected: SemanticMatcher {
                    chapter: Some(0),
                    verse: Some(0),
                    difficulty: None,
                    area: None,
                    movie_active: None,
                },
                clock_policy: ClockPolicy::HostTime,
                deadline_ms: 10000,
                required: true,
            },
            CheckpointSpec {
                checkpoint_id: "cp_movie".to_string(),
                occurrence: 1,
                target_segment: "prologue".to_string(),
                expected: SemanticMatcher {
                    chapter: Some(1),
                    verse: Some(0),
                    difficulty: None,
                    area: None,
                    movie_active: Some(true),
                },
                clock_policy: ClockPolicy::HostTime,
                deadline_ms: 15000,
                required: true,
            },
            CheckpointSpec {
                checkpoint_id: "cp_verse1".to_string(),
                occurrence: 1,
                target_segment: "combat".to_string(),
                expected: SemanticMatcher {
                    chapter: Some(1),
                    verse: Some(1),
                    difficulty: Some(1),
                    area: None,
                    movie_active: Some(false),
                },
                clock_policy: ClockPolicy::GameActiveTime,
                deadline_ms: 20000,
                required: true,
            },
        ],
    };

    let mut sm = ScenarioStateMachine::new(scenario);

    // 1. Advance time and send boot observation
    sm.advance_time(500.0, 500.0, 30);
    let obs1 = SemanticObservation {
        chapter: 0,
        verse: 0,
        difficulty: 0,
        area: 0,
        movie_active: false,
        draws_seen: 10,
        produced_at_host_ms: 500.0,
        produced_at_game_ms: 500.0,
    };
    assert!(sm.on_semantic_observation(&obs1));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_boot".to_string()));
    assert_eq!(sm.covered_segments, vec!["boot".to_string()]);

    // 2. Advance time and send movie observation
    sm.advance_time(1000.0, 1000.0, 60);
    let obs2 = SemanticObservation {
        chapter: 1,
        verse: 0,
        difficulty: 1,
        area: 1,
        movie_active: true,
        draws_seen: 25,
        produced_at_host_ms: 1500.0,
        produced_at_game_ms: 1500.0,
    };
    assert!(sm.on_semantic_observation(&obs2));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_movie".to_string()));
    assert_eq!(sm.covered_segments, vec!["boot".to_string(), "prologue".to_string()]);

    // 3. Advance time and send verse1 combat observation
    sm.advance_time(2000.0, 2000.0, 120);
    let obs3 = SemanticObservation {
        chapter: 1,
        verse: 1,
        difficulty: 1,
        area: 1,
        movie_active: false,
        draws_seen: 180,
        produced_at_host_ms: 3500.0,
        produced_at_game_ms: 3500.0,
    };
    assert!(sm.on_semantic_observation(&obs3));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_verse1".to_string()));
    assert_eq!(sm.covered_segments, vec!["boot".to_string(), "prologue".to_string(), "combat".to_string()]);
    assert_eq!(sm.execution_outcome, "COMPLETED");
    assert_eq!(sm.reason_code, reasons::CLEAN_COMPLETION);

    let outcome = sm.generate_outcome("run_123");
    assert_eq!(outcome.checkpoints.len(), 3);
    assert_eq!(outcome.last_confirmed_checkpoint, Some("cp_verse1".to_string()));
}

#[test]
fn scenario_delayed_loading_and_deadlines() {
    let scenario = ScenarioDefinition {
        scenario_id: "sc_timeout_test".to_string(),
        checkpoints: vec![CheckpointSpec {
            checkpoint_id: "cp_load_done".to_string(),
            occurrence: 1,
            target_segment: "loading".to_string(),
            expected: SemanticMatcher {
                chapter: Some(1),
                verse: Some(1),
                difficulty: None,
                area: None,
                movie_active: None,
            },
            clock_policy: ClockPolicy::HostTime,
            deadline_ms: 5000,
            required: true,
        }],
    };

    let mut sm = ScenarioStateMachine::new(scenario);

    // Delayed loading: 6 seconds elapse without semantic match
    sm.advance_time(6000.0, 200.0, 12);
    assert!(sm.is_finished);
    assert_eq!(sm.execution_outcome, "TIMED_OUT");
    assert_eq!(sm.reason_code, reasons::CHECKPOINT_TIMEOUT);
    assert_eq!(sm.results[0].status, "TIMED_OUT");
}

#[test]
fn scenario_repeated_chapter_verse_visits_and_occurrences() {
    let scenario = ScenarioDefinition {
        scenario_id: "sc_repeat_visit".to_string(),
        checkpoints: vec![
            CheckpointSpec {
                checkpoint_id: "cp_ch1_v1_first".to_string(),
                occurrence: 1,
                target_segment: "segment_a".to_string(),
                expected: SemanticMatcher { chapter: Some(1), verse: Some(1), difficulty: None, area: None, movie_active: None },
                clock_policy: ClockPolicy::HostTime,
                deadline_ms: 10000,
                required: true,
            },
            CheckpointSpec {
                checkpoint_id: "cp_ch1_v2".to_string(),
                occurrence: 1,
                target_segment: "segment_b".to_string(),
                expected: SemanticMatcher { chapter: Some(1), verse: Some(2), difficulty: None, area: None, movie_active: None },
                clock_policy: ClockPolicy::HostTime,
                deadline_ms: 10000,
                required: true,
            },
            CheckpointSpec {
                checkpoint_id: "cp_ch1_v1_second".to_string(),
                occurrence: 2,
                target_segment: "segment_c".to_string(),
                expected: SemanticMatcher { chapter: Some(1), verse: Some(1), difficulty: None, area: None, movie_active: None },
                clock_policy: ClockPolicy::HostTime,
                deadline_ms: 10000,
                required: true,
            },
        ],
    };

    let mut sm = ScenarioStateMachine::new(scenario);

    // Visit 1: ch1, v1
    sm.advance_time(100.0, 100.0, 6);
    let obs1 = SemanticObservation { chapter: 1, verse: 1, difficulty: 0, area: 0, movie_active: false, draws_seen: 10, produced_at_host_ms: 100.0, produced_at_game_ms: 100.0 };
    assert!(sm.on_semantic_observation(&obs1));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_ch1_v1_first".to_string()));

    // Visit 2: ch1, v2
    sm.advance_time(200.0, 200.0, 12);
    let obs2 = SemanticObservation { chapter: 1, verse: 2, difficulty: 0, area: 0, movie_active: false, draws_seen: 15, produced_at_host_ms: 300.0, produced_at_game_ms: 300.0 };
    assert!(sm.on_semantic_observation(&obs2));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_ch1_v2".to_string()));

    // Stale obs1 cannot satisfy occurrence 2!
    assert!(!sm.on_semantic_observation(&obs1));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_ch1_v2".to_string()));

    // Fresh visit 3: ch1, v1 (occurrence 2)
    sm.advance_time(300.0, 300.0, 18);
    let obs3 = SemanticObservation { chapter: 1, verse: 1, difficulty: 0, area: 0, movie_active: false, draws_seen: 20, produced_at_host_ms: 600.0, produced_at_game_ms: 600.0 };
    assert!(sm.on_semantic_observation(&obs3));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_ch1_v1_second".to_string()));
    assert_eq!(sm.execution_outcome, "COMPLETED");
}

#[test]
fn scenario_draw_counts_alone_cannot_satisfy_checkpoints() {
    let scenario = ScenarioDefinition {
        scenario_id: "sc_draw_test".to_string(),
        checkpoints: vec![CheckpointSpec {
            checkpoint_id: "cp_semantic_only".to_string(),
            occurrence: 1,
            target_segment: "combat".to_string(),
            expected: SemanticMatcher {
                chapter: Some(2),
                verse: Some(3),
                difficulty: None,
                area: None,
                movie_active: None,
            },
            clock_policy: ClockPolicy::HostTime,
            deadline_ms: 10000,
            required: true,
        }],
    };

    let mut sm = ScenarioStateMachine::new(scenario);

    // Big draw count change (from 10 to 500 draws) but semantic chapter is 1 instead of 2
    sm.advance_time(100.0, 100.0, 6);
    let obs_wrong = SemanticObservation {
        chapter: 1,
        verse: 3,
        difficulty: 0,
        area: 0,
        movie_active: false,
        draws_seen: 500,
        produced_at_host_ms: 100.0,
        produced_at_game_ms: 100.0,
    };
    assert!(!sm.on_semantic_observation(&obs_wrong));
    assert!(sm.last_confirmed_checkpoint.is_none());

    // When semantic state matches, any draw count succeeds
    let obs_right = SemanticObservation {
        chapter: 2,
        verse: 3,
        difficulty: 0,
        area: 0,
        movie_active: false,
        draws_seen: 42,
        produced_at_host_ms: 100.0,
        produced_at_game_ms: 100.0,
    };
    assert!(sm.on_semantic_observation(&obs_right));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_semantic_only".to_string()));
}

#[test]
fn scenario_paused_background_timing() {
    let scenario = ScenarioDefinition {
        scenario_id: "sc_background_test".to_string(),
        checkpoints: vec![CheckpointSpec {
            checkpoint_id: "cp_game_time".to_string(),
            occurrence: 1,
            target_segment: "active".to_string(),
            expected: SemanticMatcher {
                chapter: Some(1),
                verse: Some(1),
                difficulty: None,
                area: None,
                movie_active: None,
            },
            clock_policy: ClockPolicy::GameActiveTime,
            deadline_ms: 2000, // 2s game-active deadline
            required: true,
        }],
    };

    let mut sm = ScenarioStateMachine::new(scenario);

    // App goes to background for 10 seconds of wall time
    sm.set_backgrounded(true);
    sm.advance_time(10000.0, 10000.0, 0); // delta_game_ms is ignored because backgrounded
    assert_eq!(sm.game_active_ms, 0.0);
    assert_eq!(sm.host_elapsed_ms, 10000.0);
    assert!(!sm.is_finished); // Did not time out!

    // App resumes to foreground and runs 500ms
    sm.set_backgrounded(false);
    sm.advance_time(500.0, 500.0, 30);
    assert_eq!(sm.game_active_ms, 500.0);

    let obs = SemanticObservation {
        chapter: 1,
        verse: 1,
        difficulty: 0,
        area: 0,
        movie_active: false,
        draws_seen: 60,
        produced_at_host_ms: 10500.0,
        produced_at_game_ms: 500.0,
    };
    assert!(sm.on_semantic_observation(&obs));
    assert_eq!(sm.execution_outcome, "COMPLETED");
}

#[test]
fn scenario_replay_exhaustion_and_missing_combat_checkpoints() {
    let scenario = ScenarioDefinition {
        scenario_id: "sc_combat_exhaustion".to_string(),
        checkpoints: vec![
            CheckpointSpec {
                checkpoint_id: "cp_boot".to_string(),
                occurrence: 1,
                target_segment: "boot".to_string(),
                expected: SemanticMatcher { chapter: Some(0), verse: None, difficulty: None, area: None, movie_active: None },
                clock_policy: ClockPolicy::HostTime,
                deadline_ms: 5000,
                required: true,
            },
            CheckpointSpec {
                checkpoint_id: "cp_combat_final".to_string(),
                occurrence: 1,
                target_segment: "combat".to_string(),
                expected: SemanticMatcher { chapter: Some(1), verse: Some(2), difficulty: None, area: None, movie_active: None },
                clock_policy: ClockPolicy::HostTime,
                deadline_ms: 15000,
                required: true,
            },
        ],
    };

    let mut sm = ScenarioStateMachine::new(scenario);

    // Confirm cp_boot
    sm.advance_time(100.0, 100.0, 6);
    let obs1 = SemanticObservation { chapter: 0, verse: 0, difficulty: 0, area: 0, movie_active: false, draws_seen: 10, produced_at_host_ms: 100.0, produced_at_game_ms: 100.0 };
    assert!(sm.on_semantic_observation(&obs1));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_boot".to_string()));

    // Replay finishes before reaching cp_combat_final
    sm.on_replay_exhausted();
    assert!(sm.is_finished);
    assert_eq!(sm.execution_outcome, "DIVERTED");
    assert_eq!(sm.reason_code, "REPLAY_EXHAUSTED");
    // Crucial requirement: last confirmed checkpoint is preserved
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_boot".to_string()));
    assert_eq!(sm.covered_segments, vec!["boot".to_string()]);
    assert_eq!(sm.results.len(), 2);
    assert_eq!(sm.results[1].checkpoint_id, "cp_combat_final");
    assert_eq!(sm.results[1].status, "MISSED");
}

#[test]
fn synthetic_boot_continue_combat_scenario_fixture_test() {
    let here = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let diag_dir = here.join("testdata/diagnostics");
    std::fs::create_dir_all(&diag_dir).unwrap();

    // 1. Build dump-free synthetic binary replay fixture
    let mut replay_bytes = Vec::with_capacity(272);
    // Header (64 bytes)
    replay_bytes.extend_from_slice(&0x50525457u32.to_le_bytes()); // "WTRP"
    replay_bytes.extend_from_slice(&1u16.to_le_bytes());          // major 1
    replay_bytes.extend_from_slice(&0u16.to_le_bytes());          // minor 0
    replay_bytes.extend_from_slice(&1u32.to_le_bytes());          // source_policy: SYNTHETIC_FULLKEY_P1
    replay_bytes.extend_from_slice(&1u32.to_le_bytes());          // mapping_policy: DEFAULT_TYPE_A
    replay_bytes.extend_from_slice(&1u32.to_le_bytes());          // confirm_policy: NATIVE_A
    replay_bytes.extend_from_slice(&6u32.to_le_bytes());          // record_count: 6
    replay_bytes.extend_from_slice(&400u64.to_le_bytes());        // total_polls: 400
    replay_bytes.extend_from_slice(&0u32.to_le_bytes());          // flags
    replay_bytes.extend_from_slice(&0u32.to_le_bytes());          // crc32
    replay_bytes.extend_from_slice(&[0u8; 28]);                   // reserved padding

    // 6 records (32 bytes each)
    // Rec 0: poll 0, resting
    replay_bytes.extend_from_slice(&0u64.to_le_bytes());
    replay_bytes.extend_from_slice(&(-1i32).to_le_bytes());
    replay_bytes.extend_from_slice(&0u32.to_le_bytes());
    replay_bytes.extend_from_slice(&0u64.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&1u32.to_le_bytes()); // connected

    // Rec 1: poll 60, A down
    replay_bytes.extend_from_slice(&60u64.to_le_bytes());
    replay_bytes.extend_from_slice(&(-1i32).to_le_bytes());
    replay_bytes.extend_from_slice(&0u32.to_le_bytes());
    replay_bytes.extend_from_slice(&1u64.to_le_bytes()); // BTN_A
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&1u32.to_le_bytes());

    // Rec 2: poll 75, neutral
    replay_bytes.extend_from_slice(&75u64.to_le_bytes());
    replay_bytes.extend_from_slice(&(-1i32).to_le_bytes());
    replay_bytes.extend_from_slice(&0u32.to_le_bytes());
    replay_bytes.extend_from_slice(&0u64.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&1u32.to_le_bytes());

    // Rec 3: poll 180, A down (choose Continue)
    replay_bytes.extend_from_slice(&180u64.to_le_bytes());
    replay_bytes.extend_from_slice(&(-1i32).to_le_bytes());
    replay_bytes.extend_from_slice(&0u32.to_le_bytes());
    replay_bytes.extend_from_slice(&1u64.to_le_bytes()); // BTN_A
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&1u32.to_le_bytes());

    // Rec 4: poll 195, neutral
    replay_bytes.extend_from_slice(&195u64.to_le_bytes());
    replay_bytes.extend_from_slice(&(-1i32).to_le_bytes());
    replay_bytes.extend_from_slice(&0u32.to_le_bytes());
    replay_bytes.extend_from_slice(&0u64.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&1u32.to_le_bytes());

    // Rec 5: poll 300, X down + stick forward
    replay_bytes.extend_from_slice(&300u64.to_le_bytes());
    replay_bytes.extend_from_slice(&(-1i32).to_le_bytes());
    replay_bytes.extend_from_slice(&0u32.to_le_bytes());
    replay_bytes.extend_from_slice(&4u64.to_le_bytes()); // BTN_X (punch)
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&32767i16.to_le_bytes()); // ly forward
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&0i16.to_le_bytes());
    replay_bytes.extend_from_slice(&1u32.to_le_bytes());

    // Footer (16 bytes)
    replay_bytes.extend_from_slice(&0x444E455Fu32.to_le_bytes()); // "_END"
    replay_bytes.extend_from_slice(&6u32.to_le_bytes());          // count check: 6
    replay_bytes.extend_from_slice(&0u64.to_le_bytes());          // checksum

    assert_eq!(replay_bytes.len(), 300);
    let replay_path = diag_dir.join("synthetic_boot_continue_combat.wtrp");
    std::fs::write(&replay_path, &replay_bytes).unwrap();

    // 2. Build dump-free synthetic save fixture
    let mut save_bytes = Vec::with_capacity(64);
    save_bytes.extend_from_slice(b"WTSV"); // Magic
    save_bytes.extend_from_slice(&1u32.to_le_bytes()); // Version
    save_bytes.extend_from_slice(&1u32.to_le_bytes()); // Slot 1
    save_bytes.extend_from_slice(&[0u8; 52]); // Clean synthetic zero-data
    assert_eq!(save_bytes.len(), 64);
    let save_path = diag_dir.join("synthetic_save.bin");
    std::fs::write(&save_path, &save_bytes).unwrap();

    // 3. Load synthetic scenario
    let sc_bytes = std::fs::read(diag_dir.join("synthetic_scenario.json")).unwrap();
    let scenario: ScenarioDefinition = serde_json::from_slice(&sc_bytes).unwrap();
    assert_eq!(scenario.scenario_id, "synthetic_boot_continue_combat");
    assert_eq!(scenario.checkpoints.len(), 4);

    // 4. Drive state machine through boot -> menu -> continue loading -> combat
    let mut sm = ScenarioStateMachine::new(scenario);

    // Step 1: Boot
    sm.advance_time(500.0, 500.0, 30);
    assert!(sm.on_semantic_observation(&SemanticObservation {
        chapter: 0,
        verse: 0,
        difficulty: 0,
        area: 0,
        movie_active: false,
        draws_seen: 8,
        produced_at_host_ms: 500.0,
        produced_at_game_ms: 500.0,
    }));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_boot".to_string()));

    // Step 2: Title Menu
    sm.advance_time(1000.0, 1000.0, 60);
    assert!(sm.on_semantic_observation(&SemanticObservation {
        chapter: 0,
        verse: 0,
        difficulty: 0,
        area: 0,
        movie_active: false,
        draws_seen: 45,
        produced_at_host_ms: 1500.0,
        produced_at_game_ms: 1500.0,
    }));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_title_menu".to_string()));

    // Step 3: Continue Loading
    sm.advance_time(2000.0, 2000.0, 120);
    assert!(sm.on_semantic_observation(&SemanticObservation {
        chapter: 1,
        verse: 0,
        difficulty: 1,
        area: 0,
        movie_active: false,
        draws_seen: 20,
        produced_at_host_ms: 3500.0,
        produced_at_game_ms: 3500.0,
    }));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_continue_loading".to_string()));

    // Step 4: Prologue Combat
    sm.advance_time(3000.0, 3000.0, 180);
    assert!(sm.on_semantic_observation(&SemanticObservation {
        chapter: 1,
        verse: 1,
        difficulty: 1,
        area: 11,
        movie_active: false,
        draws_seen: 220,
        produced_at_host_ms: 6500.0,
        produced_at_game_ms: 6500.0,
    }));
    assert_eq!(sm.last_confirmed_checkpoint, Some("cp_prologue_combat".to_string()));
    assert_eq!(sm.execution_outcome, "COMPLETED");
    assert_eq!(sm.reason_code, reasons::CLEAN_COMPLETION);

    assert_eq!(sm.covered_segments, vec![
        "boot".to_string(),
        "menu".to_string(),
        "loading".to_string(),
        "combat".to_string()
    ]);
}

#[test]
fn test_stage_a_end_to_end_synthetic_flow() {
    use std::collections::BTreeMap;
    use crate::diag::adb::FakeAdb;
    use crate::diag::android_runner::{AndroidRunner, AndroidRunnerConfig, RunnerExitStatus};
    use crate::diag::bootstrap::compute_request_digest;
    use crate::diag::evaluation::{
        compute_segment_statistics, evaluate_comparison, render_markdown_report,
        ComparisonPolicy, FrameSample,
    };
    use serde_json::json;

    let host_temp = std::env::temp_dir().join(format!("stage_a_host_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&host_temp);
    std::fs::create_dir_all(&host_temp).unwrap();

    let candidate_run_id = "stage_a_cand_01";
    let baseline_run_id = "stage_a_base_01";

    // 1. Build synthetic requests
    let req_val = json!({
        "schema_version": "1.0",
        "run_id": candidate_run_id,
        "build": { "variant": "diagnostic", "target": "android-arm64", "native_optimization": "release" },
        "scenario": {
            "scenario_id": "boot_continue_combat", "route_id": "shibuya_prologue", "fixture_id": "prologue_v1",
            "deadlines": { "startup_deadline_secs": 1.0, "scenario_deadline_secs": 2.0, "recovery_deadline_secs": 0.5 }
        },
        "profile": {
            "profile_name": "standard", "memory_budget_bytes": 8388608, "storage_budget_bytes": 134217728,
            "incident_disk_reserve_bytes": 16777216, "max_incidents": 3, "collectors": ["events", "metrics"]
        },
        "configuration": {
            "resolution": [1920, 1080], "fps_target": 60.0, "audio_mode": "native", "cache_mode": "app_cache_cold",
            "input_policy": { "source": "controller", "mapping": "type_a", "menu_confirm": "native_a" }
        },
        "policy": {
            "policy_id": "profile1_standard", "policy_hash": "sha256_p1",
            "required_assertions": ["native_a_confirm", "prologue_combat_presence"]
        }
    });
    let req_bytes = serde_json::to_vec(&req_val).unwrap();
    let cand_req: DiagnosticRequest = parse_diagnostic_json(&req_bytes).unwrap();
    let req_digest = compute_request_digest(&req_bytes);

    // 2. Setup Fake ADB with connected device and simulated Stage A execution
    let fake = FakeAdb::new();
    fake.add_device("stage_a_device", "device");

    // Device writes startup acknowledgement before dlopen
    let manifest_bytes = json!({
        "schema_version": "1.0",
        "run_id": candidate_run_id,
        "request_digest": req_digest,
        "status": "acknowledged",
        "startup_phase": "bootstrap_pre_dlopen"
    });
    let remote_manifest_path = format!(
        "/sdcard/Android/data/org.witchtime.android/files/diagnostics/{}/manifest.json",
        candidate_run_id
    );
    fake.write_remote_file(
        "stage_a_device",
        &remote_manifest_path,
        serde_json::to_vec(&manifest_bytes).unwrap(),
    );

    // Device writes outcome upon completion of scenario
    let outcome_bytes = json!({
        "schema_version": "1.0",
        "run_id": candidate_run_id,
        "execution_outcome": "completed",
        "reason_code": reasons::CLEAN_COMPLETION,
        "exit_status": {
            "clean_exit": true,
            "exit_code": 0,
            "termination_origin": "guest_exit"
        },
        "covered_segments": ["boot", "loading", "combat"],
        "last_confirmed_checkpoint": "cp_prologue_combat",
        "correctness_assertions": [
            {
                "assertion_id": "native_a_confirm",
                "kind": "policy",
                "status": "passed",
                "required": true,
                "observation_boundary": "nnhid_delivered_pad"
            },
            {
                "assertion_id": "prologue_combat_presence",
                "kind": "semantic",
                "status": "passed",
                "required": true,
                "observation_boundary": "game_thread_presence"
            },
            {
                // Unimplemented Stage B/C gate MUST stay not_checked
                "assertion_id": "continuous_hardware_counters",
                "kind": "hardware",
                "status": "not_checked",
                "required": false,
                "observation_boundary": "continuous_recorder"
            }
        ]
    });
    let remote_outcome_path = format!(
        "/sdcard/Android/data/org.witchtime.android/files/diagnostics/{}/outcome.json",
        candidate_run_id
    );
    fake.write_remote_file(
        "stage_a_device",
        &remote_outcome_path,
        serde_json::to_vec(&outcome_bytes).unwrap(),
    );

    // 3. Execute runner via fake transport
    let cfg = AndroidRunnerConfig {
        serial: Some("stage_a_device".to_string()),
        package_id: "org.witchtime.android".to_string(),
        activity_name: ".WitchTimeActivity".to_string(),
        run_id: candidate_run_id.to_string(),
        request_bytes: req_bytes.clone(),
        host_output_dir: host_temp.join(candidate_run_id),
        startup_deadline_secs: 1.0,
        scenario_deadline_secs: 2.0,
        recovery_deadline_secs: 0.5,
        max_recovery_attempts: 1,
        allow_reconnect_without_relaunch: true,
    };
    let runner = AndroidRunner::new(&fake, cfg);
    let run_res = runner.execute();

    assert_eq!(run_res.exit_status, RunnerExitStatus::Completed);
    assert!(run_res.acknowledged);

    // Verify pulled artifacts exist locally
    assert!(host_temp.join(candidate_run_id).join("manifest.json").is_file());
    assert!(host_temp.join(candidate_run_id).join("outcome.json").is_file());

    // 4. Offline Analysis and Comparison
    let cand_out: DiagnosticOutcome = parse_diagnostic_json(&serde_json::to_vec(&outcome_bytes).unwrap()).unwrap();

    // Verify that unimplemented Stage B/C hardware gate remains not_checked
    let hw_assertion = cand_out
        .correctness_assertions
        .iter()
        .find(|a| a.assertion_id == "continuous_hardware_counters")
        .unwrap();
    assert_eq!(hw_assertion.status, "not_checked");

    // Compute segment statistics for candidate
    let mut cand_samples = Vec::new();
    for _ in 0..120 {
        cand_samples.push(FrameSample {
            frame_time_ms: 16.5,
            gpu_time_ms: Some(12.0),
            is_held: false,
        });
    }
    let cand_combat_stats = compute_segment_statistics("combat", 60.0, &cand_samples, 120);
    let mut cand_segments = BTreeMap::new();
    cand_segments.insert("combat".to_string(), cand_combat_stats);

    let cand_summary = DiagnosticSummary {
        schema_version: SchemaVersion::CURRENT,
        run_id: candidate_run_id.to_string(),
        overall_outcome: "completed".to_string(),
        validity: "valid".to_string(),
        correctness: "passed".to_string(),
        segments: cand_segments,
        measurements: BTreeMap::new(),
        evidence_references: Vec::new(),
        hypotheses: Vec::new(),
        extra: BTreeMap::new(),
    };

    // Baseline setup
    let mut base_req = cand_req.clone();
    base_req.run_id = baseline_run_id.to_string();

    let mut base_samples = Vec::new();
    for _ in 0..120 {
        base_samples.push(FrameSample {
            frame_time_ms: 18.0,
            gpu_time_ms: Some(14.0),
            is_held: false,
        });
    }
    let base_combat_stats = compute_segment_statistics("combat", 60.0, &base_samples, 120);
    let mut base_segments = BTreeMap::new();
    base_segments.insert("combat".to_string(), base_combat_stats);

    let base_summary = DiagnosticSummary {
        schema_version: SchemaVersion::CURRENT,
        run_id: baseline_run_id.to_string(),
        overall_outcome: "completed".to_string(),
        validity: "valid".to_string(),
        correctness: "passed".to_string(),
        segments: base_segments,
        measurements: BTreeMap::new(),
        evidence_references: Vec::new(),
        hypotheses: Vec::new(),
        extra: BTreeMap::new(),
    };

    let mut cand_man = parse_diagnostic_json::<DiagnosticManifest>(include_bytes!(
        "../../testdata/diagnostics/valid_manifest.json"
    ))
    .unwrap();
    cand_man.run_id = candidate_run_id.to_string();
    cand_man.effective_settings.resolution = cand_req.configuration.resolution;
    cand_man.effective_settings.audio_mode = cand_req.configuration.audio_mode.clone();

    let mut base_man = cand_man.clone();
    base_man.run_id = baseline_run_id.to_string();

    let mut base_out = cand_out.clone();
    base_out.run_id = baseline_run_id.to_string();

    let policy = ComparisonPolicy::default();
    let comp = evaluate_comparison(
        &policy,
        "comp_stage_a_01",
        Some(&base_req),
        &base_man,
        &base_out,
        &base_summary,
        Some(&cand_req),
        &cand_man,
        &cand_out,
        &cand_summary,
    );

    // Evaluated improvement without false positives
    assert!(comp.eligibility.is_eligible);
    assert_eq!(comp.dimensions.correctness, "passed");
    assert_eq!(comp.dimensions.performance, "improved");
    assert_eq!(comp.dimensions.overall_verdict, "improvement");

    // 5. Render and verify Markdown report
    let md_report = render_markdown_report(&comp, &base_man, &cand_man);
    assert!(md_report.contains("# Diagnostic Comparison Report: comp_stage_a_01"));
    assert!(md_report.contains("- **Overall Verdict:** `improvement`"));
    assert!(md_report.contains("| Metric | Segment | Baseline | Candidate | Delta | % Change | Direction |"));
    assert!(md_report.contains("improved"));

    // 6. Verification: missing required evidence CANNOT turn green
    let mut incomplete_out = cand_out.clone();
    incomplete_out.correctness_assertions[0].status = "not_checked".to_string(); // required assertion not checked!
    let incomplete_comp = evaluate_comparison(
        &policy,
        "comp_stage_a_incomplete",
        Some(&base_req),
        &base_man,
        &base_out,
        &base_summary,
        Some(&cand_req),
        &cand_man,
        &incomplete_out,
        &cand_summary,
    );
    assert_ne!(incomplete_comp.dimensions.overall_verdict, "improvement");
    assert_eq!(incomplete_comp.dimensions.overall_verdict, "inconclusive");

    let _ = std::fs::remove_dir_all(&host_temp);
}

#[test]
fn continuous_counters_units_limits_and_null_versus_zero() {
    use crate::diag::models::ContinuousCounters;

    let counters = ContinuousCounters {
        total_wait_events: 10,
        total_wait_ns: 25_000_000,
        shader_queued_jobs: 0, // measured zero jobs queued
        shader_completed_jobs: 50,
        shader_active_workers: 4,
        shader_avg_compile_ns: 1_200_000,
        file_ops_count: 5,
        file_total_duration_ns: 3_500_000,
        file_total_bytes: 131072,
        mem_compare_bytes: 4096,
        mem_copy_bytes: 8192,
        mem_stream_bytes: 16384,
        mem_allocated_bytes: 15_000_000,
        mem_peak_allocated_bytes: 20_000_000,
        audio_appended_frames: 48000,
        audio_released_frames: 47500,
        audio_underruns: 0, // measured zero underruns
        audio_starved: 0,   // measured zero starved
    };

    let measurements = counters.to_measurements();

    // Verify measured zeroes are strictly preserved as Some(0.0)
    let shader_queued = measurements.iter().find(|m| m.name == "shader_queued_jobs").unwrap();
    assert!(shader_queued.is_measured_zero());
    assert_eq!(shader_queued.units, "count");

    let audio_underruns = measurements.iter().find(|m| m.name == "audio_underruns").unwrap();
    assert!(audio_underruns.is_measured_zero());
    assert_eq!(audio_underruns.units, "count");

    // Verify units
    let wait_ns = measurements.iter().find(|m| m.name == "thread_wait_total_ns").unwrap();
    assert_eq!(wait_ns.units, "ns");
    assert_eq!(wait_ns.value, Some(25_000_000.0));

    let file_bytes = measurements.iter().find(|m| m.name == "file_io_total_bytes").unwrap();
    assert_eq!(file_bytes.units, "bytes");
    assert_eq!(file_bytes.value, Some(131072.0));

    // Verify unavailable counters serialize strictly to null
    let mut unavailable_counters = ContinuousCounters::default();
    unavailable_counters.shader_queued_jobs = -1; // Sentinel for unavailable backend/driver
    unavailable_counters.audio_appended_frames = -1;

    let unavail_meas = unavailable_counters.to_measurements();
    let unavail_shader = unavail_meas.iter().find(|m| m.name == "shader_queued_jobs").unwrap();
    assert!(unavail_shader.is_null());
    assert_eq!(unavail_shader.capability, "unavailable");
    assert_eq!(unavail_shader.validity, "unsupported");

    let unavail_audio = unavail_meas.iter().find(|m| m.name == "audio_appended_frames").unwrap();
    assert!(unavail_audio.is_null());
    assert_eq!(unavail_audio.capability, "unavailable");
}

#[test]
fn test_live_status_model_and_stall_semantics() {
    use crate::diag::models::DiagnosticLiveStatus;
    use serde_json::json;

    // 1. Foreground active with thermal support
    let val_active = json!({
        "schema_version": "1.0",
        "run_id": "run_test_status_01",
        "timestamp_ns": 1000000000u64,
        "healthy": true,
        "state": "active",
        "lifecycle": {
            "state": "resumed",
            "is_foreground": true
        },
        "surface_generation": 2,
        "thermal": {
            "headroom": 0.85,
            "status": "light"
        },
        "progress": {
            "render_age_ms": 16,
            "semantic_freshness_ms": 120,
            "last_chapter": 1,
            "last_verse": 2,
            "is_active_stall": false,
            "expected_inactivity": false
        },
        "recorder": {
            "total_emitted": 500,
            "total_dropped": 0,
            "chunks_committed": 5,
            "total_records_written": 500,
            "total_bytes_written": 60000,
            "remaining_budget": 134157728u64,
            "reserve_held": false,
            "incidents_written": 0,
            "active_producers": 2
        }
    });

    let status_active: DiagnosticLiveStatus = serde_json::from_value(val_active).unwrap();
    assert!(status_active.healthy);
    assert_eq!(status_active.lifecycle.state, "resumed");
    assert!(status_active.lifecycle.is_foreground);
    assert_eq!(status_active.surface_generation, 2);
    assert_eq!(status_active.thermal.as_ref().unwrap().headroom, Some(0.85));
    assert_eq!(status_active.progress.render_age_ms, 16);
    assert_eq!(status_active.progress.last_chapter, Some(1));
    assert!(!status_active.progress.is_active_stall);
    assert!(!status_active.progress.expected_inactivity);

    // 2. Background expected inactivity with unsupported thermal
    let val_bg = json!({
        "schema_version": "1.0",
        "run_id": "run_test_status_02",
        "timestamp_ns": 2000000000u64,
        "healthy": true,
        "state": "active",
        "lifecycle": {
            "state": "stopped",
            "is_foreground": false
        },
        "surface_generation": 1,
        "thermal": null,
        "progress": {
            "render_age_ms": 15000,
            "semantic_freshness_ms": null,
            "last_chapter": null,
            "last_verse": null,
            "is_active_stall": false,
            "expected_inactivity": true
        },
        "recorder": {
            "total_emitted": 100,
            "total_dropped": 0,
            "chunks_committed": 1,
            "total_records_written": 100,
            "total_bytes_written": 12000,
            "remaining_budget": 134205728u64,
            "reserve_held": false,
            "incidents_written": 0,
            "active_producers": 1
        }
    });

    let status_bg: DiagnosticLiveStatus = serde_json::from_value(val_bg).unwrap();
    assert!(!status_bg.lifecycle.is_foreground);
    assert_eq!(status_bg.lifecycle.state, "stopped");
    assert!(status_bg.thermal.is_none());
    assert!(status_bg.progress.expected_inactivity);
    assert!(!status_bg.progress.is_active_stall); // Must NOT be labelled an active stall!
    assert!(status_bg.progress.semantic_freshness_ms.is_none());
}

#[test]
fn test_incident_manifest_roundtrip_and_linked_recovery() {
    use crate::diag::models::DiagnosticIncidentManifest;
    use serde_json::json;

    // 1. Incident with prehistory and no fake pre-trigger screenshot
    let inc_val = json!({
        "schema_version": "1.0",
        "incident_id": "incident-001",
        "run_id": "run_test_incident_01",
        "trigger_type": "stall",
        "reason_code": "SCENARIO_TIMEOUT",
        "message": "Render thread stalled for 5.0 seconds",
        "timestamp_ns": 5000000000u64,
        "prehistory": {
            "event_count": 100,
            "start_seq": 1,
            "end_seq": 100
        },
        "capture_coverage": {
            "pre_trigger_screenshot": "none",
            "deep_trace": "unavailable",
            "post_trigger_coverage": "started",
            "collector_start_ns": 5000000000u64
        },
        "recovery": {
            "recovered": true,
            "reason": "surface_recreated_frames_resumed",
            "resume_timestamp_ns": 6500000000u64
        }
    });

    let manifest: DiagnosticIncidentManifest = serde_json::from_value(inc_val).unwrap();
    assert_eq!(manifest.incident_id, "incident-001");
    assert_eq!(manifest.trigger_type, "stall");
    assert_eq!(manifest.reason_code, "SCENARIO_TIMEOUT");
    assert_eq!(manifest.prehistory.event_count, 100);
    assert_eq!(manifest.capture_coverage.pre_trigger_screenshot, "none");
    assert_eq!(manifest.capture_coverage.post_trigger_coverage, "started");

    let rec = manifest.recovery.unwrap();
    assert!(rec.recovered);
    assert_eq!(rec.reason, "surface_recreated_frames_resumed");
    assert_eq!(rec.resume_timestamp_ns, 6500000000);
}

#[test]
fn test_offline_symbolization_and_exit_info() {
    use crate::diag::symbolization::*;
    use std::collections::HashMap;

    // 1. Raw crash breadcrumb parser
    let mut raw_crumb = [0u8; 64];
    raw_crumb[0..4].copy_from_slice(&0x43525348u32.to_le_bytes()); // DIAG_CRASH_MAGIC
    raw_crumb[4..8].copy_from_slice(&11u32.to_le_bytes()); // SIGSEGV
    raw_crumb[8..16].copy_from_slice(&0xDEADBEEFCAFEu64.to_le_bytes()); // fault address
    raw_crumb[16..20].copy_from_slice(&4321u32.to_le_bytes()); // thread_id
    raw_crumb[20..24].copy_from_slice(&0u32.to_le_bytes()); // exit_reason = 0
    raw_crumb[24..32].copy_from_slice(&123456789000u64.to_le_bytes()); // monotonic_ns
    raw_crumb[32..40].copy_from_slice(&555u64.to_le_bytes()); // global_seq

    let crumb = parse_crash_breadcrumb(&raw_crumb).unwrap();
    assert_eq!(crumb.magic, DIAG_CRASH_MAGIC);
    assert_eq!(crumb.signal_num, 11);
    assert_eq!(crumb.fault_address, 0xDEADBEEFCAFE);
    assert_eq!(crumb.thread_id, 4321);
    assert_eq!(crumb.monotonic_ns, 123456789000);
    assert_eq!(crumb.global_seq, 555);

    // Invalid magic and short slice
    let mut bad_magic = raw_crumb;
    bad_magic[0..4].copy_from_slice(&0x12345678u32.to_le_bytes());
    assert!(parse_crash_breadcrumb(&bad_magic).is_err());
    assert!(parse_crash_breadcrumb(&raw_crumb[..30]).is_err());

    // 2. dumpsys exit-info parsing
    let dumpsys_valid = r#"
ApplicationExitInfo #0:
  package=org.witchtime.android
  pid=12345
  process=org.witchtime.android
  reason=5 (CRASH_NATIVE)
  status=11
  importance=100
  timestamp=1700000000000
  description=Segmentation fault
"#;
    let exit_status = parse_dumpsys_exit_info(dumpsys_valid, "org.witchtime.android");
    match exit_status {
        ExitInfoStatus::Found(rec) => {
            assert_eq!(rec.pid, 12345);
            assert_eq!(rec.reason, ApplicationExitReason::NativeCrash);
            assert_eq!(rec.status, 11);
            assert_eq!(rec.importance, 100);
            assert_eq!(rec.description.as_deref(), Some("Segmentation fault"));
        }
        _ => panic!("Expected Found ApplicationExitRecord"),
    }

    // Absent or overwritten
    let dumpsys_empty = "No exit info found for package org.witchtime.android";
    assert_eq!(
        parse_dumpsys_exit_info(dumpsys_empty, "org.witchtime.android"),
        ExitInfoStatus::AbsentOrOverwritten
    );

    // API level unsupported (< 30)
    let dumpsys_unsupported = "Unknown service: activity exit-info";
    assert_eq!(
        parse_dumpsys_exit_info(dumpsys_unsupported, "org.witchtime.android"),
        ExitInfoStatus::ApiLevelUnsupported(29)
    );

    // 3. Logcat native tombstone parsing
    let logcat_sample = r#"
01-01 12:00:00.000  1234  1234 F DEBUG   : *** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
01-01 12:00:00.000  1234  1234 F DEBUG   : Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0xdeadbeefcafe in tid 4321 (witch-time-game)
01-01 12:00:00.000  1234  1234 F DEBUG   : backtrace:
01-01 12:00:00.000  1234  1234 F DEBUG   :   #00 pc 000000000004a120  /data/app/.../libwitch-time-game.so (BuildId: 0123456789abcdef)
01-01 12:00:00.000  1234  1234 F DEBUG   :   #01 pc 000000000002c000  /data/app/.../libwitch-time-game.so (BuildId: 0123456789abcdef)
01-01 12:00:00.000  1234  1234 F DEBUG   :   #02 pc 000000000005b340  /apex/com.android.runtime/lib64/bionic/libc.so
"#;
    let tb = parse_logcat_tombstone(logcat_sample).unwrap();
    assert_eq!(tb.signal, 11);
    assert_eq!(tb.signal_name, "SIGSEGV");
    assert_eq!(tb.fault_address, 0xdeadbeefcafe);
    assert_eq!(tb.tid, 4321);
    assert_eq!(tb.frames.len(), 3);
    assert_eq!(tb.frames[0].frame_num, 0);
    assert_eq!(tb.frames[0].module_offset, 0x4a120);
    assert_eq!(tb.frames[0].build_id.as_deref(), Some("0123456789abcdef"));

    // 4. Local private offline symbolization
    let mut guest_map = HashMap::new();
    guest_map.insert(0x4a000, "f_710004a000".to_string());

    let local_modules = vec![LocalModuleSymbols {
        module_name: "libwitch-time-game.so".to_string(),
        expected_build_id: "0123456789abcdef".to_string(),
        symbols: vec![
            LocalSymbolEntry {
                symbol_name: "f_710004a000".to_string(),
                start_offset: 0x4a000,
                size: 0x200,
            },
            LocalSymbolEntry {
                symbol_name: "nvn_vk_present".to_string(),
                start_offset: 0x2b000,
                size: 0x1500,
            },
        ],
        guest_map,
    }];

    // Symbolize frame 0 (guest function)
    let sym0 = symbolize_frame(&tb.frames[0], &local_modules);
    assert_eq!(sym0.module, "libwitch-time-game.so");
    assert_eq!(sym0.symbol, "f_710004a000+0x120");
    assert_eq!(sym0.guest_function.as_deref(), Some("f_710004a000"));
    assert_eq!(sym0.resolution_status, SymbolResolutionStatus::ResolvedGuestMap);

    // Symbolize frame 1 (host helper)
    let sym1 = symbolize_frame(&tb.frames[1], &local_modules);
    assert_eq!(sym1.symbol, "nvn_vk_present+0x1000");
    assert_eq!(sym1.guest_function, None);
    assert_eq!(sym1.resolution_status, SymbolResolutionStatus::ResolvedExact);

    // Frame with BuildId mismatch must refuse wrong symbols and fall back to raw offset
    let frame_mismatch = TombstoneFrame {
        frame_num: 0,
        pc: 0x4a120,
        module_path: "libwitch-time-game.so".to_string(),
        module_offset: 0x4a120,
        build_id: Some("fedcba9876543210".to_string()),
        function_name: None,
        function_offset: None,
    };
    let sym_mismatch = symbolize_frame(&frame_mismatch, &local_modules);
    assert_eq!(sym_mismatch.symbol, "libwitch-time-game.so+0x4a120");
    match sym_mismatch.resolution_status {
        SymbolResolutionStatus::BuildIdMismatch { expected, actual } => {
            assert_eq!(expected, "0123456789abcdef");
            assert_eq!(actual, "fedcba9876543210");
        }
        _ => panic!("Expected BuildIdMismatch"),
    }

    // Missing symbol table (libc.so) falls back to raw offset
    let sym_libc = symbolize_frame(&tb.frames[2], &local_modules);
    assert_eq!(sym_libc.module, "libc.so");
    assert_eq!(sym_libc.symbol, "libc.so+0x5b340");
    assert_eq!(sym_libc.resolution_status, SymbolResolutionStatus::SymbolsNotFound);

    // 5. evaluate_crash_analysis: Missing record never fabricated into confirmed cause
    let outcome_missing = evaluate_crash_analysis(
        None,
        ExitInfoStatus::AbsentOrOverwritten,
        None,
        &local_modules,
        None,
    );
    assert_eq!(outcome_missing.confirmed_crash_cause, None);

    // Confirmed cause when tombstone is present
    let outcome_confirmed = evaluate_crash_analysis(
        Some(crumb),
        ExitInfoStatus::AbsentOrOverwritten,
        Some(tb),
        &local_modules,
        None,
    );
    assert!(outcome_confirmed.confirmed_crash_cause.is_some());
    assert!(outcome_confirmed
        .confirmed_crash_cause
        .unwrap()
        .contains("Signal 11 (SIGSEGV)"));
}

#[test]
fn test_collector_command_and_capability_checks() {
    use crate::diag::adb::FakeAdb;
    use crate::diag::collector::*;

    // 1. Command construction: Perfetto
    let perf_cfg = DeepCaptureConfig {
        collector_type: DeepCollectorType::Perfetto,
        app_package: "org.witchtime.android".to_string(),
        max_duration_secs: 10.0,
        max_size_bytes: 32 * 1024 * 1024,
        categories: vec!["sched".to_string(), "gfx".to_string(), "view".to_string()],
        sample_frequency_hz: None,
        output_remote_path: "/data/local/tmp/perfetto.trace".to_string(),
    };
    let perf_cmd = build_collector_command(&perf_cfg).unwrap();
    assert_eq!(perf_cmd[0], "perfetto");
    assert!(perf_cmd.contains(&"-o".to_string()));
    assert!(perf_cmd.contains(&"/data/local/tmp/perfetto.trace".to_string()));
    assert!(perf_cmd.contains(&"--app".to_string()));
    assert!(perf_cmd.contains(&"org.witchtime.android".to_string()));
    assert!(perf_cmd.contains(&"sched".to_string()));
    // Verify no root command
    assert!(!perf_cmd.iter().any(|c| c == "su" || c == "root"));

    // 2. Command construction: Simpleperf
    let simp_cfg = DeepCaptureConfig {
        collector_type: DeepCollectorType::Simpleperf,
        app_package: "org.witchtime.android".to_string(),
        max_duration_secs: 5.0,
        max_size_bytes: 16 * 1024 * 1024,
        categories: vec![],
        sample_frequency_hz: Some(2000),
        output_remote_path: "/data/local/tmp/perf.data".to_string(),
    };
    let simp_cmd = build_collector_command(&simp_cfg).unwrap();
    assert_eq!(simp_cmd[0], "simpleperf");
    assert_eq!(simp_cmd[1], "record");
    assert!(simp_cmd.contains(&"-f".to_string()));
    assert!(simp_cmd.contains(&"2000".to_string()));
    assert!(!simp_cmd.iter().any(|c| c == "su" || c == "root"));

    // Invalid parameters
    let mut bad_cfg = perf_cfg.clone();
    bad_cfg.app_package = "".to_string();
    assert!(build_collector_command(&bad_cfg).is_err());
    bad_cfg.app_package = "org.witchtime.android".to_string();
    bad_cfg.max_duration_secs = 0.0;
    assert!(build_collector_command(&bad_cfg).is_err());

    // 3. Capability checks using FakeAdb
    let fake = FakeAdb::new();
    fake.add_device("dev_perfetto_ok", "device");

    // Device with API 33 and tool available
    let res = check_collector_capabilities(&fake, "dev_perfetto_ok", DeepCollectorType::Perfetto).unwrap();
    // In FakeAdb, getprop returns empty -> api_level 0, which simulates missing tool
    assert!(!res.supported);

    // 4. Trace metadata and budget termination
    let mut fake_simpleperf_trace = Vec::new();
    fake_simpleperf_trace.extend_from_slice(b"PERFILE2"); // Magic header
    fake_simpleperf_trace.extend_from_slice(&[0u8; 1000]);

    let meta = parse_trace_metadata(&fake_simpleperf_trace, &simp_cfg).unwrap();
    assert_eq!(meta.clock_domain, "MONOTONIC_RAW");
    assert_eq!(meta.file_size_bytes, 1008);
    assert!(!meta.budget_exceeded);

    // Budget exceeded
    let mut small_budget_cfg = simp_cfg.clone();
    small_budget_cfg.max_size_bytes = 500;
    let meta_exceeded = parse_trace_metadata(&fake_simpleperf_trace, &small_budget_cfg).unwrap();
    assert!(meta_exceeded.budget_exceeded);

    // 5. Eligibility check: Deep capture cannot be pooled with lightweight profile
    assert!(verify_deep_capture_eligibility("lightweight", true).is_err());
    assert!(verify_deep_capture_eligibility("lightweight", false).is_ok());
    assert!(verify_deep_capture_eligibility("deep_trace", true).is_ok());
}

#[test]
fn test_checkpoint_visual_assertions_and_tolerances() {
    use crate::diag::visual_assertion::*;

    let res = [64u32, 64u32];
    let num_pixels = (res[0] * res[1]) as usize;
    let byte_count = num_pixels * 4;

    // Create synthetic reference image (solid blue)
    let mut ref_pixels = vec![0u8; byte_count];
    for i in 0..num_pixels {
        ref_pixels[i * 4] = 0;       // R
        ref_pixels[i * 4 + 1] = 0;   // G
        ref_pixels[i * 4 + 2] = 200; // B
        ref_pixels[i * 4 + 3] = 255; // A
    }

    let approved_prov = ImageReferenceProvenance {
        reference_id: "ref_verse1_intro".to_string(),
        sha256: "0123456789abcdef".to_string(),
        approved_by: "qa_lead".to_string(),
        approved_timestamp_ms: 1700000000000,
        effective_resolution: res,
        settings_hash: "settings_1080p_default".to_string(),
    };

    let tolerances = ImageToleranceConfig::default();

    // 1. Identical candidate image -> Pass
    let cand_identical = ref_pixels.clone();
    let res_ident = compare_checkpoint_image(
        "chk_intro",
        1,
        &cand_identical,
        res,
        &ref_pixels,
        Some(&approved_prov),
        &tolerances,
        0,
    );
    assert_eq!(res_ident.status, VisualAssertionStatus::Pass);
    assert_eq!(res_ident.diff_fraction, 0.0);
    assert_eq!(res_ident.mean_abs_error, 0.0);
    assert!(!res_ident.physical_display_verified); // Invariant!

    // 2. Corrupt / changed image -> AssertionFailed
    let mut cand_corrupt = ref_pixels.clone();
    // Invert large region (half the screen)
    for i in 0..(num_pixels / 2) {
        cand_corrupt[i * 4] = 255;
        cand_corrupt[i * 4 + 1] = 255;
        cand_corrupt[i * 4 + 2] = 0;
    }
    let res_corrupt = compare_checkpoint_image(
        "chk_intro",
        1,
        &cand_corrupt,
        res,
        &ref_pixels,
        Some(&approved_prov),
        &tolerances,
        0,
    );
    match res_corrupt.status {
        VisualAssertionStatus::AssertionFailed { diff_fraction, .. } => {
            assert!(diff_fraction >= 0.49);
        }
        other => panic!("Expected AssertionFailed, got {:?}", other),
    }

    // 3. Heuristic flash with bright effect within pixel tolerance -> PassWithSuspectedFlashAnomaly
    // A slight brightness jump on a few pixels
    let mut cand_bright = ref_pixels.clone();
    for i in 0..10 {
        cand_bright[i * 4 + 2] = 210; // Within per-channel threshold 16
    }
    let res_flash = compare_checkpoint_image(
        "chk_intro",
        1,
        &cand_bright,
        res,
        &ref_pixels,
        Some(&approved_prov),
        &tolerances,
        2, // 2 heuristic flashes detected by flash detector
    );
    assert_eq!(
        res_flash.status,
        VisualAssertionStatus::PassWithSuspectedFlashAnomaly { flash_count: 2 }
    );
    assert_eq!(res_flash.heuristic_flashes_detected, 2);

    // 4. Masked region exclusion: diff only inside masked area -> Pass
    let mut cand_masked_diff = ref_pixels.clone();
    // Modify top-left 10x10 area (e.g. HUD)
    for y in 0..10 {
        for x in 0..10 {
            let idx = ((y * res[0] + x) * 4) as usize;
            cand_masked_diff[idx] = 255;
            cand_masked_diff[idx + 1] = 255;
            cand_masked_diff[idx + 2] = 255;
        }
    }
    let mut masked_tolerances = tolerances.clone();
    masked_tolerances.masked_regions.push(Rect {
        x: 0,
        y: 0,
        width: 10,
        height: 10,
    });
    let res_masked = compare_checkpoint_image(
        "chk_intro",
        1,
        &cand_masked_diff,
        res,
        &ref_pixels,
        Some(&approved_prov),
        &masked_tolerances,
        0,
    );
    assert_eq!(res_masked.status, VisualAssertionStatus::Pass);
    assert_eq!(res_masked.diff_fraction, 0.0);

    // 5. Mismatched resolution -> SettingsMismatch
    let res_mismatch = compare_checkpoint_image(
        "chk_intro",
        1,
        &cand_identical,
        [1280, 720],
        &ref_pixels,
        Some(&approved_prov),
        &tolerances,
        0,
    );
    match res_mismatch.status {
        VisualAssertionStatus::SettingsMismatch { expected_res, actual_res } => {
            assert_eq!(expected_res, [64, 64]);
            assert_eq!(actual_res, [1280, 720]);
        }
        other => panic!("Expected SettingsMismatch, got {:?}", other),
    }

    // 6. Candidate self-promotion rejected (lacks approved provenance)
    let res_self_promo = compare_checkpoint_image(
        "chk_intro",
        1,
        &cand_identical,
        res,
        &ref_pixels,
        None, // No approved reference
        &tolerances,
        0,
    );
    match res_self_promo.status {
        VisualAssertionStatus::SelfPromotionRejected { ref reason } => {
            assert!(reason.contains("self-promote"));
        }
        other => panic!("Expected SelfPromotionRejected, got {:?}", other),
    }

    // Self-promotion with "self" or "auto" approval rejected
    let mut auto_prov = approved_prov.clone();
    auto_prov.approved_by = "auto".to_string();
    let res_auto = compare_checkpoint_image(
        "chk_intro",
        1,
        &cand_identical,
        res,
        &ref_pixels,
        Some(&auto_prov),
        &tolerances,
        0,
    );
    assert!(matches!(res_auto.status, VisualAssertionStatus::SelfPromotionRejected { .. }));
}

#[test]
fn test_audio_assertion_gate_boundaries() {
    use crate::diag::audio_assertion::*;

    let real_backend = AudioBackendKind::RealDevice {
        device_name: "Android AAudio Speaker".to_string(),
        sample_rate: 48000,
        channels: 2,
    };
    let clock_fallback = AudioBackendKind::ClockFallback {
        reason: "Device open failed / WT_AUDIO_MUTE=1".to_string(),
    };

    let mut cfg = AudioGateConfig::default();
    cfg.require_real_backend = true;
    cfg.max_allowed_underruns = 0;
    cfg.max_allowed_starvations = 0;
    cfg.max_av_drift_ms = 100.0;

    // 1. Guest silent -> FailedGuestSilent
    let res_silent = evaluate_audio_gate(
        0, 0, 0, 0, &real_backend, 48000, Some(1_000_000_000), &cfg,
    );
    assert!(matches!(res_silent.status, AudioGateStatus::FailedGuestSilent { .. }));
    assert!(!res_silent.guest_pcm_passed);
    assert_eq!(res_silent.physical_sound, "not_checked");

    // 2. Guest active + Real backend + 0 underruns + synchronized -> Pass
    // 48000 frames released at 48000Hz = 1.0 second audio time.
    // Video elapsed = 1.01 seconds (10ms drift).
    let res_pass = evaluate_audio_gate(
        48000, 48000, 0, 0, &real_backend, 48000, Some(1_010_000_000), &cfg,
    );
    assert_eq!(res_pass.status, AudioGateStatus::Pass);
    assert!(res_pass.guest_pcm_passed);
    assert!(res_pass.host_backend_passed);
    assert!(res_pass.starvation_passed);
    assert!(res_pass.av_sync_passed);
    assert_eq!(res_pass.physical_sound, "not_checked");
    assert!(res_pass.av_drift_ms.unwrap() <= 15.0);

    // 3. CRITICAL INVARIANT: Guest PCM alone CANNOT pass the output/speaker gate!
    // Even if guest PCM produces 48000 valid frames, clock fallback fails the gate when real backend is required.
    let res_fallback_fail = evaluate_audio_gate(
        48000, 48000, 0, 0, &clock_fallback, 48000, Some(1_000_000_000), &cfg,
    );
    assert!(matches!(res_fallback_fail.status, AudioGateStatus::FailedRealBackendRequired { .. }));
    assert!(res_fallback_fail.guest_pcm_passed); // Guest PCM did produce frames
    assert!(!res_fallback_fail.host_backend_passed); // BUT output gate failed!

    // Clock fallback allowed when require_real_backend = false (for headless / mute tests)
    let mut cfg_no_backend = cfg.clone();
    cfg_no_backend.require_real_backend = false;
    let res_fallback_ok = evaluate_audio_gate(
        48000, 48000, 0, 0, &clock_fallback, 48000, Some(1_000_000_000), &cfg_no_backend,
    );
    assert_eq!(res_fallback_ok.status, AudioGateStatus::Pass);
    assert!(res_fallback_ok.host_backend_passed);

    // 4. Underruns exceeded
    let res_underrun = evaluate_audio_gate(
        48000, 48000, 3, 0, &real_backend, 48000, Some(1_000_000_000), &cfg,
    );
    assert!(matches!(res_underrun.status, AudioGateStatus::FailedUnderrunsExceeded { underruns: 3, .. }));
    assert!(!res_underrun.starvation_passed);

    // 5. Starvation exceeded
    let res_starved = evaluate_audio_gate(
        48000, 48000, 0, 2, &real_backend, 48000, Some(1_000_000_000), &cfg,
    );
    assert!(matches!(res_starved.status, AudioGateStatus::FailedStarvationsExceeded { starvations: 2, .. }));
    assert!(!res_starved.starvation_passed);

    // 6. A/V Drift exceeded: 48000 frames released (1.0s audio) vs 1.3s video (300ms drift > 100ms max)
    let res_drift = evaluate_audio_gate(
        48000, 48000, 0, 0, &real_backend, 48000, Some(1_300_000_000), &cfg,
    );
    match res_drift.status {
        AudioGateStatus::FailedAvDriftExceeded { drift_ms, max_allowed_ms } => {
            assert!(drift_ms >= 299.0);
            assert_eq!(max_allowed_ms, 100.0);
        }
        other => panic!("Expected FailedAvDriftExceeded, got {:?}", other),
    }
    assert!(!res_drift.av_sync_passed);
}

#[test]
fn test_save_progression_and_lifecycle_recovery_assertions() {
    use crate::diag::lifecycle_assertion::*;

    // 1. Valid progression: Save file mutated as expected during game checkpoint progression
    let res_prog = evaluate_save_progression(
        "slot0.sav",
        "hash_checkpoint_0",
        "hash_checkpoint_1",
        "user_orig_hash_123",
        "user_orig_hash_123", // Original user save remained untouched
        true,
        true,
        4096,
    );
    match res_prog.status {
        SaveProgressionStatus::ValidProgression { starting_hash, ending_hash, .. } => {
            assert_eq!(starting_hash, "hash_checkpoint_0");
            assert_eq!(ending_hash, "hash_checkpoint_1");
        }
        other => panic!("Expected ValidProgression, got {:?}", other),
    }
    assert!(res_prog.original_save_untouched);
    assert!(res_prog.save_written);
    assert!(res_prog.reload_successful);

    // 2. Original user save tampered -> Rejection
    let res_tampered = evaluate_save_progression(
        "slot0.sav",
        "hash_checkpoint_0",
        "hash_checkpoint_1",
        "user_orig_hash_123",
        "user_orig_hash_CORRUPTED", // Tampered!
        true,
        true,
        4096,
    );
    assert!(matches!(res_tampered.status, SaveProgressionStatus::OriginalUserSaveTampered { .. }));
    assert!(!res_tampered.original_save_untouched);

    // 3. Save write failed
    let res_write_fail = evaluate_save_progression(
        "slot0.sav",
        "hash_0",
        "hash_0",
        "orig_hash",
        "orig_hash",
        false, // Write failed
        true,
        0,
    );
    assert!(matches!(res_write_fail.status, SaveProgressionStatus::WriteFailed { .. }));
    assert!(!res_write_fail.save_written);

    // 4. Save reload corrupted
    let res_reload_fail = evaluate_save_progression(
        "slot0.sav",
        "hash_0",
        "hash_1",
        "orig_hash",
        "orig_hash",
        true,
        false, // Reload corrupted
        4096,
    );
    assert!(matches!(res_reload_fail.status, SaveProgressionStatus::ReloadCorrupted { .. }));
    assert!(!res_reload_fail.reload_successful);

    // 5. Lifecycle surface loss & recovery
    let res_rec = evaluate_lifecycle_recovery(1, 2, true, true);
    assert_eq!(res_rec.status, LifecycleRecoveryStatus::RecoveredCleanly);
    assert!(res_rec.surface_loss_observed);
    assert!(res_rec.surface_recovered);
    assert_eq!(res_rec.starting_surface_generation, 1);
    assert_eq!(res_rec.ending_surface_generation, 2);
    // Invariant: Modeled WSI is strictly NEVER labeled real physical fold validation
    assert!(!res_rec.physical_fold_validated);
    assert!(res_rec.modeled_wsi);

    // 6. Surface loss without frame recovery -> SurfaceRecoveryFailed
    let res_rec_fail = evaluate_lifecycle_recovery(1, 2, false, true);
    match res_rec_fail.status {
        LifecycleRecoveryStatus::SurfaceRecoveryFailed { starting_gen, ending_gen } => {
            assert_eq!(starting_gen, 1);
            assert_eq!(ending_gen, 2);
        }
        other => panic!("Expected SurfaceRecoveryFailed, got {:?}", other),
    }
    assert!(!res_rec_fail.surface_recovered);
}

#[test]
fn test_paired_run_comparison_policy_and_uncertainty() {
    use crate::diag::comparison_policy::*;

    let mut policy = PairedRunPolicy::default();
    policy.min_pairs = 3;
    policy.practical_margin_pct = 2.0;
    policy.equivalence_threshold_pct = 1.5;
    policy.is_frozen = true;

    // 1. Policy not frozen -> PolicyNotFrozen
    let mut unfrozen_policy = policy.clone();
    unfrozen_policy.is_frozen = false;
    let res_unfrozen = evaluate_paired_runs(&unfrozen_policy, &[], false);
    assert!(matches!(res_unfrozen.classification, PairedClassification::PolicyNotFrozen { .. }));

    // 2. Insufficient pairs (< 3)
    let p1 = PairedRunObservation {
        pair_index: 0,
        baseline_run_id: "base_0".to_string(),
        candidate_run_id: "cand_0".to_string(),
        baseline_median_ms: 16.6,
        candidate_median_ms: 16.0,
        baseline_p99_ms: 22.0,
        candidate_p99_ms: 21.0,
        baseline_gpu_ms: Some(10.0),
        candidate_gpu_ms: Some(9.8),
    };
    let res_insuf = evaluate_paired_runs(&policy, &[p1.clone()], false);
    assert!(matches!(res_insuf.classification, PairedClassification::InsufficientPairs { actual_pairs: 1, min_required: 3 }));

    // 3. Missing required GPU metric in candidate
    let mut p_missing_gpu = p1.clone();
    p_missing_gpu.candidate_gpu_ms = None;
    let res_missing = evaluate_paired_runs(&policy, &[p1.clone(), p1.clone(), p_missing_gpu], false);
    assert!(matches!(res_missing.classification, PairedClassification::MissingRequiredMetric { .. }));

    // 4. A/A Noise / Equivalence: 3 pairs of identical runs with slight sub-1% jitter
    let aa_pairs = vec![
        PairedRunObservation {
            pair_index: 0,
            baseline_run_id: "base_0".to_string(),
            candidate_run_id: "cand_0".to_string(),
            baseline_median_ms: 16.60,
            candidate_median_ms: 16.65, // +0.3%
            baseline_p99_ms: 22.00,
            candidate_p99_ms: 22.05,
            baseline_gpu_ms: Some(10.0),
            candidate_gpu_ms: Some(10.0),
        },
        PairedRunObservation {
            pair_index: 1,
            baseline_run_id: "base_1".to_string(),
            candidate_run_id: "cand_1".to_string(),
            baseline_median_ms: 16.62,
            candidate_median_ms: 16.60, // -0.1%
            baseline_p99_ms: 22.10,
            candidate_p99_ms: 22.08,
            baseline_gpu_ms: Some(10.0),
            candidate_gpu_ms: Some(10.0),
        },
        PairedRunObservation {
            pair_index: 2,
            baseline_run_id: "base_2".to_string(),
            candidate_run_id: "cand_2".to_string(),
            baseline_median_ms: 16.61,
            candidate_median_ms: 16.62, // +0.06%
            baseline_p99_ms: 22.05,
            candidate_p99_ms: 22.04,
            baseline_gpu_ms: Some(10.0),
            candidate_gpu_ms: Some(10.0),
        },
    ];
    let res_aa = evaluate_paired_runs(&policy, &aa_pairs, false);
    assert!(matches!(res_aa.classification, PairedClassification::Equivalence { .. }));

    // 5. Reproducible Gain: Candidate is consistently 5% faster median and tail
    let gain_pairs = vec![
        PairedRunObservation {
            pair_index: 0,
            baseline_run_id: "base_0".to_string(),
            candidate_run_id: "cand_0".to_string(),
            baseline_median_ms: 16.60,
            candidate_median_ms: 15.77, // -5%
            baseline_p99_ms: 22.00,
            candidate_p99_ms: 20.90, // -5%
            baseline_gpu_ms: Some(10.0),
            candidate_gpu_ms: Some(9.5),
        },
        PairedRunObservation {
            pair_index: 1,
            baseline_run_id: "base_1".to_string(),
            candidate_run_id: "cand_1".to_string(),
            baseline_median_ms: 16.60,
            candidate_median_ms: 15.75, // -5.1%
            baseline_p99_ms: 22.00,
            candidate_p99_ms: 20.85,
            baseline_gpu_ms: Some(10.0),
            candidate_gpu_ms: Some(9.5),
        },
        PairedRunObservation {
            pair_index: 2,
            baseline_run_id: "base_2".to_string(),
            candidate_run_id: "cand_2".to_string(),
            baseline_median_ms: 16.60,
            candidate_median_ms: 15.80, // -4.8%
            baseline_p99_ms: 22.00,
            candidate_p99_ms: 20.92,
            baseline_gpu_ms: Some(10.0),
            candidate_gpu_ms: Some(9.5),
        },
    ];
    let res_gain = evaluate_paired_runs(&policy, &gain_pairs, false);
    match res_gain.classification {
        PairedClassification::Improvement { median_gain_pct, tail_gain_pct } => {
            assert!(median_gain_pct >= 4.8);
            assert!(tail_gain_pct >= 4.8);
        }
        other => panic!("Expected Improvement, got {:?}", other),
    }

    // 6. Median / Tail Trade-Off: Median improves by 4%, but p99 tail worsens by 6%
    let tradeoff_pairs = vec![
        PairedRunObservation {
            pair_index: 0,
            baseline_run_id: "base_0".to_string(),
            candidate_run_id: "cand_0".to_string(),
            baseline_median_ms: 16.60,
            candidate_median_ms: 15.93, // -4%
            baseline_p99_ms: 22.00,
            candidate_p99_ms: 23.32, // +6%
            baseline_gpu_ms: Some(10.0),
            candidate_gpu_ms: Some(9.6),
        },
        PairedRunObservation {
            pair_index: 1,
            baseline_run_id: "base_1".to_string(),
            candidate_run_id: "cand_1".to_string(),
            baseline_median_ms: 16.60,
            candidate_median_ms: 15.90,
            baseline_p99_ms: 22.00,
            candidate_p99_ms: 23.35,
            baseline_gpu_ms: Some(10.0),
            candidate_gpu_ms: Some(9.6),
        },
        PairedRunObservation {
            pair_index: 2,
            baseline_run_id: "base_2".to_string(),
            candidate_run_id: "cand_2".to_string(),
            baseline_median_ms: 16.60,
            candidate_median_ms: 15.95,
            baseline_p99_ms: 22.00,
            candidate_p99_ms: 23.30,
            baseline_gpu_ms: Some(10.0),
            candidate_gpu_ms: Some(9.6),
        },
    ];
    let res_tradeoff = evaluate_paired_runs(&policy, &tradeoff_pairs, false);
    match res_tradeoff.classification {
        PairedClassification::TradeOff { median_delta_pct, tail_delta_pct } => {
            assert!(median_delta_pct <= -3.8);
            assert!(tail_delta_pct >= 5.8);
        }
        other => panic!("Expected TradeOff, got {:?}", other),
    }

    // 7. Determinism: identical inputs produce bitwise identical outputs
    let res_gain2 = evaluate_paired_runs(&policy, &gain_pairs, false);
    assert_eq!(res_gain, res_gain2);
}

#[test]
fn test_thermal_eligibility_and_sustained_trajectory() {
    use crate::diag::thermal_sustained::*;

    // 1. Matched starting thermal headroom -> Eligible
    let elig_ok = check_thermal_eligibility(Some(0.85), Some(0.82), 0.10);
    assert!(elig_ok.is_eligible);
    assert!(elig_ok.reason.is_none());

    // 2. Mismatched starting thermal headroom (e.g. cold 0.90 vs already hot 0.60) -> Ineligible
    let elig_mismatch = check_thermal_eligibility(Some(0.90), Some(0.60), 0.10);
    assert!(!elig_mismatch.is_eligible);
    assert!(elig_mismatch.reason.unwrap().contains("headroom mismatch"));

    // 3. Unsupported thermal data -> Eligible with unverified note
    let elig_unsupported = check_thermal_eligibility(None, None, 0.10);
    assert!(elig_unsupported.is_eligible);
    assert!(elig_unsupported.reason.unwrap().contains("unsupported"));

    // 4. Sustained-play trajectory with excluded loading interval
    let baseline_windows = vec![
        TrajectoryWindow {
            window_index: 0,
            start_sec: 0.0,
            end_sec: 60.0,
            median_fps: 35.0, // Initial asset loading window
            p99_frame_time_ms: 35.0,
            mean_thermal_headroom: Some(0.90),
            is_excluded: true, // Excluded from steady-state
        },
        TrajectoryWindow {
            window_index: 1,
            start_sec: 60.0,
            end_sec: 120.0,
            median_fps: 59.8,
            p99_frame_time_ms: 18.0,
            mean_thermal_headroom: Some(0.80),
            is_excluded: false,
        },
        TrajectoryWindow {
            window_index: 2,
            start_sec: 120.0,
            end_sec: 180.0,
            median_fps: 59.5,
            p99_frame_time_ms: 18.2,
            mean_thermal_headroom: Some(0.70),
            is_excluded: false,
        },
        TrajectoryWindow {
            window_index: 3,
            start_sec: 180.0,
            end_sec: 240.0,
            median_fps: 59.4,
            p99_frame_time_ms: 18.3,
            mean_thermal_headroom: Some(0.65),
            is_excluded: false,
        },
    ];

    let base_traj = compute_sustained_trajectory(&baseline_windows, 10.0);
    assert_eq!(base_traj.excluded_intervals_count, 1);
    assert_eq!(base_traj.initial_steady_fps, 59.8);
    assert_eq!(base_traj.final_steady_fps, 59.4);
    assert!(!base_traj.throttling_observed);
    assert!(base_traj.time_to_degradation_secs.is_none());

    // 5. Candidate causes early throttling: drops from 60 FPS to 42 FPS at 180s
    let candidate_windows = vec![
        TrajectoryWindow {
            window_index: 0,
            start_sec: 0.0,
            end_sec: 60.0,
            median_fps: 35.0,
            p99_frame_time_ms: 35.0,
            mean_thermal_headroom: Some(0.90),
            is_excluded: true,
        },
        TrajectoryWindow {
            window_index: 1,
            start_sec: 60.0,
            end_sec: 120.0,
            median_fps: 60.0,
            p99_frame_time_ms: 17.5,
            mean_thermal_headroom: Some(0.60), // Rapid thermal loss
            is_excluded: false,
        },
        TrajectoryWindow {
            window_index: 2,
            start_sec: 120.0,
            end_sec: 180.0,
            median_fps: 58.0,
            p99_frame_time_ms: 19.0,
            mean_thermal_headroom: Some(0.20),
            is_excluded: false,
        },
        TrajectoryWindow {
            window_index: 3,
            start_sec: 180.0,
            end_sec: 240.0,
            median_fps: 42.0, // Heavy throttling drop >= 10%
            p99_frame_time_ms: 28.0,
            mean_thermal_headroom: Some(0.05),
            is_excluded: false,
        },
    ];

    let cand_traj = compute_sustained_trajectory(&candidate_windows, 10.0);
    assert!(cand_traj.throttling_observed);
    assert_eq!(cand_traj.time_to_degradation_secs, Some(180.0));

    // 6. Compare trajectories: candidate-caused throttling is retained and flagged
    let verdict = compare_thermal_trajectories(&base_traj, &cand_traj);
    match verdict {
        TrajectoryComparisonVerdict::CandidateCausedThrottling { degradation_sec, late_fps_drop_pct } => {
            assert_eq!(degradation_sec, 180.0);
            assert!(late_fps_drop_pct >= 29.0);
        }
        other => panic!("Expected CandidateCausedThrottling, got {:?}", other),
    }
}

#[test]
fn test_overhead_evaluation_capped_fps_and_budget_checks() {
    use crate::diag::overhead::*;

    // 1. Uncapped run within design overhead budget
    let m_uncapped = OverheadMeasurement {
        recorder_off_median_ms: 14.50,
        recorder_on_median_ms: 14.65, // +0.15 ms overhead (<= 0.25 ms limit)
        recorder_off_cpu_ms: 10.00,
        recorder_on_cpu_ms: 10.12,
        recorder_off_resident_bytes: 120 * 1024 * 1024,
        recorder_on_resident_bytes: 124 * 1024 * 1024, // 4 MiB overhead (<= 16 MiB limit)
        is_capped_fps: false,
        frame_rate_cap_fps: None,
        provenance: "synthetic_harness_v1".to_string(),
        hardware_calibrated: false,
    };

    let assess_uncapped = evaluate_overhead_budget(&m_uncapped, 0.25, 16 * 1024 * 1024);
    assert!(assess_uncapped.within_frame_budget);
    assert!(assess_uncapped.within_memory_budget);
    assert!(assess_uncapped.within_overall_budget);
    assert_eq!(assess_uncapped.hardware_status, "unverified_hardware_overhead");
    assert!((assess_uncapped.frame_overhead_ms - 0.15).abs() < 0.001);
    assert_eq!(assess_uncapped.memory_overhead_bytes, 4 * 1024 * 1024);

    // 2. Capped 60 FPS run (16.66ms both off and on): CPU and memory must accompany frame rate!
    let m_capped = OverheadMeasurement {
        recorder_off_median_ms: 16.666,
        recorder_on_median_ms: 16.666, // 0.0 ms frame delta due to vsync cap
        recorder_off_cpu_ms: 8.50,
        recorder_on_cpu_ms: 8.65, // Genuine CPU time cost is 0.15 ms
        recorder_off_resident_bytes: 150 * 1024 * 1024,
        recorder_on_resident_bytes: 153 * 1024 * 1024, // 3 MiB resident cost
        is_capped_fps: true,
        frame_rate_cap_fps: Some(60.0),
        provenance: "synthetic_vsync_test".to_string(),
        hardware_calibrated: false,
    };

    let assess_capped = evaluate_overhead_budget(&m_capped, 0.25, 16 * 1024 * 1024);
    assert!(assess_capped.is_capped_fps);
    assert_eq!(assess_capped.frame_overhead_ms, 0.0);
    // Verified: CPU and memory are present and not zero
    assert!((assess_capped.cpu_overhead_ms - 0.15).abs() < 0.001);
    assert_eq!(assess_capped.memory_overhead_bytes, 3 * 1024 * 1024);
    assert!(assess_capped.within_overall_budget);

    // 3. Exceeded memory budget
    let m_heavy_mem = OverheadMeasurement {
        recorder_off_median_ms: 14.50,
        recorder_on_median_ms: 14.60,
        recorder_off_cpu_ms: 10.00,
        recorder_on_cpu_ms: 10.10,
        recorder_off_resident_bytes: 100 * 1024 * 1024,
        recorder_on_resident_bytes: 125 * 1024 * 1024, // 25 MiB > 16 MiB
        is_capped_fps: false,
        frame_rate_cap_fps: None,
        provenance: "heavy_capture_test".to_string(),
        hardware_calibrated: true,
    };

    let assess_heavy = evaluate_overhead_budget(&m_heavy_mem, 0.25, 16 * 1024 * 1024);
    assert!(assess_heavy.within_frame_budget);
    assert!(!assess_heavy.within_memory_budget);
    assert!(!assess_heavy.within_overall_budget);
    assert_eq!(assess_heavy.hardware_status, "device_calibrated");
}
