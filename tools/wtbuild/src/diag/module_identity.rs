use std::fs;
use std::path::Path;

use sha2::{Digest, Sha256};

use crate::diag::models::DiagnosticBuildIdentity;

#[derive(Debug, Clone, PartialEq)]
pub enum ModuleIdentityError {
    MissingLibrary(String),
    StandinGameRefused(String),
    MismatchedArchitecture { lib: String, details: String },
    StaleLibrary { lib: String, details: String },
    IoError(String),
}

impl std::fmt::Display for ModuleIdentityError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::MissingLibrary(name) => write!(f, "Missing required library: {}", name),
            Self::StandinGameRefused(msg) => write!(f, "Stand-in game library refused: {}", msg),
            Self::MismatchedArchitecture { lib, details } => {
                write!(f, "Mismatched architecture for {}: {}", lib, details)
            }
            Self::StaleLibrary { lib, details } => {
                write!(f, "Stale library detected for {}: {}", lib, details)
            }
            Self::IoError(msg) => write!(f, "I/O error during module verification: {}", msg),
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum InstallVerificationResult {
    Success,
    SignatureMismatchNonDestructiveStop(String),
    VersionDowngradeNonDestructiveStop(String),
    InstallationFailed(String),
}

fn file_sha256(path: &Path) -> Result<String, ModuleIdentityError> {
    let data = fs::read(path).map_err(|e| {
        ModuleIdentityError::IoError(format!("Failed to read {}: {}", path.display(), e))
    })?;
    let mut hasher = Sha256::new();
    hasher.update(&data);
    let res = hasher.finalize();
    Ok(res.iter().map(|b| format!("{:02x}", b)).collect())
}

/// Checks if an ELF file is a 64-bit ARM (AArch64) binary.
pub fn verify_elf_arm64(bytes: &[u8]) -> Result<(), String> {
    if bytes.len() < 20 {
        return Err("Binary too small for ELF header".to_string());
    }
    // Magic: 0x7F, 'E', 'L', 'F'
    if &bytes[0..4] != b"\x7fELF" {
        return Err("Not an ELF binary (missing magic)".to_string());
    }
    // Class: 2 = 64-bit
    if bytes[4] != 2 {
        return Err("Not a 64-bit ELF binary".to_string());
    }
    // Machine: e_machine at offset 18 (2 bytes, little-endian): 183 (0x00B7) = EM_AARCH64
    let machine = u16::from_le_bytes([bytes[18], bytes[19]]);
    if machine != 183 {
        return Err(format!(
            "Architecture machine {} is not AArch64 (expected 183)",
            machine
        ));
    }
    Ok(())
}

/// Verifies that a game library is NOT a stand-in stub.
pub fn verify_not_standin_game(data: &[u8]) -> Result<(), ModuleIdentityError> {
    let standin_needle = b"Link-only stand-in";
    if data
        .windows(standin_needle.len())
        .any(|w| w == standin_needle)
    {
        return Err(ModuleIdentityError::StandinGameRefused(
            "libgame.so contains link stand-in marker; refuse to package stand-in game as production build"
                .to_string(),
        ));
    }
    // A real Bayonetta 3 game shim is substantial (> 128 KiB); stand-in stubs are tiny.
    if data.len() < 16384 {
        return Err(ModuleIdentityError::StandinGameRefused(
            format!("libgame.so is suspiciously small ({} bytes); refuse to package stand-in stub", data.len())
        ));
    }
    Ok(())
}

