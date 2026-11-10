use std::path::PathBuf;
use std::time::{Duration, Instant};

use crate::diag::adb::{AdbDeviceInfo, AdbTransport};
use crate::diag::bootstrap::compute_request_digest;
use crate::diag::models::DiagnosticLiveStatus;
use crate::diag::symbolization::parse_crash_breadcrumb;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ObserverPhase {
    Init,
    Startup,
    ActiveGame,
    BackgroundInactivity,
    LiveStall,
    ConfirmedDeath,
    AdbTransportLoss,
    Completed,
}

#[derive(Debug, Clone, PartialEq)]
pub struct FirstFailureRecord {
    pub phase: ObserverPhase,
    pub reason_code: String,
    pub description: String,
    pub timestamp_ms: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct AndroidRunnerConfig {
    pub serial: Option<String>,
    pub package_id: String,
    pub activity_name: String,
    pub run_id: String,
    pub request_bytes: Vec<u8>,
    pub host_output_dir: PathBuf,
    pub startup_deadline_secs: f64,
    pub scenario_deadline_secs: f64,
    pub recovery_deadline_secs: f64,
    pub max_recovery_attempts: usize,
    pub allow_reconnect_without_relaunch: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub enum RunnerExitStatus {
    Completed,
    StartupTimeout,
    NoFirstFrame,
    ScenarioTimeout,
    LiveStall(String),
    ConfirmedDeath { exit_code: i32, signal: Option<u32> },
    TransportLost,
    ConcurrentRunConflict(String),
    ExistingAppRunningConflict(String),
    AmbiguousDevice(Vec<String>),
    DeviceNotFound(String),
    AcknowledgeMismatch(String),
    CollectorFailure(String),
    Cancelled,
}

#[derive(Debug, Clone, PartialEq)]
pub struct AndroidRunnerResult {
    pub run_id: String,
    pub target_device: String,
    pub target_package: String,
    pub exit_status: RunnerExitStatus,
    pub acknowledged: bool,
    pub local_artifact_dir: PathBuf,
    pub failure_reason: Option<String>,
    pub first_failure: Option<FirstFailureRecord>,
    pub recovery_attempts: usize,
    pub reconnected_without_relaunch: bool,
    pub observed_phases: Vec<ObserverPhase>,
}

pub struct AndroidRunner<'a> {
    pub transport: &'a dyn AdbTransport,
    pub config: AndroidRunnerConfig,
}

impl<'a> AndroidRunner<'a> {
    pub fn new(transport: &'a dyn AdbTransport, config: AndroidRunnerConfig) -> Self {
        Self { transport, config }
    }

    /// Resolves target device serial explicitly or via unambiguous single connected device.
    pub fn resolve_device(&self) -> Result<String, RunnerExitStatus> {
        let devices = self
            .transport
            .list_devices()
            .map_err(|e| RunnerExitStatus::DeviceNotFound(e))?;

        let online_devices: Vec<AdbDeviceInfo> = devices
            .into_iter()
            .filter(|d| d.state == "device")
            .collect();

        if let Some(req_serial) = &self.config.serial {
            if let Some(d) = online_devices.iter().find(|d| &d.serial == req_serial) {
                return Ok(d.serial.clone());
            } else {
                return Err(RunnerExitStatus::DeviceNotFound(format!(
                    "Device '{}' not found or not in 'device' state",
                    req_serial
                )));
            }
        }

        match online_devices.len() {
            0 => Err(RunnerExitStatus::DeviceNotFound("No connected Android devices found".to_string())),
            1 => Ok(online_devices[0].serial.clone()),
            _ => {
                let serials: Vec<String> = online_devices.into_iter().map(|d| d.serial).collect();
                Err(RunnerExitStatus::AmbiguousDevice(serials))
            }
        }
    }

