use std::path::PathBuf;
use std::time::{Duration, Instant};

use crate::diag::adb::{AdbDeviceInfo, AdbTransport};
use crate::diag::bootstrap::compute_request_digest;
use crate::diag::models::{DiagnosticLiveStatus, DiagnosticTemporalObservation, TemporalSelection, SgsrVariant, TemporalSupport, TemporalOutputGrid, DiagnosticGpuComponent, DiagnosticGpuComponentReport};
use crate::diag::symbolization::parse_crash_breadcrumb;

#[derive(Debug, Clone, serde::Deserialize)]
struct TemporalRecord {
    seq: u64, prod: u32, tid: u32, gen: u32,
    #[serde(rename = "type")]
    kind: u32,
    p0: i64, p1: i64, p2: i64, p3: i64,
}

fn decode_temporal_records(records: &[TemporalRecord]) -> Option<DiagnosticTemporalObservation> {
    let mut group: Vec<&TemporalRecord> = Vec::new();
    let mut latest = None;
    let mut previous_seq = None;
    let mut surfaces = std::collections::BTreeMap::new();
    for r in records {
        if r.kind == 8 {
            if !(0..=u32::MAX as i64).contains(&r.p0) || !(0..=u32::MAX as i64).contains(&r.p1)
                || !(0..=u32::MAX as i64).contains(&r.p2) { return None; }
            if r.p1 == 0 || r.p2 == 0 { surfaces.remove(&(r.p0 as u32)); }
            else { surfaces.insert(r.p0 as u32, (r.seq, [r.p1 as u32, r.p2 as u32])); }
            continue;
        }
        if !(32..=35).contains(&r.kind) { continue; }
        if previous_seq.map_or(false, |seq| r.seq <= seq) { return None; }
        previous_seq = Some(r.seq);
        if r.kind == 32 { group.clear(); latest = None; }
        if r.kind != 32 + group.len() as u32 { return None; }
        if let Some(first) = group.first() {
            if (r.p0, r.prod, r.tid, r.gen) != (first.p0, first.prod, first.tid, first.gen) { return None; }
        }
        group.push(r);
        if group.len() != 4 { continue; }
        let (selection, dims, state, failure) = (group[0], group[1], group[2], group[3]);
        if selection.p0 < 0 || !(0..=8).contains(&selection.p1) || !(0..=63).contains(&state.p1)
            || !(0..=u32::MAX as i64).contains(&dims.p3) { return None; }
        let variant = match selection.p3 {
            0 => Some(SgsrVariant::Fragment), 1 => Some(SgsrVariant::Compute2),
            2 => Some(SgsrVariant::Compute3), -1 => None, _ => return None,
        };
        let effective = match selection.p2 {
            0 => TemporalSelection::Native {}, 1 => TemporalSelection::Fsr3 {},
            2 => TemporalSelection::Fsr3Quality {}, 3 => TemporalSelection::Fsr3Balanced {},
            4 => TemporalSelection::Fsr3Performance {}, 5 => TemporalSelection::Fsr3UltraPerformance {},
            6 => TemporalSelection::Sgsr2 { variant: variant? },
            7 => TemporalSelection::Sgsr2Quality { variant: variant? },
            8 => TemporalSelection::Sgsr2Performance { variant: variant? }, _ => return None,
        };
        let motion = match state.p2 {
            0 => TemporalSupport::Unavailable, 1 => TemporalSupport::CameraOnly,
            2 => TemporalSupport::PartialDynamic, 3 => TemporalSupport::FullDynamic, _ => return None,
        };
        let flags = state.p1;
        // A contradictory consumed claim is corrupt, not successful execution.
        if flags & 8 != 0 && (flags & 7 != 7 || flags & 16 != 0 || failure.p1 != 0 || selection.p2 == 0) { return None; }
        let extent = |p: i64| [((p as u64) >> 32) as u32, p as u32];
        latest = Some(DiagnosticTemporalObservation {
            effective, scene_dimensions: extent(dims.p1), output_dimensions: extent(dims.p2), motion,
            output_grid: TemporalOutputGrid::Reconstruction,
            surface_dimensions: surfaces.get(&(dims.p3 as u32)).filter(|(seq, _)| *seq <= selection.seq).map(|(_, size)| *size),
            reactive_consumed: flags & 32 != 0, supported: flags & 1 != 0,
            initialized: flags & 2 != 0, dispatched: flags & 4 != 0, consumed: flags & 8 != 0,
            skipped: flags & 16 != 0,
            reset_reason: if state.p3 == 0 { None } else { Some(format!("renderer_reset_{}", state.p3)) },
            fallback_reason: if failure.p1 == 0 { None } else { Some(format!("renderer_fallback_{}", failure.p1)) },
            scene_frame: selection.p0 as u64, surface_generation: dims.p3 as u32,
            // No event establishes LOD/mips/etc. Never derive effective settings from the request.
            unrelated_settings_hash: String::new(),
        });
        group.clear();
    }
    if group.is_empty() { latest } else { None }
}

const MAX_EVENT_STREAM_BYTES: usize = 128 * 1024 * 1024;
const MAX_EVENT_CHUNK_BYTES: usize = 16 * 1024 * 1024;
// Compact required fields, one-digit values, one separating newline; no trailing newline assumed.
const MIN_EVENT_HEADER_BYTES: usize = b"{\"chunk_id\":1,\"start_seq\":1,\"end_seq\":1,\"count\":1}\n".len();
const MIN_EVENT_RECORD_BYTES: usize = b"{\"seq\":1,\"ts\":0,\"type\":0,\"prod\":0,\"tid\":0,\"gen\":0,\"p0\":0,\"p1\":0,\"p2\":0,\"p3\":0}".len();

