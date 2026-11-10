use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

impl Rect {
    pub fn contains(&self, px: u32, py: u32) -> bool {
        px >= self.x && px < self.x + self.width && py >= self.y && py < self.y + self.height
    }
}

/// Provenance of an approved manual visual reference.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ImageReferenceProvenance {
    pub reference_id: String,
    pub sha256: String,
    pub approved_by: String,
    pub approved_timestamp_ms: u64,
    pub effective_resolution: [u32; 2],
    pub settings_hash: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ImageToleranceConfig {
    pub max_mean_abs_error: f64,
    pub max_mismatched_pixel_fraction: f64,
    pub per_channel_threshold: u8,
    pub temporal_frame_tolerance: usize,
    pub masked_regions: Vec<Rect>,
    pub bright_effect_tolerance: f64,
}

impl Default for ImageToleranceConfig {
    fn default() -> Self {
        Self {
            max_mean_abs_error: 5.0,
            max_mismatched_pixel_fraction: 0.02, // 2%
            per_channel_threshold: 16,
            temporal_frame_tolerance: 2,
            masked_regions: Vec::new(),
            bright_effect_tolerance: 25.0,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum VisualAssertionStatus {
    Pass,
    PassWithSuspectedFlashAnomaly { flash_count: usize },
    AssertionFailed { diff_fraction: f64, mean_abs_error: f64, reason: String },
    SettingsMismatch { expected_res: [u32; 2], actual_res: [u32; 2] },
    SelfPromotionRejected { reason: String },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct VisualAssertionResult {
    pub checkpoint_id: String,
    pub occurrence_id: u32,
    pub status: VisualAssertionStatus,
    pub capture_source: String,
    pub physical_display_verified: bool,
    pub diff_fraction: f64,
    pub mean_abs_error: f64,
    pub heuristic_flashes_detected: usize,
}

/// Compares a captured checkpoint texture against an approved reference image.
pub fn compare_checkpoint_image(
    checkpoint_id: &str,
    occurrence_id: u32,
    candidate_pixels: &[u8],
    candidate_res: [u32; 2],
    reference_pixels: &[u8],
    reference_provenance: Option<&ImageReferenceProvenance>,
    tolerances: &ImageToleranceConfig,
    heuristic_flashes: usize,
) -> VisualAssertionResult {
    // 1. Refuse candidate-baseline self-promotion
    let Some(prov) = reference_provenance else {
        return VisualAssertionResult {
            checkpoint_id: checkpoint_id.to_string(),
            occurrence_id,
            status: VisualAssertionStatus::SelfPromotionRejected {
                reason: "Candidate cannot self-promote unverified render as approved reference"
                    .to_string(),
            },
            capture_source: "vulkan_swapchain_texture".to_string(),
            physical_display_verified: false,
            diff_fraction: 1.0,
            mean_abs_error: 255.0,
            heuristic_flashes_detected: heuristic_flashes,
        };
    };

    if prov.approved_by.trim().is_empty() || prov.approved_by == "self" || prov.approved_by == "auto" {
        return VisualAssertionResult {
            checkpoint_id: checkpoint_id.to_string(),
            occurrence_id,
            status: VisualAssertionStatus::SelfPromotionRejected {
                reason: "Reference lacks verified human/developer approval provenance".to_string(),
            },
            capture_source: "vulkan_swapchain_texture".to_string(),
            physical_display_verified: false,
            diff_fraction: 1.0,
            mean_abs_error: 255.0,
            heuristic_flashes_detected: heuristic_flashes,
        };
    }

    // 2. Validate resolution matching
    if candidate_res != prov.effective_resolution {
        return VisualAssertionResult {
            checkpoint_id: checkpoint_id.to_string(),
            occurrence_id,
            status: VisualAssertionStatus::SettingsMismatch {
                expected_res: prov.effective_resolution,
                actual_res: candidate_res,
            },
            capture_source: "vulkan_swapchain_texture".to_string(),
            physical_display_verified: false,
            diff_fraction: 1.0,
            mean_abs_error: 255.0,
            heuristic_flashes_detected: heuristic_flashes,
        };
    }

    let [w, h] = candidate_res;
    let expected_bytes = (w * h * 4) as usize;
    if candidate_pixels.len() != expected_bytes || reference_pixels.len() != expected_bytes {
        return VisualAssertionResult {
            checkpoint_id: checkpoint_id.to_string(),
            occurrence_id,
            status: VisualAssertionStatus::AssertionFailed {
                diff_fraction: 1.0,
                mean_abs_error: 255.0,
                reason: format!(
                    "Pixel buffer size mismatch: candidate={}, reference={}, expected={}",
                    candidate_pixels.len(),
                    reference_pixels.len(),
                    expected_bytes
                ),
            },
            capture_source: "vulkan_swapchain_texture".to_string(),
            physical_display_verified: false,
            diff_fraction: 1.0,
            mean_abs_error: 255.0,
            heuristic_flashes_detected: heuristic_flashes,
        };
    }

    // 3. Pixel difference with region masking
    let mut total_evaluated_pixels = 0u64;
    let mut mismatched_pixels = 0u64;
    let mut sum_abs_error = 0.0f64;

    for y in 0..h {
        for x in 0..w {
            let is_masked = tolerances.masked_regions.iter().any(|r| r.contains(x, y));
            if is_masked {
                continue;
            }

            total_evaluated_pixels += 1;
            let idx = ((y * w + x) * 4) as usize;
            let cr = candidate_pixels[idx] as i32;
            let cg = candidate_pixels[idx + 1] as i32;
            let cb = candidate_pixels[idx + 2] as i32;

            let rr = reference_pixels[idx] as i32;
            let rg = reference_pixels[idx + 1] as i32;
            let rb = reference_pixels[idx + 2] as i32;

            let dr = (cr - rr).abs();
            let dg = (cg - rg).abs();
            let db = (cb - rb).abs();

            let max_channel_diff = dr.max(dg).max(db);
            let pixel_mae = (dr + dg + db) as f64 / 3.0;
            sum_abs_error += pixel_mae;

            if max_channel_diff > tolerances.per_channel_threshold as i32 {
                mismatched_pixels += 1;
            }
        }
    }

    if total_evaluated_pixels == 0 {
        total_evaluated_pixels = 1;
    }

    let diff_fraction = mismatched_pixels as f64 / total_evaluated_pixels as f64;
    let mean_abs_error = sum_abs_error / total_evaluated_pixels as f64;

    // 4. Assertion evaluation
    let status = if diff_fraction > tolerances.max_mismatched_pixel_fraction
        || mean_abs_error > tolerances.max_mean_abs_error
    {
        VisualAssertionStatus::AssertionFailed {
            diff_fraction,
            mean_abs_error,
            reason: format!(
                "Render mismatch: diff_fraction={:.4} (max {:.4}), MAE={:.2} (max {:.2})",
                diff_fraction, tolerances.max_mismatched_pixel_fraction, mean_abs_error, tolerances.max_mean_abs_error
            ),
        }
    } else if heuristic_flashes > 0 {
        // Heuristic flash detected but within assertion tolerances -> remains suspected anomaly
        VisualAssertionStatus::PassWithSuspectedFlashAnomaly {
            flash_count: heuristic_flashes,
        }
    } else {
        VisualAssertionStatus::Pass
    };

    VisualAssertionResult {
        checkpoint_id: checkpoint_id.to_string(),
        occurrence_id,
        status,
        capture_source: "vulkan_swapchain_texture".to_string(),
        physical_display_verified: false, // Invariant: texture capture does not claim physical display
        diff_fraction,
        mean_abs_error,
        heuristic_flashes_detected: heuristic_flashes,
    }
}