    /// Checks create-new lease and verifies no conflicting interactive app or diagnostic run is active.
    pub fn verify_create_new_lease(&self, serial: &str) -> Result<(), RunnerExitStatus> {
        let pidof_out = self.transport.shell(serial, &["pidof", &self.config.package_id]);
        if let Ok(out) = pidof_out {
            let pids = out.stdout.trim();
            if !pids.is_empty() {
                // An instance is already running!
                // Check if it belongs to an active diagnostic lease
                let lease_path = format!(
                    "/sdcard/Android/data/{}/files/diagnostics/active_lease.json",
                    self.config.package_id
                );
                let lease_out = self.transport.shell(serial, &["cat", &lease_path]);
                if let Ok(lout) = lease_out {
                    if lout.exit_code == 0 && !lout.stdout.is_empty() {
                        return Err(RunnerExitStatus::ConcurrentRunConflict(format!(
                            "Active diagnostic lease already held: {}",
                            lout.stdout.trim()
                        )));
                    }
                }
                // No diagnostic lease found: existing normal interactive app running
                return Err(RunnerExitStatus::ExistingAppRunningConflict(format!(
                    "Package {} is already running (PID {}). Refusing to disrupt existing interactive game session.",
                    self.config.package_id, pids
                )));
            }
        }
        Ok(())
    }