/// Computes module identities and verifies build integrity.
pub fn verify_module_identities(
    package_id: &str,
    build_variant: &str,
    libs_dir: &Path,
    targets_dir: Option<&Path>,
    apk_path: Option<&Path>,
) -> Result<DiagnosticBuildIdentity, ModuleIdentityError> {
    // 1. libwitchtime.so
    let rt_path = libs_dir.join("libwitchtime.so");
    if !rt_path.is_file() {
        return Err(ModuleIdentityError::MissingLibrary(
            "libwitchtime.so".to_string(),
        ));
    }
    let rt_bytes = fs::read(&rt_path).map_err(|e| ModuleIdentityError::IoError(e.to_string()))?;
    verify_elf_arm64(&rt_bytes).map_err(|details| ModuleIdentityError::MismatchedArchitecture {
        lib: "libwitchtime.so".to_string(),
        details,
    })?;
    let runtime_lib_hash = Some(file_sha256(&rt_path)?);

    // 2. libgame.so
    let game_path = libs_dir.join("libgame.so");
    if !game_path.is_file() {
        return Err(ModuleIdentityError::MissingLibrary("libgame.so".to_string()));
    }
    let game_bytes =
        fs::read(&game_path).map_err(|e| ModuleIdentityError::IoError(e.to_string()))?;
    verify_elf_arm64(&game_bytes).map_err(|details| {
        ModuleIdentityError::MismatchedArchitecture {
            lib: "libgame.so".to_string(),
            details,
        }
    })?;
    verify_not_standin_game(&game_bytes)?;
    let game_shim_hash = Some(file_sha256(&game_path)?);

    // 3. Targets (main.elf / sdk.elf)
    let mut main_executable_hash = None;
    let mut generated_sdk_identity = None;
    if let Some(t_dir) = targets_dir {
        let exefs = t_dir.join("exefs");
        let main_elf = exefs.join("main.elf");
        if main_elf.is_file() {
            main_executable_hash = Some(file_sha256(&main_elf)?);
        }
        let sdk_elf = exefs.join("sdk.elf");
        if sdk_elf.is_file() {
            generated_sdk_identity = Some(file_sha256(&sdk_elf)?);
        }
    }

    // 4. APK hash
    let mut apk_hash = None;
    if let Some(apk) = apk_path {
        if apk.is_file() {
            apk_hash = Some(file_sha256(apk)?);
        }
    }

    Ok(DiagnosticBuildIdentity {
        package_id: package_id.to_string(),
        apk_hash,
        runtime_lib_hash,
        game_shim_hash,
        main_executable_hash,
        generated_sdk_identity,
        build_variant: build_variant.to_string(),
    })
}

