use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum SaveProgressionStatus {
    ValidProgression {
        save_file: String,
        starting_hash: String,
        ending_hash: String,
    },
    ProgressionUnchanged {
        save_file: String,
        hash: String,
    },
    WriteFailed {
        reason: String,
    },
    ReloadCorrupted {
        reason: String,
    },
    OriginalUserSaveTampered {
        original_expected_hash: String,
        original_actual_hash: String,
    },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SaveProgressionResult {
    pub status: SaveProgressionStatus,
    pub save_written: bool,
    pub reload_successful: bool,
    pub original_save_untouched: bool,
    pub bytes_written: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum LifecycleRecoveryStatus {
    RecoveredCleanly,
    SurfaceRecoveryFailed {
        starting_gen: u32,
        ending_gen: u32,
    },
    UnexpectedBackgroundStall,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LifecycleRecoveryResult {
    pub status: LifecycleRecoveryStatus,
    pub surface_loss_observed: bool,
    pub surface_recovered: bool,
    pub starting_surface_generation: u32,
    pub ending_surface_generation: u32,
    /// Must remain false for all modeled/synthetic tests
    pub physical_fold_validated: bool,
    pub modeled_wsi: bool,
}

/// Evaluates isolated-save writes, reload integrity, and original save protection.
pub fn evaluate_save_progression(
    save_file_name: &str,
    starting_session_hash: &str,
    ending_session_hash: &str,
    original_save_start_hash: &str,
    original_save_end_hash: &str,
    write_succeeded: bool,
    reload_succeeded: bool,
    bytes_written: u64,
) -> SaveProgressionResult {
    // 1. Verify original user save remained 100% untouched
    if original_save_start_hash != original_save_end_hash {
        return SaveProgressionResult {
            status: SaveProgressionStatus::OriginalUserSaveTampered {
                original_expected_hash: original_save_start_hash.to_string(),
                original_actual_hash: original_save_end_hash.to_string(),
            },
            save_written: write_succeeded,
            reload_successful: reload_succeeded,
            original_save_untouched: false,
            bytes_written,
        };
    }

    // 2. Check write failure
    if !write_succeeded {
        return SaveProgressionResult {
            status: SaveProgressionStatus::WriteFailed {
                reason: "Save write operation failed or was refused".to_string(),
            },
            save_written: false,
            reload_successful: false,
            original_save_untouched: true,
            bytes_written,
        };
    }

    // 3. Check reload corruption
    if !reload_succeeded {
        return SaveProgressionResult {
            status: SaveProgressionStatus::ReloadCorrupted {
                reason: "Save file reload or checksum verification failed after write".to_string(),
            },
            save_written: true,
            reload_successful: false,
            original_save_untouched: true,
            bytes_written,
        };
    }

    // 4. Invariant: Byte difference from starting save is NOT automatically failure!
    // Game progression intentionally mutates the session save file.
    let status = if starting_session_hash != ending_session_hash {
        SaveProgressionStatus::ValidProgression {
            save_file: save_file_name.to_string(),
            starting_hash: starting_session_hash.to_string(),
            ending_hash: ending_session_hash.to_string(),
        }
    } else {
        SaveProgressionStatus::ProgressionUnchanged {
            save_file: save_file_name.to_string(),
            hash: starting_session_hash.to_string(),
        }
    };

    SaveProgressionResult {
        status,
        save_written: true,
        reload_successful: true,
        original_save_untouched: true,
        bytes_written,
    }
}

/// Evaluates WSI surface loss, recreation, and lifecycle recovery.
pub fn evaluate_lifecycle_recovery(
    starting_gen: u32,
    ending_gen: u32,
    frames_resumed: bool,
    is_modeled_wsi: bool,
) -> LifecycleRecoveryResult {
    let surface_loss_observed = ending_gen > starting_gen;
    let surface_recovered = surface_loss_observed && frames_resumed;

    let status = if surface_loss_observed && !frames_resumed {
        LifecycleRecoveryStatus::SurfaceRecoveryFailed {
            starting_gen,
            ending_gen,
        }
    } else {
        LifecycleRecoveryStatus::RecoveredCleanly
    };

    LifecycleRecoveryResult {
        status,
        surface_loss_observed,
        surface_recovered,
        starting_surface_generation: starting_gen,
        ending_surface_generation: ending_gen,
        // Invariant: Modeled WSI is NEVER labeled physical fold validation!
        physical_fold_validated: false,
        modeled_wsi: is_modeled_wsi,
    }
}