    /// Runs the diagnostic session orchestration.
    pub fn execute(&self) -> AndroidRunnerResult {
        let serial = match self.resolve_device() {
            Ok(s) => s,
            Err(e) => {
                return AndroidRunnerResult {
                    run_id: self.config.run_id.clone(),
                    target_device: self.config.serial.clone().unwrap_or_default(),
                    target_package: self.config.package_id.clone(),
                    exit_status: e,
                    acknowledged: false,
                    local_artifact_dir: self.config.host_output_dir.clone(),
                    failure_reason: Some("Device resolution failed".to_string()),
                    first_failure: None,
                    recovery_attempts: 0,
                    reconnected_without_relaunch: false,
                    observed_phases: vec![ObserverPhase::Init],
                };
            }
        };

        if let Err(e) = self.verify_create_new_lease(&serial) {
            return AndroidRunnerResult {
                run_id: self.config.run_id.clone(),
                target_device: serial,
                target_package: self.config.package_id.clone(),
                exit_status: e,
                acknowledged: false,
                local_artifact_dir: self.config.host_output_dir.clone(),
                failure_reason: Some("Lease conflict".to_string()),
                first_failure: None,
                recovery_attempts: 0,
                reconnected_without_relaunch: false,
                observed_phases: vec![ObserverPhase::Init],
            };
        }

        let remote_diag_root = format!(
            "/sdcard/Android/data/{}/files/diagnostics",
            self.config.package_id
        );
        let remote_session_root = format!("{}/{}", remote_diag_root, self.config.run_id);
        let remote_status = format!("{}/status.json", remote_session_root);
        let remote_breadcrumb = format!("{}/crash_breadcrumb.bin", remote_session_root);

        let mut observed_phases = vec![ObserverPhase::Init, ObserverPhase::Startup];
        let mut first_failure: Option<FirstFailureRecord> = None;
        let mut recovery_attempts: usize = 0;
        let mut reconnected_without_relaunch = false;

        // Stage active lease
        let lease_json = format!(
            "{{\"run_id\":\"{}\",\"package\":\"{}\"}}",
            self.config.run_id, self.config.package_id
        );
        let lease_path = format!("{}/active_lease.json", remote_diag_root);
        let temp_lease = std::env::temp_dir().join(format!("lease_{}.json", self.config.run_id));
        let _ = std::fs::write(&temp_lease, &lease_json);
        let _ = self.transport.push(&serial, &temp_lease, &lease_path);
        let _ = std::fs::remove_file(&temp_lease);

        // Stage request.json
        let temp_req = std::env::temp_dir().join(format!("req_{}.json", self.config.run_id));
        let _ = std::fs::write(&temp_req, &self.config.request_bytes);
        let remote_req_path = format!("{}/request.json", remote_session_root);
        if let Err(e) = self.transport.push(&serial, &temp_req, &remote_req_path) {
            let _ = std::fs::remove_file(&temp_req);
            return AndroidRunnerResult {
                run_id: self.config.run_id.clone(),
                target_device: serial,
                target_package: self.config.package_id.clone(),
                exit_status: RunnerExitStatus::TransportLost,
                acknowledged: false,
                local_artifact_dir: self.config.host_output_dir.clone(),
                failure_reason: Some(format!("Failed to stage request.json: {}", e)),
                first_failure: None,
                recovery_attempts: 0,
                reconnected_without_relaunch: false,
                observed_phases,
            };
        }
        let _ = std::fs::remove_file(&temp_req);

        let expected_digest = compute_request_digest(&self.config.request_bytes);

        // Launch Intent
        let comp = format!("{}/{}", self.config.package_id, self.config.activity_name);
        let start_res = self.transport.shell(
            &serial,
            &[
                "am",
                "start",
                "-n",
                &comp,
                "--es",
                "diag_run_id",
                &self.config.run_id,
            ],
        );

        if let Err(e) = start_res {
            self.cleanup(&serial, &remote_diag_root);
            return AndroidRunnerResult {
                run_id: self.config.run_id.clone(),
                target_device: serial,
                target_package: self.config.package_id.clone(),
                exit_status: RunnerExitStatus::TransportLost,
                acknowledged: false,
                local_artifact_dir: self.config.host_output_dir.clone(),
                failure_reason: Some(format!("Failed to launch Activity: {}", e)),
                first_failure: None,
                recovery_attempts: 0,
                reconnected_without_relaunch: false,
                observed_phases,
            };
        }

        // Watchdog: Await startup acknowledgement and first frame
        let startup_start = Instant::now();
        let startup_deadline = Duration::from_secs_f64(self.config.startup_deadline_secs);
        let mut acknowledged = false;
        let mut first_frame_rendered = false;
        let remote_manifest = format!("{}/manifest.json", remote_session_root);

        while startup_start.elapsed() < startup_deadline {
            let cat_out = self.transport.shell(&serial, &["cat", &remote_manifest]);
            if let Ok(out) = cat_out {
                if out.exit_code == 0 && out.stdout.contains("\"acknowledged\"") {
                    if !out.stdout.contains(&self.config.run_id)
                        || !out.stdout.contains(&expected_digest)
                    {
                        self.cleanup(&serial, &remote_diag_root);
                        return AndroidRunnerResult {
                            run_id: self.config.run_id.clone(),
                            target_device: serial,
                            target_package: self.config.package_id.clone(),
                            exit_status: RunnerExitStatus::AcknowledgeMismatch(
                                "Startup acknowledgement digest or run_id mismatch".to_string(),
                            ),
                            acknowledged: false,
                            local_artifact_dir: self.config.host_output_dir.clone(),
                            failure_reason: Some("Mismatched manifest acknowledgment".to_string()),
                            first_failure: None,
                            recovery_attempts: 0,
                            reconnected_without_relaunch: false,
                            observed_phases,
                        };
                    }
                    acknowledged = true;

                    // Check status.json for surface generation / frame presentation
                    let stat_out = self.transport.shell(&serial, &["cat", &remote_status]);
                    if let Ok(sout) = stat_out {
                        if let Ok(status) = serde_json::from_str::<DiagnosticLiveStatus>(&sout.stdout) {
                            if status.surface_generation > 0 || status.progress.render_age_ms < 5000 {
                                first_frame_rendered = true;
                                break;
                            }
                        } else if sout.exit_code != 0 || sout.stdout.is_empty() {
                            // status.json not created yet or legacy Stage A, treat as rendered
                            first_frame_rendered = true;
                            break;
                        }
                    } else {
                        first_frame_rendered = true;
                        break;
                    }
                }
            }
            std::thread::sleep(Duration::from_millis(50));
        }

        if !acknowledged {
            self.cleanup(&serial, &remote_diag_root);
            return AndroidRunnerResult {
                run_id: self.config.run_id.clone(),
                target_device: serial,
                target_package: self.config.package_id.clone(),
                exit_status: RunnerExitStatus::StartupTimeout,
                acknowledged: false,
                local_artifact_dir: self.config.host_output_dir.clone(),
                failure_reason: Some("Startup acknowledgement deadline exceeded".to_string()),
                first_failure: None,
                recovery_attempts: 0,
                reconnected_without_relaunch: false,
                observed_phases,
            };
        }

        if !first_frame_rendered {
            // Check status.json one final time before deciding NoFirstFrame
            let stat_out = self.transport.shell(&serial, &["cat", &remote_status]);
            if let Ok(sout) = stat_out {
                if let Ok(status) = serde_json::from_str::<DiagnosticLiveStatus>(&sout.stdout) {
                    if status.surface_generation == 0 && status.progress.render_age_ms >= 5000 {
                        self.cleanup(&serial, &remote_diag_root);
                        return AndroidRunnerResult {
                            run_id: self.config.run_id.clone(),
                            target_device: serial,
                            target_package: self.config.package_id.clone(),
                            exit_status: RunnerExitStatus::NoFirstFrame,
                            acknowledged: true,
                            local_artifact_dir: self.config.host_output_dir.clone(),
                            failure_reason: Some(
                                "First frame presentation never occurred before startup deadline (no presentation)".to_string(),
                            ),
                            first_failure: Some(FirstFailureRecord {
                                phase: ObserverPhase::Startup,
                                reason_code: "NO_FIRST_FRAME".to_string(),
                                description: "No presentation occurred".to_string(),
                                timestamp_ms: startup_start.elapsed().as_millis() as u64,
                            }),
                            recovery_attempts: 0,
                            reconnected_without_relaunch: false,
                            observed_phases,
                        };
                    }
                }
            }
        }

        // Scenario execution loop
        let scenario_start = Instant::now();
        let scenario_deadline = Duration::from_secs_f64(self.config.scenario_deadline_secs);
        let remote_outcome = format!("{}/outcome.json", remote_session_root);
        let mut final_status = RunnerExitStatus::ScenarioTimeout;
        let mut transport_lost_time: Option<Instant> = None;
        let mut in_transport_loss = false;

        while scenario_start.elapsed() < scenario_deadline {
            // 1. Check status.json for live status
            let stat_out = self.transport.shell(&serial, &["cat", &remote_status]);
            if let Ok(sout) = stat_out {
                if let Ok(status) = serde_json::from_str::<DiagnosticLiveStatus>(&sout.stdout) {
                    if status.progress.expected_inactivity {
                        if observed_phases.last() != Some(&ObserverPhase::BackgroundInactivity) {
                            observed_phases.push(ObserverPhase::BackgroundInactivity);
                        }
                    } else if status.progress.is_active_stall {
                        if observed_phases.last() != Some(&ObserverPhase::LiveStall) {
                            observed_phases.push(ObserverPhase::LiveStall);
                        }
                        if first_failure.is_none() {
                            first_failure = Some(FirstFailureRecord {
                                phase: ObserverPhase::LiveStall,
                                reason_code: "SCENARIO_TIMEOUT".to_string(),
                                description: "Active stall in foreground".to_string(),
                                timestamp_ms: scenario_start.elapsed().as_millis() as u64,
                            });
                        }
                        if recovery_attempts < self.config.max_recovery_attempts {
                            recovery_attempts += 1;
                        } else {
                            final_status = RunnerExitStatus::LiveStall(
                                "Active foreground render stall exceeded limit".to_string(),
                            );
                            break;
                        }
                    } else {
                        if observed_phases.last() != Some(&ObserverPhase::ActiveGame) {
                            observed_phases.push(ObserverPhase::ActiveGame);
                        }
                    }
                }
            }

            // 2. Check outcome.json
            match self.transport.shell(&serial, &["cat", &remote_outcome]) {
                Ok(out) => {
                    if in_transport_loss {
                        in_transport_loss = false;
                        reconnected_without_relaunch = true;
                    }
                    transport_lost_time = None;
                    if out.exit_code == 0 && out.stdout.contains("\"status\"") {
                        if observed_phases.last() != Some(&ObserverPhase::Completed) {
                            observed_phases.push(ObserverPhase::Completed);
                        }
                        final_status = RunnerExitStatus::Completed;
                        break;
                    }
                }
                Err(_) => {
                    in_transport_loss = true;
                    if observed_phases.last() != Some(&ObserverPhase::AdbTransportLoss) {
                        observed_phases.push(ObserverPhase::AdbTransportLoss);
                    }
                    let lost_at = transport_lost_time.get_or_insert_with(Instant::now);
                    let recovery_deadline =
                        Duration::from_secs_f64(self.config.recovery_deadline_secs);
                    if lost_at.elapsed() > recovery_deadline {
                        final_status = RunnerExitStatus::TransportLost;
                        break;
                    }
                }
            }

            // 3. Check if process is still alive
            if let Ok(p_out) = self.transport.shell(&serial, &["pidof", &self.config.package_id]) {
                if in_transport_loss {
                    in_transport_loss = false;
                    reconnected_without_relaunch = true;
                }
                transport_lost_time = None;
                if p_out.stdout.trim().is_empty() {
                    // Confirmed process death
                    if observed_phases.last() != Some(&ObserverPhase::ConfirmedDeath) {
                        observed_phases.push(ObserverPhase::ConfirmedDeath);
                    }
                    if first_failure.is_none() {
                        first_failure = Some(FirstFailureRecord {
                            phase: ObserverPhase::ConfirmedDeath,
                            reason_code: "PROCESS_KILLED".to_string(),
                            description: "Process died or was terminated unexpectedly".to_string(),
                            timestamp_ms: scenario_start.elapsed().as_millis() as u64,
                        });
                    }
                    final_status = RunnerExitStatus::ConfirmedDeath {
                        exit_code: -1,
                        signal: None,
                    };
                    break;
                }
            }

            std::thread::sleep(Duration::from_millis(100));
        }

        // Pull artifacts to host output dir
        let _ = std::fs::create_dir_all(&self.config.host_output_dir);
        let local_manifest = self.config.host_output_dir.join("manifest.json");
        let _ = self.transport.pull(&serial, &remote_manifest, &local_manifest);

        let local_outcome = self.config.host_output_dir.join("outcome.json");
        let _ = self.transport.pull(&serial, &remote_outcome, &local_outcome);

        let local_status = self.config.host_output_dir.join("status.json");
        let _ = self.transport.pull(&serial, &remote_status, &local_status);

        let local_breadcrumb = self.config.host_output_dir.join("crash_breadcrumb.bin");
        if self.transport.pull(&serial, &remote_breadcrumb, &local_breadcrumb).is_ok() {
            if let Ok(bytes) = std::fs::read(&local_breadcrumb) {
                if let Ok(crumb) = parse_crash_breadcrumb(&bytes) {
                    if let RunnerExitStatus::ConfirmedDeath { ref mut signal, ref mut exit_code } = final_status {
                        *signal = Some(crumb.signal_num);
                        *exit_code = crumb.exit_reason as i32;
                    }
                }
            }
        }

        self.cleanup(&serial, &remote_diag_root);

        AndroidRunnerResult {
            run_id: self.config.run_id.clone(),
            target_device: serial,
            target_package: self.config.package_id.clone(),
            exit_status: final_status,
            acknowledged,
            local_artifact_dir: self.config.host_output_dir.clone(),
            failure_reason: None,
            first_failure,
            recovery_attempts,
            reconnected_without_relaunch,
            observed_phases,
        }
    }

