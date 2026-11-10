use crate::diag::models::{
    reasons, ComparisonEligibility, DiagnosticManifest, DiagnosticOutcome, DiagnosticRequest,
};

/// Evaluates whether two diagnostic sessions are eligible for a valid, matched A/B comparison.
///
/// Compares effective configuration, device and driver versions, scenario/save/checkpoint coverage,
/// diagnostic profile and version, cache class, and output modes.
/// Intentionally different candidate binaries (e.g. commit, APK hash, runtime library hash)
/// are explicitly permitted when all execution conditions match.
pub fn evaluate_comparison_eligibility(
    baseline_req: Option<&DiagnosticRequest>,
    baseline_man: &DiagnosticManifest,
    baseline_out: &DiagnosticOutcome,
    candidate_req: Option<&DiagnosticRequest>,
    candidate_man: &DiagnosticManifest,
    candidate_out: &DiagnosticOutcome,
) -> ComparisonEligibility {
    let mut reasons = Vec::new();

    // 1. Execution outcomes: both runs must have completed cleanly without crash or timeout
    if baseline_out.execution_outcome != "completed" || !baseline_out.exit_status.clean_exit {
        reasons.push(reasons::FAILED_BASELINE.to_string());
    }
    if candidate_out.execution_outcome != "completed" || !candidate_out.exit_status.clean_exit {
        reasons.push(reasons::FAILED_CANDIDATE.to_string());
    }

    // 2. Acknowledgement: neither run may have unacknowledged settings or requested/effective mismatches
    if !baseline_man.mismatches.is_empty() || !candidate_man.mismatches.is_empty() {
        reasons.push(reasons::MISSING_ACKNOWLEDGEMENT.to_string());
    }
    if let (Some(req), man) = (baseline_req, baseline_man) {
        if req.configuration.resolution != man.effective_settings.resolution
            || (req.configuration.fps_target - man.effective_settings.fps_target).abs() > 0.01
            || req.configuration.audio_mode != man.effective_settings.audio_mode
        {
            if !reasons.contains(&reasons::MISSING_ACKNOWLEDGEMENT.to_string()) {
                reasons.push(reasons::MISSING_ACKNOWLEDGEMENT.to_string());
            }
        }
        if let Some(req_t) = &req.treatments {
            match &man.effective_treatments {
                Some(eff_t) if req_t != eff_t => {
                    if !reasons.contains(&reasons::MISSING_ACKNOWLEDGEMENT.to_string()) {
                        reasons.push(reasons::MISSING_ACKNOWLEDGEMENT.to_string());
                    }
                }
                None => {
                    if !reasons.contains(&reasons::MISSING_ACKNOWLEDGEMENT.to_string()) {
                        reasons.push(reasons::MISSING_ACKNOWLEDGEMENT.to_string());
                    }
                }
                _ => {}
            }
        }
    }
    if let (Some(req), man) = (candidate_req, candidate_man) {
        if req.configuration.resolution != man.effective_settings.resolution
            || (req.configuration.fps_target - man.effective_settings.fps_target).abs() > 0.01
            || req.configuration.audio_mode != man.effective_settings.audio_mode
        {
            if !reasons.contains(&reasons::MISSING_ACKNOWLEDGEMENT.to_string()) {
                reasons.push(reasons::MISSING_ACKNOWLEDGEMENT.to_string());
            }
        }
        if let Some(req_t) = &req.treatments {
            match &man.effective_treatments {
                Some(eff_t) if req_t != eff_t => {
                    if !reasons.contains(&reasons::MISSING_ACKNOWLEDGEMENT.to_string()) {
                        reasons.push(reasons::MISSING_ACKNOWLEDGEMENT.to_string());
                    }
                }
                None => {
                    if !reasons.contains(&reasons::MISSING_ACKNOWLEDGEMENT.to_string()) {
                        reasons.push(reasons::MISSING_ACKNOWLEDGEMENT.to_string());
                    }
                }
                _ => {}
            }
        }
    }

    // 3. Scenario & starting save matching
    if baseline_man.fixtures.scenario_hash != candidate_man.fixtures.scenario_hash
        || baseline_man.fixtures.save_hash != candidate_man.fixtures.save_hash
        || baseline_man.fixtures.save_fixture_id != candidate_man.fixtures.save_fixture_id
    {
        reasons.push(reasons::DIFFERENT_SCENARIOS.to_string());
    }

    // 4. Checkpoint & segment coverage
    if baseline_out.covered_segments != candidate_out.covered_segments {
        reasons.push(reasons::PARTIAL_ROUTE.to_string());
    } else {
        // Compare passed checkpoints
        let base_cps: Vec<&str> = baseline_out
            .checkpoints
            .iter()
            .filter(|cp| cp.status == "passed")
            .map(|cp| cp.checkpoint_id.as_str())
            .collect();
        let cand_cps: Vec<&str> = candidate_out
            .checkpoints
            .iter()
            .filter(|cp| cp.status == "passed")
            .map(|cp| cp.checkpoint_id.as_str())
            .collect();
        if base_cps != cand_cps {
            reasons.push(reasons::PARTIAL_ROUTE.to_string());
        }
    }

    // 5. Effective graphics, resolution, and pacing configuration
    if baseline_man.effective_settings.resolution != candidate_man.effective_settings.resolution {
        reasons.push(reasons::MISMATCHED_RESOLUTION.to_string());
    }
    if (baseline_man.effective_settings.fps_target - candidate_man.effective_settings.fps_target).abs() > 0.01
        || baseline_man.effective_settings.audio_mode != candidate_man.effective_settings.audio_mode
    {
        reasons.push(reasons::POLICY_MISMATCH.to_string());
    }

    // 6. Input source and controller mapping policies
    if baseline_man.effective_settings.input_source_policy != candidate_man.effective_settings.input_source_policy
        || baseline_man.effective_settings.input_mapping_policy != candidate_man.effective_settings.input_mapping_policy
        || baseline_man.effective_settings.menu_confirm_policy != candidate_man.effective_settings.menu_confirm_policy
    {
        reasons.push(reasons::DIFFERENT_MAPPING_POLICY.to_string());
    }

    // 7. Device, OS, Vulkan driver, and page size
    if baseline_man.device.model != candidate_man.device.model
        || baseline_man.device.api_level != candidate_man.device.api_level
        || baseline_man.device.vulkan_driver != candidate_man.device.vulkan_driver
        || baseline_man.device.page_size != candidate_man.device.page_size
    {
        reasons.push(reasons::DEVICE_DRIVER_MISMATCH.to_string());
    }

    // 8. Diagnostic subsystem version and profiling profile
    if baseline_man.diagnostic_version != candidate_man.diagnostic_version {
        reasons.push(reasons::INCOMPATIBLE_PROFILES.to_string());
    }
    if let (Some(b_req), Some(c_req)) = (baseline_req, candidate_req) {
        if b_req.profile.profile_name != c_req.profile.profile_name
            || b_req.profile.collectors != c_req.profile.collectors
        {
            if !reasons.contains(&reasons::INCOMPATIBLE_PROFILES.to_string()) {
                reasons.push(reasons::INCOMPATIBLE_PROFILES.to_string());
            }
        }
    }

    // 9. Cache class and seed hash
    if baseline_man.cache.cache_class != candidate_man.cache.cache_class
        || baseline_man.cache.seed_hash != candidate_man.cache.seed_hash
    {
        reasons.push(reasons::CACHE_CLASS_MISMATCH.to_string());
    }

    ComparisonEligibility {
        is_eligible: reasons.is_empty(),
        ineligibility_reasons: reasons,
    }
}