fn collect_event_chunks(transport: &dyn AdbTransport, serial: &str, remote_root: &str,
                        output: &std::path::Path, status_path: Option<&std::path::Path>, run_id: &str) -> Result<Vec<PathBuf>, String> {
    let status = status_path.and_then(|p| read_temporal_artifact(p, 1024 * 1024).ok())
        .and_then(|bytes| serde_json::from_slice::<DiagnosticLiveStatus>(&bytes).ok());
    let stopped = status.filter(|s| s.run_id == run_id && s.state == "stopped");
    let required = stopped.as_ref().map(|s| s.recorder.chunks_committed);
    if let Some(s) = &stopped {
        let r = &s.recorder;
        let minimum_bytes = u64::from(r.chunks_committed).checked_mul(MIN_EVENT_HEADER_BYTES as u64)
            .and_then(|headers| r.total_records_written.checked_mul(MIN_EVENT_RECORD_BYTES as u64)
                .and_then(|records| headers.checked_add(records)))
            .ok_or("stopped status byte accounting overflow")?;
        // Writer bytes include incidents; only event bytes have this collector's 128 MiB ceiling.
        if u64::from(r.chunks_committed) > r.total_records_written
            || (r.chunks_committed == 0) != (r.total_records_written == 0)
            || r.total_records_written > r.total_emitted
            || minimum_bytes > r.total_bytes_written
            || minimum_bytes > MAX_EVENT_STREAM_BYTES as u64 {
            return Err("inconsistent or over-budget stopped status byte/record/chunk counts".into());
        }
    }
    if required.is_none() {
        eprintln!("event collection completeness unknown: no matching typed stopped status (legacy limit 200)");
    }
    let mut chunks = Vec::new();
    let mut bytes_read = 0usize;
    let mut sequence = Vec::new();
    for i in 1..=required.unwrap_or(200) {
        let name = format!("events-{:06}.jsonl", i);
        let local = output.join(&name);
        if let Err(error) = transport.pull(serial, &format!("{}/{}", remote_root, name), &local) {
            if required.is_some() { return Err(format!("required event chunk {} unavailable: {}", i, error)); }
            break;
        }
        let bytes = read_temporal_artifact(&local, MAX_EVENT_CHUNK_BYTES)?;
        bytes_read = bytes_read.checked_add(bytes.len()).ok_or("event stream byte accounting overflow")?;
        if bytes_read > MAX_EVENT_STREAM_BYTES { return Err("event stream exceeds 128 MiB size bound".into()); }
        if let Some(s) = &stopped {
            if bytes_read as u64 > s.recorder.total_bytes_written {
                return Err("event stream exceeds stopped status declared bytes".into());
            }
            let mut lines = bytes.split(|b| *b == b'\n').filter(|line| !line.is_empty());
            let header_line = lines.next().ok_or("missing chunk header")?;
            if header_line.len() > 4096 { return Err("event header exceeds size bound".into()); }
            let header: serde_json::Value = serde_json::from_slice(header_line)
                .map_err(|e| e.to_string())?;
            let mut count = 0u64;
            let mut first = None;
            let mut last = None;
            for line in lines {
                if line.len() > 4096 { return Err("event row exceeds size bound".into()); }
                let value: serde_json::Value = serde_json::from_slice(line).map_err(|e| e.to_string())?;
                if value["ts"].as_u64().is_none() { return Err("missing or invalid event timestamp".into()); }
                let record: TemporalRecord = serde_json::from_value(value).map_err(|e| e.to_string())?;
                if record.seq == 0 || last.map_or(false, |seq| record.seq <= seq) {
                    return Err("invalid within-chunk sequence".into());
                }
                if first.is_none() { first = Some(record.seq); }
                last = Some(record.seq);
                count = count.checked_add(1).ok_or("event record accounting overflow")?;
                sequence.push(record.seq);
                if sequence.len() as u64 > s.recorder.total_records_written {
                    return Err("event stream exceeds stopped status declared records".into());
                }
            }
            if count == 0 || header["chunk_id"].as_u64() != Some(u64::from(i))
                || header["count"].as_u64() != Some(count) || header["start_seq"].as_u64() != first
                || header["end_seq"].as_u64() != last {
                return Err("empty or inconsistent event chunk header".into());
            }
        }
        chunks.push(local);
    }
    if let Some(s) = &stopped {
        if sequence.len() as u64 != s.recorder.total_records_written
            || (s.recorder.incidents_written == 0 && bytes_read as u64 != s.recorder.total_bytes_written) {
            return Err("event stream disagrees with stopped status totals".into());
        }
        sequence.sort_unstable();
        if sequence.iter().enumerate().any(|(i, seq)| *seq != i as u64 + 1) {
            return Err("incomplete or duplicate global event sequence".into());
        }
    }
    Ok(chunks)
}

fn read_temporal_artifact(path: &std::path::Path, limit: usize) -> Result<Vec<u8>, String> {
    use std::io::Read;
    let file = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let mut bytes = Vec::new();
    file.take(limit as u64 + 1).read_to_end(&mut bytes).map_err(|e| e.to_string())?;
    if bytes.len() > limit { return Err("temporal artifact exceeds size bound".into()); }
    Ok(bytes)
}