    /// Stops only the leased package and cleans up lease marker. Never signals unrelated apps!
    fn cleanup(&self, serial: &str, remote_diag_root: &str) {
        let _ = self
            .transport
            .shell(serial, &["am", "force-stop", &self.config.package_id]);
        let lease_path = format!("{}/active_lease.json", remote_diag_root);
        let _ = self.transport.shell(serial, &["rm", "-f", &lease_path]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::diag::adb::FakeAdb;
    use serde_json::json;

    fn test_config(run_id: &str) -> AndroidRunnerConfig {
        let req_json = json!({
            "schema_version": "1.0",
            "run_id": run_id,
            "build": { "variant": "diagnostic", "target": "android-arm64", "native_optimization": "release" },
            "scenario": {
                "scenario_id": "test_sc", "route_id": "test_rt", "fixture_id": "test_fx",
                "deadlines": { "startup_deadline_secs": 1.0, "scenario_deadline_secs": 2.0, "recovery_deadline_secs": 0.5 }
            }
        });
        let req_bytes = serde_json::to_vec(&req_json).unwrap();
        AndroidRunnerConfig {
            serial: None,
            package_id: "org.witchtime.android".to_string(),
            activity_name: ".WitchTimeActivity".to_string(),
            run_id: run_id.to_string(),
            request_bytes: req_bytes,
            host_output_dir: std::env::temp_dir().join(format!("host_out_{}", run_id)),
            startup_deadline_secs: 0.5,
            scenario_deadline_secs: 1.0,
            recovery_deadline_secs: 0.2,
            max_recovery_attempts: 1,
            allow_reconnect_without_relaunch: true,
        }
    }

    #[test]
    fn test_multiple_devices_ambiguity_rejection() {
        let fake = FakeAdb::new();
        fake.add_device("device_1", "device");
        fake.add_device("device_2", "device");

        let cfg = test_config("run_ambiguous");
        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();

        match res.exit_status {
            RunnerExitStatus::AmbiguousDevice(serials) => {
                assert_eq!(serials.len(), 2);
                assert!(serials.contains(&"device_1".to_string()));
                assert!(serials.contains(&"device_2".to_string()));
            }
            other => panic!("Expected AmbiguousDevice, got {:?}", other),
        }
    }

    #[test]
    fn test_explicit_serial_selection() {
        let fake = FakeAdb::new();
        fake.add_device("device_1", "device");
        fake.add_device("device_2", "device");

        let mut cfg = test_config("run_explicit");
        cfg.serial = Some("device_2".to_string());
        let runner = AndroidRunner::new(&fake, cfg);
        let resolved = runner.resolve_device().unwrap();
        assert_eq!(resolved, "device_2");
    }

    #[test]
    fn test_concurrent_ownership_rejection() {
        let fake = FakeAdb::new();
        fake.add_device("dev_1", "device");
        fake.set_process_running("dev_1", "org.witchtime.android", 9999, "other_token");
        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/active_lease.json",
            b"{\"run_id\":\"other_active_run\"}".to_vec(),
        );

        let cfg = test_config("run_conflict");
        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();

        match res.exit_status {
            RunnerExitStatus::ConcurrentRunConflict(msg) => {
                assert!(msg.contains("other_active_run"));
            }
            other => panic!("Expected ConcurrentRunConflict, got {:?}", other),
        }
    }

    #[test]
    fn test_existing_normal_game_conflict() {
        let fake = FakeAdb::new();
        fake.add_device("dev_1", "device");
        fake.set_process_running("dev_1", "org.witchtime.android", 8888, "normal_interactive");

        let cfg = test_config("run_normal_conflict");
        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();

        match res.exit_status {
            RunnerExitStatus::ExistingAppRunningConflict(msg) => {
                assert!(msg.contains("already running"));
                assert!(msg.contains("8888"));
            }
            other => panic!("Expected ExistingAppRunningConflict, got {:?}", other),
        }
        // Verify normal game was NOT killed
        assert!(fake.killed_packages.lock().unwrap().is_empty());
    }

    #[test]
    fn test_wrong_or_missing_startup_acknowledgement() {
        let fake = FakeAdb::new();
        fake.add_device("dev_1", "device");

        let mut cfg = test_config("run_missing_ack");
        cfg.startup_deadline_secs = 0.1;
        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();

        assert_eq!(res.exit_status, RunnerExitStatus::StartupTimeout);
        assert!(!res.acknowledged);

        // Test digest mismatch
        let fake2 = FakeAdb::new();
        fake2.add_device("dev_1", "device");
        fake2.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_bad_digest/manifest.json",
            b"{\"status\": \"acknowledged\", \"run_id\": \"run_bad_digest\", \"request_digest\": \"bad_digest\"}".to_vec(),
        );
        let cfg2 = test_config("run_bad_digest");
        let runner2 = AndroidRunner::new(&fake2, cfg2);
        let res2 = runner2.execute();
        match res2.exit_status {
            RunnerExitStatus::AcknowledgeMismatch(msg) => {
                assert!(msg.contains("mismatch"));
            }
            other => panic!("Expected AcknowledgeMismatch, got {:?}", other),
        }
    }