/// Evaluates adb install output to catch signature or package incompatibilities without destructive uninstalls.
pub fn verify_install_output(adb_output: &str) -> InstallVerificationResult {
    if adb_output.contains("Success") {
        return InstallVerificationResult::Success;
    }
    if adb_output.contains("INSTALL_FAILED_UPDATE_INCOMPATIBLE")
        || adb_output.contains("signatures do not match")
        || adb_output.contains("INSTALL_FAILED_SHARED_USER_INCOMPATIBLE")
    {
        return InstallVerificationResult::SignatureMismatchNonDestructiveStop(
            "Signing certificate mismatch with currently installed package. Stopping non-destructively; never uninstalling or clearing application data."
                .to_string(),
        );
    }
    if adb_output.contains("INSTALL_FAILED_VERSION_DOWNGRADE") {
        return InstallVerificationResult::VersionDowngradeNonDestructiveStop(
            "Cannot downgrade installed version. Stopping non-destructively.".to_string(),
        );
    }
    InstallVerificationResult::InstallationFailed(adb_output.trim().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_fake_arm64_elf(size: usize, extra_marker: Option<&[u8]>) -> Vec<u8> {
        let mut bytes = vec![0u8; size];
        // ELF header
        bytes[0..4].copy_from_slice(b"\x7fELF");
        bytes[4] = 2; // 64-bit
        bytes[5] = 1; // little endian
        bytes[6] = 1; // ELF version
                      // e_machine = 183 (0x00B7) at bytes 18..20
        bytes[18] = 0xb7;
        bytes[19] = 0x00;

        if let Some(marker) = extra_marker {
            if marker.len() + 32 < size {
                bytes[32..32 + marker.len()].copy_from_slice(marker);
            }
        }
        bytes
    }

    #[test]
    fn test_standin_game_library_refused() {
        let temp_dir = std::env::temp_dir().join(format!("diag_mod_standin_{}", std::process::id()));
        let _ = fs::remove_dir_all(&temp_dir);
        fs::create_dir_all(&temp_dir).unwrap();

        let rt_elf = make_fake_arm64_elf(65536, None);
        fs::write(temp_dir.join("libwitchtime.so"), rt_elf).unwrap();

        // Stand-in game with marker
        let standin_game = make_fake_arm64_elf(32768, Some(b"Link-only stand-in"));
        fs::write(temp_dir.join("libgame.so"), standin_game).unwrap();

        let err = verify_module_identities("org.witchtime.android", "diagnostic", &temp_dir, None, None).unwrap_err();
        match err {
            ModuleIdentityError::StandinGameRefused(msg) => {
                assert!(msg.contains("refuse to package stand-in"));
            }
            other => panic!("Expected StandinGameRefused, got {:?}", other),
        }

        let _ = fs::remove_dir_all(&temp_dir);
    }

    #[test]
    fn test_mixed_or_stale_libraries_rejected() {
        let temp_dir = std::env::temp_dir().join(format!("diag_mod_arch_{}", std::process::id()));
        let _ = fs::remove_dir_all(&temp_dir);
        fs::create_dir_all(&temp_dir).unwrap();

        // libwitchtime.so is x86_64 (e_machine = 62 = 0x3E)
        let mut x86_elf = make_fake_arm64_elf(65536, None);
        x86_elf[18] = 0x3e;
        x86_elf[19] = 0x00;
        fs::write(temp_dir.join("libwitchtime.so"), x86_elf).unwrap();

        let game_elf = make_fake_arm64_elf(65536, None);
        fs::write(temp_dir.join("libgame.so"), game_elf).unwrap();

        let err = verify_module_identities("org.witchtime.android", "diagnostic", &temp_dir, None, None).unwrap_err();
        match err {
            ModuleIdentityError::MismatchedArchitecture { lib, details } => {
                assert_eq!(lib, "libwitchtime.so");
                assert!(details.contains("not AArch64"));
            }
            other => panic!("Expected MismatchedArchitecture, got {:?}", other),
        }

        let _ = fs::remove_dir_all(&temp_dir);
    }

    #[test]
    fn test_valid_module_identity_emission() {
        let temp_dir = std::env::temp_dir().join(format!("diag_mod_valid_{}", std::process::id()));
        let _ = fs::remove_dir_all(&temp_dir);
        let libs_dir = temp_dir.join("libs");
        let targets_dir = temp_dir.join("targets");
        fs::create_dir_all(&libs_dir).unwrap();
        fs::create_dir_all(targets_dir.join("exefs")).unwrap();

        let rt_elf = make_fake_arm64_elf(65536, None);
        fs::write(libs_dir.join("libwitchtime.so"), rt_elf).unwrap();

        let game_elf = make_fake_arm64_elf(131072, None);
        fs::write(libs_dir.join("libgame.so"), game_elf).unwrap();

        fs::write(targets_dir.join("exefs/main.elf"), b"MAIN_ELF_GAME_DUMP").unwrap();
        fs::write(targets_dir.join("exefs/sdk.elf"), b"SDK_ELF_DATA").unwrap();

        let identity = verify_module_identities(
            "org.witchtime.android",
            "diagnostic",
            &libs_dir,
            Some(&targets_dir),
            None,
        ).unwrap();

        assert_eq!(identity.package_id, "org.witchtime.android");
        assert_eq!(identity.build_variant, "diagnostic");
        assert!(identity.runtime_lib_hash.is_some());
        assert!(identity.game_shim_hash.is_some());
        assert!(identity.main_executable_hash.is_some());
        assert!(identity.generated_sdk_identity.is_some());

        let _ = fs::remove_dir_all(&temp_dir);
    }

    #[test]
    fn test_signing_mismatch_reports_nondestructive_stop_no_uninstall() {
        let error_out = "Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: Existing package's signatures do not match]";
        let res = verify_install_output(error_out);
        match res {
            InstallVerificationResult::SignatureMismatchNonDestructiveStop(msg) => {
                assert!(msg.contains("Stopping non-destructively"));
                assert!(msg.contains("never uninstalling"));
            }
            other => panic!("Expected SignatureMismatchNonDestructiveStop, got {:?}", other),
        }

        let success_out = "Performing Streamed Install\nSuccess\n";
        assert_eq!(verify_install_output(success_out), InstallVerificationResult::Success);
    }
}