/// Derive bounded runtime observations; missing legacy identities/settings stay missing.
fn enrich_temporal_manifest(manifest_path: &std::path::Path, status_path: Option<&std::path::Path>,
                            chunks: &[PathBuf], run_id: &str, gpu_components_requested: bool) -> Result<(), String> {
    let mut manifest: serde_json::Value = serde_json::from_slice(&read_temporal_artifact(manifest_path, 16 * 1024 * 1024)?)
        .map_err(|e| e.to_string())?;
    if manifest["run_id"].as_str() != Some(run_id) { return Err("manifest run mismatch".into()); }
    let object = manifest.as_object_mut().ok_or("manifest is not an object")?;
    let had_observation = object.remove("temporal_observation").is_some();
    let had_components = object.remove("gpu_components").is_some();
    if gpu_components_requested {
        object.insert("gpu_components".into(), serde_json::to_value(DiagnosticGpuComponentReport {
            healthy: false, reason: Some("GPU component collector has no validated stopped-writer events".into()), observations: Vec::new(),
        }).map_err(|e| e.to_string())?);
    }
    let status = status_path.and_then(|p| read_temporal_artifact(p, 1024 * 1024).ok())
        .and_then(|bytes| serde_json::from_slice::<DiagnosticLiveStatus>(&bytes).ok());
    let healthy = status.as_ref().map_or(false, |s| s.run_id == run_id && s.healthy && s.state == "stopped"
        && s.recorder.total_dropped == 0 && s.recorder.total_emitted == s.recorder.total_records_written
        && s.recorder.chunks_committed as usize == chunks.len());
    if healthy {
        let mut records = Vec::new();
        let mut valid = true;
        let mut bytes_read = 0usize;
        let mut records_read = 0u64;
        let mut sequence = Vec::new();
        for (index, path) in chunks.iter().enumerate() {
            let bytes = read_temporal_artifact(path, 16 * 1024 * 1024)?;
            bytes_read += bytes.len();
            if bytes.len() > 16 * 1024 * 1024 || bytes_read > 128 * 1024 * 1024 { valid = false; break; }
            let mut header = None;
            let mut count = 0u64;
            let mut first_seq = None;
            let mut last_seq = None;
            for line in bytes.split(|b| *b == b'\n').filter(|l| !l.is_empty()) {
                if line.len() > 4096 { valid = false; break; }
                let value: serde_json::Value = match serde_json::from_slice(line) {
                    Ok(v) => v, Err(_) => { valid = false; break; }
                };
                if header.is_none() {
                    if value["chunk_id"].as_u64() != Some(index as u64 + 1) { valid = false; break; }
                    header = Some(value); continue;
                }
                let record: TemporalRecord = match serde_json::from_value(value) {
                    Ok(r) => r, Err(_) => { valid = false; break; }
                };
                if last_seq.map_or(false, |s| record.seq <= s) { valid = false; break; }
                if first_seq.is_none() { first_seq = Some(record.seq); }
                last_seq = Some(record.seq); count += 1;
                sequence.push(record.seq);
                if record.kind == 8 || (32..=36).contains(&record.kind) { records.push(record); }
            }
            if header.as_ref().map_or(true, |h| h["count"].as_u64() != Some(count)
                || h["start_seq"].as_u64() != first_seq || h["end_seq"].as_u64() != last_seq) { valid = false; }
            records_read += count;
            if !valid { break; }
        }
        if status.as_ref().map_or(true, |s| s.recorder.total_records_written != records_read) { valid = false; }
        // Chunks drain separate producers: verify the complete sequence set, not chunk order.
        sequence.sort_unstable();
        if sequence.iter().enumerate().any(|(i, seq)| *seq != i as u64 + 1) { valid = false; }
        if valid {
            records.sort_unstable_by_key(|r| r.seq);
            if let Some(observation) = decode_temporal_records(&records) {
                object.insert("temporal_observation".into(), serde_json::to_value(observation).map_err(|e| e.to_string())?);
            }
        }
        let mut components: Vec<_> = records.iter().filter(|r| r.kind == 36).collect();
        if !components.is_empty() {
            components.sort_unstable_by_key(|r| r.seq);
            let report = if !gpu_components_requested {
                DiagnosticGpuComponentReport { healthy: false, reason: Some("GPU component collector was not requested".into()), observations: Vec::new() }
            } else if !valid {
                DiagnosticGpuComponentReport { healthy: false, reason: Some("incomplete or malformed event stream".into()), observations: Vec::new() }
            } else if components.windows(2).any(|pair| pair[0].seq == pair[1].seq) {
                DiagnosticGpuComponentReport { healthy: false, reason: Some("duplicate GPU component sequence".into()), observations: Vec::new() }
            } else {
                match DiagnosticGpuComponent::reduce_payloads(components.iter().map(|r| [r.p0, r.p1, r.p2, r.p3]), true) {
                    Ok(observations) => DiagnosticGpuComponentReport { healthy: true, reason: None, observations },
                    Err(reason) => DiagnosticGpuComponentReport { healthy: false, reason: Some(reason.into()), observations: Vec::new() },
                }
            };
            object.insert("gpu_components".into(), serde_json::to_value(report).map_err(|e| e.to_string())?);
        }
    }
    if !had_observation && !had_components && !object.contains_key("temporal_observation") && !object.contains_key("gpu_components") { return Ok(()); }
    // Local derived artifact only; the app's receipt and other provenance are unchanged.
    let tmp = manifest_path.with_extension("temporal.tmp");
    std::fs::write(&tmp, serde_json::to_vec_pretty(&manifest).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    std::fs::rename(tmp, manifest_path).map_err(|e| e.to_string())
}

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

        // Ensure remote session directory exists
        let _ = self.transport.shell(&serial, &["mkdir", "-p", &remote_session_root]);

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

        // Ensure diagnostic directory and staged request are world-accessible by app user
        let _ = self.transport.shell(&serial, &["chmod", "-R", "777", &remote_diag_root]);

        let expected_digest = compute_request_digest(&self.config.request_bytes);

        // Launch Intent
        let comp = format!("{}/{}", self.config.package_id, self.config.activity_name);
        let start_res = self.transport.shell(
            &serial,
            &[
                "am",
                "start",
                "-a",
                "android.intent.action.MAIN",
                "-c",
                "android.intent.category.LAUNCHER",
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
                    if out.exit_code == 0 && (out.stdout.contains("\"execution_outcome\"") || out.stdout.contains("\"status\"")) {
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
                    // Check if outcome.json was written before process exit
                    let outcome_check = self.transport.shell(&serial, &["cat", &remote_outcome]);
                    if let Ok(chk) = outcome_check {
                        if chk.exit_code == 0 && (chk.stdout.contains("\"completed\"") || chk.stdout.contains("\"clean_completion\"")) {
                            if observed_phases.last() != Some(&ObserverPhase::Completed) {
                                observed_phases.push(ObserverPhase::Completed);
                            }
                            final_status = RunnerExitStatus::Completed;
                            break;
                        }
                    }
                    // Confirmed unexpected process death
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

        // Ensure remote session artifacts are readable by shell
        let _ = self.transport.shell(&serial, &["chmod", "-R", "777", &remote_session_root]);

        // Pull artifacts to host output dir
        let _ = std::fs::create_dir_all(&self.config.host_output_dir);
        let local_manifest = self.config.host_output_dir.join("manifest.json");
        let manifest_pulled = self.transport.pull(&serial, &remote_manifest, &local_manifest).is_ok();

        let local_outcome = self.config.host_output_dir.join("outcome.json");
        let _ = self.transport.pull(&serial, &remote_outcome, &local_outcome);

        let local_status = self.config.host_output_dir.join("status.json");
        let status_pulled = self.transport.pull(&serial, &remote_status, &local_status).is_ok();

        let remote_session_log = format!("{}/session.log", remote_session_root);
        let local_session_log = self.config.host_output_dir.join("session.log");
        let _ = self.transport.pull(&serial, &remote_session_log, &local_session_log);

        // Pull event chunks
        let temporal_chunks = match collect_event_chunks(self.transport, &serial, &remote_session_root,
            &self.config.host_output_dir, if status_pulled { Some(&local_status) } else { None }, &self.config.run_id) {
            Ok(chunks) => chunks,
            Err(error) => {
                eprintln!("event collection failed: {}", error);
                if final_status == RunnerExitStatus::Completed { final_status = RunnerExitStatus::CollectorFailure(error); }
                Vec::new()
            }
        };
        if manifest_pulled {
            let components_requested = serde_json::from_slice::<crate::diag::models::DiagnosticRequest>(&self.config.request_bytes)
                .map(|request| request.profile.collectors.iter().any(|c| c == "gpu_components")).unwrap_or(false);
            if let Err(error) = enrich_temporal_manifest(&local_manifest,
                if status_pulled { Some(local_status.as_path()) } else { None },
                &temporal_chunks, &self.config.run_id, components_requested) {
                eprintln!("runtime observation provenance unavailable: {}", error);
            }
        }

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

    fn temporal_records() -> Vec<TemporalRecord> {
        let payloads = [[17, 7, 7, 1], [17, (853i64 << 32) | 479, (1280i64 << 32) | 720, 3],
            [17, 15, 1, 1], [17, 0, -1, -1]];
        payloads.iter().enumerate().map(|(i, p)| TemporalRecord {
            seq: i as u64 + 10, kind: 32 + i as u32, prod: 0, tid: 8, gen: 1,
            p0: p[0], p1: p[1], p2: p[2], p3: p[3],
        }).collect()
    }

    fn collection_fixture(tag: &str, count: u32) -> (PathBuf, PathBuf, serde_json::Value, FakeAdb) {
        let dir = std::env::temp_dir().join(format!("event_collection_{}_{}", std::process::id(), tag));
        std::fs::create_dir_all(&dir).unwrap();
        let status = dir.join("status.json");
        let mut health = json!({"schema_version":"1.0", "run_id":"collection", "timestamp_ns":1,
            "healthy":true, "state":"stopped", "lifecycle":{"state":"resumed","is_foreground":true},
            "surface_generation":3, "progress":{"render_age_ms":0,"is_active_stall":false,"expected_inactivity":false},
            "recorder":{"total_emitted":count,"total_dropped":0,"chunks_committed":count,"total_records_written":count,
                "total_bytes_written":0,"remaining_budget":1024,"reserve_held":false,"incidents_written":0,"active_producers":1}});
        let adb = FakeAdb::new();
        adb.add_device("fixture", "device");
        let mut bytes = 0;
        for i in 1..=count {
            let chunk = format!("{}\n{}\n", json!({"chunk_id":i,"start_seq":i,"end_seq":i,"count":1}),
                json!({"seq":i,"ts":0,"type":0,"prod":0,"tid":0,"gen":0,"p0":0,"p1":0,"p2":0,"p3":0}));
            bytes += chunk.len();
            adb.write_remote_file("fixture", &format!("/run/events-{:06}.jsonl", i), chunk.into_bytes());
        }
        health["recorder"]["total_bytes_written"] = json!(bytes);
        std::fs::write(&status, health.to_string()).unwrap();
        (dir, status, health, adb)
    }

    fn remove_collection_fixture(dir: PathBuf) {
        for entry in std::fs::read_dir(&dir).unwrap() { std::fs::remove_file(entry.unwrap().path()).unwrap(); }
        std::fs::remove_dir(dir).unwrap();
    }

    #[test]
    fn stopped_collection_accepts_byte_bounded_4176_and_4102_chunk_streams() {
        for count in [336, 4176, 4102] {
            let (dir, status, _, adb) = collection_fixture(&format!("large_{count}"), count);
            let chunks = collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection").unwrap();
            assert_eq!(chunks.len(), count as usize);
            assert!(chunks.last().unwrap().ends_with(format!("events-{count:06}.jsonl")));
            remove_collection_fixture(dir);
        }
    }

    #[test]
    fn stopped_collection_requires_middle_and_tail_chunks() {
        for missing in [2, 4] {
            let (dir, status, _, adb) = collection_fixture(&format!("missing_{missing}"), 4);
            adb.remote_fs.lock().unwrap().get_mut("fixture").unwrap().remove(&format!("/run/events-{missing:06}.jsonl"));
            assert!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection")
                .unwrap_err().contains(&format!("chunk {missing} unavailable")));
            remove_collection_fixture(dir);
        }
    }

    #[test]
    fn stopped_collection_rejects_dishonest_totals_and_overflow_before_pulling() {
        let (dir, status, health, adb) = collection_fixture("totals", 4);
        for (field, value) in [("chunks_committed", 5), ("chunks_committed", 0),
            ("total_records_written", 3), ("total_records_written", u64::MAX),
            ("total_bytes_written", 1)] {
            let mut bad = health.clone(); bad["recorder"][field] = json!(value);
            std::fs::write(&status, bad.to_string()).unwrap();
            assert!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection").is_err(), "{field}={value}");
            assert!(!dir.join("events-000001.jsonl").exists());
        }
        for delta in [-1i64, 1] {
            let mut bad = health.clone();
            bad["recorder"]["total_bytes_written"] = json!((health["recorder"]["total_bytes_written"].as_u64().unwrap() as i64 + delta) as u64);
            std::fs::write(&status, bad.to_string()).unwrap();
            assert!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection").is_err());
        }
        // Incident bytes are not event bytes; only the upper bound is knowable here.
        let mut incident = health.clone(); incident["recorder"]["incidents_written"] = json!(1);
        incident["recorder"]["total_bytes_written"] = json!(health["recorder"]["total_bytes_written"].as_u64().unwrap() + 50);
        std::fs::write(&status, incident.to_string()).unwrap();
        assert_eq!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection").unwrap().len(), 4);
        // Combined writer accounting can exceed the separate event-stream collection budget.
        incident["recorder"]["total_bytes_written"] = json!(MAX_EVENT_STREAM_BYTES as u64 + 1024 * 1024);
        std::fs::write(&status, incident.to_string()).unwrap();
        assert_eq!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection").unwrap().len(), 4);
        let mut oversized_events = health.clone();
        let records = MAX_EVENT_STREAM_BYTES as u64 / MIN_EVENT_RECORD_BYTES as u64 + 1;
        oversized_events["recorder"]["total_records_written"] = json!(records);
        oversized_events["recorder"]["total_emitted"] = json!(records);
        oversized_events["recorder"]["total_bytes_written"] = json!(u64::MAX);
        std::fs::write(&status, oversized_events.to_string()).unwrap();
        assert!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection")
            .unwrap_err().contains("over-budget"));
        remove_collection_fixture(dir);
    }

    #[test]
    fn stopped_collection_rejects_bad_headers_empty_chunks_and_sequence_loss() {
        let (dir, status, health, adb) = collection_fixture("malformed", 4);
        let original = adb.remote_fs.lock().unwrap()["fixture"]["/run/events-000002.jsonl"].clone();
        let text = String::from_utf8(original.clone()).unwrap();
        let rows: Vec<serde_json::Value> = text.lines().map(|l| serde_json::from_str(l).unwrap()).collect();
        for case in 0..6 {
            let mut header = rows[0].clone(); let mut record = rows[1].clone();
            match case {
                0 => header["count"] = json!(2),
                1 => header["chunk_id"] = json!(3),
                2 => { header["start_seq"] = json!(1); header["end_seq"] = json!(1); record["seq"] = json!(1); },
                3 => { header["start_seq"] = json!(9); header["end_seq"] = json!(9); record["seq"] = json!(9); },
                4 => { header["count"] = json!(0); },
                _ => { record.as_object_mut().unwrap().remove("ts"); },
            }
            let chunk = if case == 4 { format!("{header}\n") } else { format!("{header}\n{record}\n") };
            let mut bad = health.clone();
            bad["recorder"]["total_bytes_written"] = json!(health["recorder"]["total_bytes_written"].as_u64().unwrap() - original.len() as u64 + chunk.len() as u64);
            std::fs::write(&status, bad.to_string()).unwrap();
            adb.write_remote_file("fixture", "/run/events-000002.jsonl", chunk.into_bytes());
            assert!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection").is_err(), "case {case}");
        }
        // Producer drains need not publish chunks in global sequence order.
        for (chunk_id, seq) in [(2, 3), (3, 2)] {
            let mut header = rows[0].clone(); let mut record = rows[1].clone();
            header["chunk_id"] = json!(chunk_id); header["start_seq"] = json!(seq);
            header["end_seq"] = json!(seq); record["seq"] = json!(seq);
            adb.write_remote_file("fixture", &format!("/run/events-{chunk_id:06}.jsonl"), format!("{header}\n{record}\n").into_bytes());
        }
        std::fs::write(&status, health.to_string()).unwrap();
        assert_eq!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection").unwrap().len(), 4);
        remove_collection_fixture(dir);
    }

    #[test]
    fn nonmatching_or_unavailable_status_retains_unknown_legacy_collection() {
        let (dir, status, health, adb) = collection_fixture("legacy", 336);
        for (field, value) in [("run_id", "other"), ("state", "running")] {
            let mut bad = health.clone(); bad[field] = json!(value);
            std::fs::write(&status, bad.to_string()).unwrap();
            assert_eq!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection").unwrap().len(), 200);
        }
        assert_eq!(collect_event_chunks(&adb, "fixture", "/run", &dir, None, "collection").unwrap().len(), 200);
        remove_collection_fixture(dir);
    }

    #[test]
    fn collection_enforces_file_and_aggregate_byte_limits_without_trusted_status() {
        let (dir, _, _, adb) = collection_fixture("byte_limits", 0);
        adb.write_remote_file("fixture", "/run/events-000001.jsonl", vec![b' '; MAX_EVENT_CHUNK_BYTES + 1]);
        assert!(collect_event_chunks(&adb, "fixture", "/run", &dir, None, "collection")
            .unwrap_err().contains("size bound"));
        for i in 1..=9 {
            adb.write_remote_file("fixture", &format!("/run/events-{i:06}.jsonl"), vec![b' '; MAX_EVENT_CHUNK_BYTES]);
        }
        assert!(collect_event_chunks(&adb, "fixture", "/run", &dir, None, "collection")
            .unwrap_err().contains("128 MiB"));
        remove_collection_fixture(dir);
    }

    #[test]
    fn collection_minimum_wire_bound_accepts_nonempty_minimal_chunk_and_empty_stream() {
        let (dir, status, mut health, adb) = collection_fixture("minimum", 1);
        let mut chunk = b"{\"chunk_id\":1,\"start_seq\":1,\"end_seq\":1,\"count\":1}\n".to_vec();
        chunk.extend_from_slice(b"{\"seq\":1,\"ts\":0,\"type\":0,\"prod\":0,\"tid\":0,\"gen\":0,\"p0\":0,\"p1\":0,\"p2\":0,\"p3\":0}");
        assert_eq!(chunk.len(), MIN_EVENT_HEADER_BYTES + MIN_EVENT_RECORD_BYTES);
        health["recorder"]["total_bytes_written"] = json!(chunk.len());
        std::fs::write(&status, health.to_string()).unwrap();
        adb.write_remote_file("fixture", "/run/events-000001.jsonl", chunk);
        assert_eq!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection").unwrap().len(), 1);
        for field in ["chunks_committed", "total_emitted", "total_records_written", "total_bytes_written"] {
            health["recorder"][field] = json!(0);
        }
        std::fs::write(&status, health.to_string()).unwrap();
        assert!(collect_event_chunks(&adb, "fixture", "/run", &dir, Some(&status), "collection").unwrap().is_empty());
        remove_collection_fixture(dir);
    }

    #[test]
    fn temporal_decoder_requires_complete_same_scene_producer_and_valid_consumption() {
        let r = temporal_records();
        let o = decode_temporal_records(&r).unwrap();
        assert_eq!(o.effective, TemporalSelection::Sgsr2Quality { variant: SgsrVariant::Compute2 });
        assert_eq!(o.scene_dimensions, [853, 479]);
        assert_eq!(o.output_dimensions, [1280, 720]);
        assert!(o.consumed && o.dispatched && !o.reactive_consumed);
        assert!(o.unrelated_settings_hash.is_empty());
        assert_eq!(o.output_grid, TemporalOutputGrid::Reconstruction);
        assert_eq!(o.surface_dimensions, None);
        let mut with_surface = vec![TemporalRecord { seq: 9, prod: 0, tid: 8, gen: 1,
            kind: 8, p0: 3, p1: 1600, p2: 900, p3: 44 }];
        with_surface.extend(r.clone());
        let observed = decode_temporal_records(&with_surface).unwrap();
        assert_eq!(observed.surface_dimensions, Some([1600, 900]));
        assert_eq!(observed.output_dimensions, [1280, 720]);
        with_surface[0].p0 = 4;
        assert_eq!(decode_temporal_records(&with_surface).unwrap().surface_dimensions, None);
        with_surface[0].p0 = 3; with_surface[0].seq = 1000;
        assert_eq!(decode_temporal_records(&with_surface).unwrap().surface_dimensions, None);
        assert!(decode_temporal_records(&[]).is_none());
        assert!(decode_temporal_records(&r[..3]).is_none());
        for bad in 0..5 {
            let mut records = r.clone();
            match bad {
                0 => records[2].p0 += 1,
                1 => records[2].gen += 1,
                2 => records[2].tid += 1,
                3 => records[2].p1 = 8, // consumed without dispatch/setup/support
                _ => records[3].p1 = 3, // consumed despite fallback
            }
            assert!(decode_temporal_records(&records).is_none());
        }
        let mut incomplete_tail = r.clone();
        let mut next = r[0].clone(); next.seq = 100; next.p0 += 1;
        incomplete_tail.push(next);
        assert!(decode_temporal_records(&incomplete_tail).is_none());
        let mut skipped = r.clone(); skipped[0].p2 = 0; skipped[2].p1 = 23; skipped[3].p1 = 5;
        let o = decode_temporal_records(&skipped).unwrap();
        assert!(!o.consumed && o.skipped && o.fallback_reason.is_some());
    }

    #[test]
    fn temporal_manifest_enrichment_never_invents_legacy_provenance() {
        let dir = std::env::temp_dir().join(format!("task81_temporal_enrich_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let manifest = dir.join("manifest.json"); let status = dir.join("status.json");
        let chunk = dir.join("events-000001.jsonl");
        let receipt = json!({"run_id":"temporal_fixture", "status":"acknowledged", "request_digest":"receipt_only"});
        let mut health = json!({"schema_version":"1.0", "run_id":"temporal_fixture", "timestamp_ns":1,
            "healthy":true, "state":"stopped", "lifecycle":{"state":"resumed","is_foreground":true},
            "surface_generation":3, "progress":{"render_age_ms":0,"is_active_stall":false,"expected_inactivity":false},
            "recorder":{"total_emitted":4,"total_dropped":0,"chunks_committed":1,"total_records_written":4,
                "total_bytes_written":1024,"remaining_budget":1024,"reserve_held":false,"incidents_written":0,"active_producers":1}});
        let mut jsonl = String::from("{\"chunk_id\":1,\"count\":4,\"start_seq\":1,\"end_seq\":4}\n");
        for (i, mut r) in temporal_records().into_iter().enumerate() {
            r.seq = i as u64 + 1;
            jsonl.push_str(&json!({"seq":r.seq,"type":r.kind,"prod":r.prod,"tid":r.tid,"gen":r.gen,
                "p0":r.p0,"p1":r.p1,"p2":r.p2,"p3":r.p3}).to_string());
            jsonl.push('\n');
        }
        std::fs::write(&chunk, &jsonl).unwrap();
        assert!(read_temporal_artifact(&chunk, 4).is_err());
        std::fs::write(&manifest, receipt.to_string()).unwrap();
        std::fs::write(&status, health.to_string()).unwrap();
        enrich_temporal_manifest(&manifest, Some(&status), &[chunk.clone()], "temporal_fixture", false).unwrap();
        let enriched: serde_json::Value = serde_json::from_slice(&std::fs::read(&manifest).unwrap()).unwrap();
        assert_eq!(enriched["status"], "acknowledged");
        assert_eq!(enriched["temporal_observation"]["effective"]["mode"], "sgsr2_quality");
        assert_eq!(enriched["temporal_observation"]["unrelated_settings_hash"], "");
        assert!(enriched.get("effective_treatments").is_none() && enriched.get("build_identity").is_none());
        for condition in 0..7 {
            std::fs::write(&manifest, receipt.to_string()).unwrap();
            health["recorder"]["total_dropped"] = json!(if condition == 0 { 1 } else { 0 });
            health["recorder"]["chunks_committed"] = json!(if condition == 1 { 2 } else { 1 });
            health["recorder"]["total_emitted"] = json!(if condition == 3 { 5 } else { 4 });
            health["state"] = json!(if condition == 4 { "active" } else { "stopped" });
            std::fs::write(&status, health.to_string()).unwrap();
            let content = match condition {
                2 => format!("{}{{\"seq\":", jsonl),
                5 => format!("{}{{\"p0\":18}}\n", jsonl),
                6 => jsonl.replacen("\"count\":4", "\"count\":3", 1),
                _ => jsonl.clone(),
            };
            std::fs::write(&chunk, content).unwrap();
            enrich_temporal_manifest(&manifest, Some(&status), &[chunk.clone()], "temporal_fixture", false).unwrap();
            let missing: serde_json::Value = serde_json::from_slice(&std::fs::read(&manifest).unwrap()).unwrap();
            assert!(missing.get("temporal_observation").is_none());
        }
        for file in [&manifest, &status, &chunk] { std::fs::remove_file(file).unwrap(); }
        std::fs::remove_dir(dir).unwrap();
    }

    #[test]
    fn enrichment_checks_global_sequence_coverage_across_producers() {
        let dir = std::env::temp_dir().join(format!("temporal_global_sequence_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let manifest = dir.join("manifest.json");
        let status = dir.join("status.json");
        let chunks = [dir.join("events-000001.jsonl"), dir.join("events-000002.jsonl")];
        let health = json!({"schema_version":"1.0", "run_id":"sequence_fixture", "timestamp_ns":1,
            "healthy":true, "state":"stopped", "lifecycle":{"state":"resumed","is_foreground":true},
            "surface_generation":3, "progress":{"render_age_ms":0,"is_active_stall":false,"expected_inactivity":false},
            "recorder":{"total_emitted":8,"total_dropped":0,"chunks_committed":2,"total_records_written":8,
                "total_bytes_written":2048,"remaining_budget":1024,"reserve_held":false,"incidents_written":0,"active_producers":2}});
        std::fs::write(&status, health.to_string()).unwrap();
        let mut first = String::from("{\"chunk_id\":1,\"count\":7,\"start_seq\":1,\"end_seq\":8}\n");
        let row = |seq, kind, prod, p0, p1, p2, p3| json!({"seq":seq,"type":kind,"prod":prod,"tid":2,"gen":1,
            "p0":p0,"p1":p1,"p2":p2,"p3":p3}).to_string() + "\n";
        first.push_str(&row(1u64, 0, 1, 0i64, 0i64, 0i64, 0i64));
        for (i, r) in temporal_records().into_iter().enumerate() {
            first.push_str(&json!({"seq":i as u64 + 3,"type":r.kind,"prod":r.prod,"tid":r.tid,"gen":r.gen,
                "p0":r.p0,"p1":r.p1,"p2":r.p2,"p3":r.p3}).to_string());
            first.push('\n');
        }
        first.push_str(&row(7, 36, 1, 17, (10i64 << 32) | 1, 9, -1));
        first.push_str(&row(8, 36, 1, 17, 10i64 << 32, 9, 0));
        std::fs::write(&chunks[0], first).unwrap();
        for duplicate in [false, true] {
            let seq = if duplicate { 1 } else { 2 };
            let second = json!({"chunk_id":2,"count":1,"start_seq":seq,"end_seq":seq}).to_string()
                + "\n" + &row(seq, 0, 2, 0, 0, 0, 0);
            std::fs::write(&chunks[1], second).unwrap();
            std::fs::write(&manifest, json!({"run_id":"sequence_fixture","status":"acknowledged"}).to_string()).unwrap();
            enrich_temporal_manifest(&manifest, Some(&status), &chunks, "sequence_fixture", true).unwrap();
            let enriched: serde_json::Value = serde_json::from_slice(&std::fs::read(&manifest).unwrap()).unwrap();
            assert_eq!(enriched.get("temporal_observation").is_some(), !duplicate);
            assert_eq!(enriched["gpu_components"]["healthy"], !duplicate);
            if duplicate {
                assert_eq!(enriched["gpu_components"]["observations"], json!([]));
            } else {
                assert_eq!(enriched["gpu_components"]["observations"][0]["duration_ns"], 0);
            }
        }
        for file in [&manifest, &status, &chunks[0], &chunks[1]] { std::fs::remove_file(file).unwrap(); }
        std::fs::remove_dir(dir).unwrap();
    }

    #[test]
    fn collected_event36_stream_publishes_reduced_components_without_frame_totals() {
        let dir = std::env::temp_dir().join(format!("task82_components_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let manifest = dir.join("manifest.json"); let status = dir.join("status.json");
        let chunk = dir.join("events-000001.jsonl");
        let payload = |pass, serial, state, ns| [10, ((pass as i64) << 32) | state, serial, ns];
        let events = vec![payload(20, 40, 1, -1), payload(21, 40, 1, -1), payload(20, 40, 0, 0),
            payload(20, 41, 1, -1), payload(20, 41, 4, 250), payload(22, 42, 1, -1)];
        for case in 0..7 {
            let mut records = events.clone();
            if case == 1 { records.push(payload(20, 40, 0, 0)); }
            if case == 2 { records.push(payload(23, 43, 99, -1)); }
            if case >= 5 { records.clear(); }
            let requested = case != 3 && case != 6;
            let count = records.len();
            let mut jsonl = json!({"chunk_id":1,"start_seq":1,"end_seq":count,"count":count}).to_string();
            jsonl.push('\n');
            for (index, p) in records.iter().enumerate() {
                jsonl.push_str(&json!({"seq":index + 1,"type":36,"prod":0,"tid":8,"gen":1,
                    "p0":p[0],"p1":p[1],"p2":p[2],"p3":p[3]}).to_string());
                jsonl.push('\n');
            }
            std::fs::write(&chunk, jsonl).unwrap();
            std::fs::write(&manifest, json!({"run_id":"components_fixture","status":"acknowledged"}).to_string()).unwrap();
            let health = json!({"schema_version":"1.0","run_id":"components_fixture","timestamp_ns":1,
                "healthy":true,"state":if case == 4 { "active" } else { "stopped" },
                "lifecycle":{"state":"resumed","is_foreground":true},"surface_generation":3,
                "progress":{"render_age_ms":0,"is_active_stall":false,"expected_inactivity":false},
                "recorder":{"total_emitted":count,"total_dropped":0,"chunks_committed":if count == 0 {0} else {1},
                    "total_records_written":count,"total_bytes_written":1024,"remaining_budget":1024,
                    "reserve_held":false,"incidents_written":0,"active_producers":1}});
            std::fs::write(&status, health.to_string()).unwrap();
            let chunks = if count == 0 { Vec::new() } else { vec![chunk.clone()] };
            enrich_temporal_manifest(&manifest, Some(&status), &chunks, "components_fixture", requested).unwrap();
            let result: serde_json::Value = serde_json::from_slice(&std::fs::read(&manifest).unwrap()).unwrap();
            assert!(result.get("frame_gpu_time_ns").is_none() && result.get("gpu_utilization").is_none());
            assert!(result.get("build_identity").is_none() && result.get("effective_treatments").is_none());
            if case == 6 { assert!(result.get("gpu_components").is_none()); continue; }
            let report: DiagnosticGpuComponentReport = serde_json::from_value(result["gpu_components"].clone()).unwrap();
            if case == 0 {
                assert!(report.healthy && report.reason.is_none());
                assert_eq!(report.observations.len(), 4);
                assert_eq!(report.observations[0].duration_ns, Some(0));
                assert_eq!(report.observations[1].duration_ns, Some(250));
                for observation in &report.observations[2..] {
                    assert_eq!(observation.state, crate::diag::models::GpuComponentState::LostTail);
                    assert_eq!(observation.duration_ns, None);
                }
            } else { assert!(!report.healthy && report.reason.is_some() && report.observations.is_empty()); }
        }
        for file in [&manifest, &status, &chunk] { std::fs::remove_file(file).unwrap(); }
        std::fs::remove_dir(dir).unwrap();
    }

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