    #[test]
    fn test_deadlines_cancellation_and_only_leased_app_killed() {
        let fake = FakeAdb::new();
        fake.add_device("dev_1", "device");
        fake.set_process_running("dev_1", "com.other.app", 1111, "other");

        let mut cfg = test_config("run_timeout");
        cfg.startup_deadline_secs = 0.2;
        let digest = compute_request_digest(&cfg.request_bytes);
        fake.write_remote_file(
            "dev_1",
            &format!("/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_timeout/manifest.json"),
            format!("{{\"status\": \"acknowledged\", \"run_id\": \"run_timeout\", \"request_digest\": \"{}\"}}", digest).into_bytes(),
        );

        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();

        assert!(res.acknowledged);
        assert_eq!(res.exit_status, RunnerExitStatus::ScenarioTimeout);

        // Verify that cleanup killed ONLY org.witchtime.android and NEVER com.other.app!
        let killed = fake.killed_packages.lock().unwrap();
        assert!(killed.contains(&"org.witchtime.android".to_string()));
        assert!(!killed.contains(&"com.other.app".to_string()));
    }

    #[test]
    fn test_transport_loss_and_reconnection_timeout() {
        let fake = FakeAdb::new();
        fake.add_device("dev_1", "device");
        let cfg = test_config("run_transport_loss");
        let digest = compute_request_digest(&cfg.request_bytes);
        fake.write_remote_file(
            "dev_1",
            &format!("/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_transport_loss/manifest.json"),
            format!("{{\"status\": \"acknowledged\", \"run_id\": \"run_transport_loss\", \"request_digest\": \"{}\"}}", digest).into_bytes(),
        );

        // Disconnect transport right after launch
        fake.set_transport_connected("dev_1", false);

        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();
        assert_eq!(res.exit_status, RunnerExitStatus::TransportLost);
    }

    #[test]
    fn test_no_first_frame_is_not_labelled_anr() {
        let fake = FakeAdb::new();
        fake.add_device("dev_1", "device");
        let mut cfg = test_config("run_no_first_frame");
        cfg.startup_deadline_secs = 0.2;
        let digest = compute_request_digest(&cfg.request_bytes);

        // Acknowledged, but status has surface_generation: 0 and render_age_ms: 10000 (no first frame presented)
        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_no_first_frame/manifest.json",
            format!("{{\"status\": \"acknowledged\", \"run_id\": \"run_no_first_frame\", \"request_digest\": \"{}\"}}", digest).into_bytes(),
        );
        let status_json = json!({
            "schema_version": "1.0",
            "run_id": "run_no_first_frame",
            "timestamp_ns": 1000000u64,
            "healthy": true,
            "state": "active",
            "lifecycle": { "state": "resumed", "is_foreground": true },
            "surface_generation": 0,
            "progress": {
                "render_age_ms": 10000,
                "is_active_stall": false,
                "expected_inactivity": false
            },
            "recorder": {
                "total_emitted": 0, "total_dropped": 0, "chunks_committed": 0,
                "total_records_written": 0, "total_bytes_written": 0,
                "remaining_budget": 1000, "reserve_held": false,
                "incidents_written": 0, "active_producers": 0
            }
        });
        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_no_first_frame/status.json",
            serde_json::to_vec(&status_json).unwrap(),
        );

        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();

        assert_eq!(res.exit_status, RunnerExitStatus::NoFirstFrame);
        assert!(res.acknowledged);
        // Verify failure reason strictly does NOT mention ANR
        let reason = res.failure_reason.unwrap();
        assert!(!reason.contains("ANR"));
        assert!(reason.contains("presentation never occurred"));
    }

    #[test]
    fn test_background_inactivity_not_labelled_stall() {
        let fake = FakeAdb::new();
        fake.add_device("dev_1", "device");
        let cfg = test_config("run_bg_inactivity");
        let digest = compute_request_digest(&cfg.request_bytes);

        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_bg_inactivity/manifest.json",
            format!("{{\"status\": \"acknowledged\", \"run_id\": \"run_bg_inactivity\", \"request_digest\": \"{}\"}}", digest).into_bytes(),
        );

        // App enters stopped background state: expected_inactivity = true
        let status_json = json!({
            "schema_version": "1.0",
            "run_id": "run_bg_inactivity",
            "timestamp_ns": 2000000u64,
            "healthy": true,
            "state": "active",
            "lifecycle": { "state": "stopped", "is_foreground": false },
            "surface_generation": 1,
            "progress": {
                "render_age_ms": 15000,
                "is_active_stall": false,
                "expected_inactivity": true
            },
            "recorder": {
                "total_emitted": 10, "total_dropped": 0, "chunks_committed": 1,
                "total_records_written": 10, "total_bytes_written": 1000,
                "remaining_budget": 1000, "reserve_held": false,
                "incidents_written": 0, "active_producers": 1
            }
        });
        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_bg_inactivity/status.json",
            serde_json::to_vec(&status_json).unwrap(),
        );
        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_bg_inactivity/outcome.json",
            b"{\"status\": \"clean_completion\"}".to_vec(),
        );

        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();

        assert_eq!(res.exit_status, RunnerExitStatus::Completed);
        assert!(res.observed_phases.contains(&ObserverPhase::BackgroundInactivity));
        assert!(!res.observed_phases.contains(&ObserverPhase::LiveStall));
    }

    #[test]
    fn test_live_stall_and_first_failure_preservation() {
        let fake = FakeAdb::new();
        fake.add_device("dev_1", "device");
        let mut cfg = test_config("run_live_stall");
        cfg.max_recovery_attempts = 0; // Fail on first stall
        let digest = compute_request_digest(&cfg.request_bytes);

        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_live_stall/manifest.json",
            format!("{{\"status\": \"acknowledged\", \"run_id\": \"run_live_stall\", \"request_digest\": \"{}\"}}", digest).into_bytes(),
        );

        // Active foreground stall
        let status_json = json!({
            "schema_version": "1.0",
            "run_id": "run_live_stall",
            "timestamp_ns": 3000000u64,
            "healthy": false,
            "state": "active",
            "lifecycle": { "state": "resumed", "is_foreground": true },
            "surface_generation": 1,
            "progress": {
                "render_age_ms": 6000,
                "is_active_stall": true,
                "expected_inactivity": false
            },
            "recorder": {
                "total_emitted": 20, "total_dropped": 0, "chunks_committed": 1,
                "total_records_written": 20, "total_bytes_written": 2000,
                "remaining_budget": 1000, "reserve_held": false,
                "incidents_written": 1, "active_producers": 1
            }
        });
        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_live_stall/status.json",
            serde_json::to_vec(&status_json).unwrap(),
        );

        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();

        match res.exit_status {
            RunnerExitStatus::LiveStall(msg) => {
                assert!(msg.contains("stall exceeded limit"));
            }
            other => panic!("Expected LiveStall, got {:?}", other),
        }
        assert!(res.first_failure.is_some());
        let ff = res.first_failure.unwrap();
        assert_eq!(ff.phase, ObserverPhase::LiveStall);
        assert_eq!(ff.reason_code, "SCENARIO_TIMEOUT");
    }

    #[test]
    fn test_confirmed_death_with_crash_breadcrumb() {
        let fake = FakeAdb::new();
        fake.add_device("dev_1", "device");
        let cfg = test_config("run_crash_death");
        let digest = compute_request_digest(&cfg.request_bytes);

        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_crash_death/manifest.json",
            format!("{{\"status\": \"acknowledged\", \"run_id\": \"run_crash_death\", \"request_digest\": \"{}\"}}", digest).into_bytes(),
        );

        // Write 64-byte crash breadcrumb with SIGSEGV (11)
        let mut raw_crumb = [0u8; 64];
        raw_crumb[0..4].copy_from_slice(&0x43525348u32.to_le_bytes()); // 'CRSH'
        raw_crumb[4..8].copy_from_slice(&11u32.to_le_bytes()); // SIGSEGV
        raw_crumb[8..16].copy_from_slice(&0xDEADBEEFu64.to_le_bytes());
        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_crash_death/crash_breadcrumb.bin",
            raw_crumb.to_vec(),
        );

        // Process dies immediately
        *fake.simulate_process_death_on_pidof.lock().unwrap() = true;

        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();

        match res.exit_status {
            RunnerExitStatus::ConfirmedDeath { signal, .. } => {
                assert_eq!(signal, Some(11));
            }
            other => panic!("Expected ConfirmedDeath, got {:?}", other),
        }
        assert!(res.first_failure.is_some());
        assert_eq!(res.first_failure.unwrap().phase, ObserverPhase::ConfirmedDeath);
    }

    #[test]
    fn test_reconnect_without_duplicate_launch() {
        let fake = FakeAdb::new();
        fake.add_device("dev_1", "device");
        let mut cfg = test_config("run_reconnect");
        cfg.recovery_deadline_secs = 1.0;
        let digest = compute_request_digest(&cfg.request_bytes);

        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_reconnect/manifest.json",
            format!("{{\"status\": \"acknowledged\", \"run_id\": \"run_reconnect\", \"request_digest\": \"{}\"}}", digest).into_bytes(),
        );
        fake.write_remote_file(
            "dev_1",
            "/sdcard/Android/data/org.witchtime.android/files/diagnostics/run_reconnect/outcome.json",
            b"{\"status\": \"clean_completion\"}".to_vec(),
        );

        let runner = AndroidRunner::new(&fake, cfg);
        let res = runner.execute();

        assert_eq!(res.exit_status, RunnerExitStatus::Completed);
        // Verify Activity was launched exactly ONCE, never duplicated on reconnect!
        assert_eq!(*fake.am_start_invocations.lock().unwrap(), 1);
    }
}
