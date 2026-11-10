use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

pub const CURRENT_SCHEMA_MAJOR: u32 = 1;
pub const CURRENT_SCHEMA_MINOR: u32 = 0;
pub const MAX_DIAGNOSTIC_JSON_BYTES: usize = 16 * 1024 * 1024; // 16 MiB default budget

/// Schema version representing major and minor versions.
/// Formats as "major.minor" in JSON, while accepting either "1.0" or `{"major": 1, "minor": 0}`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SchemaVersion {
    pub major: u32,
    pub minor: u32,
}

impl SchemaVersion {
    pub const CURRENT: SchemaVersion = SchemaVersion {
        major: CURRENT_SCHEMA_MAJOR,
        minor: CURRENT_SCHEMA_MINOR,
    };

    pub fn current() -> Self {
        Self::CURRENT
    }

    pub fn new(major: u32, minor: u32) -> Self {
        Self { major, minor }
    }

    pub fn parse(s: &str) -> Result<Self, String> {
        let parts: Vec<&str> = s.split('.').collect();
        if parts.len() != 2 {
            return Err(format!("invalid schema_version string: '{}'", s));
        }
        let major = parts[0]
            .parse::<u32>()
            .map_err(|e| format!("invalid major in '{}': {}", s, e))?;
        let minor = parts[1]
            .parse::<u32>()
            .map_err(|e| format!("invalid minor in '{}': {}", s, e))?;
        Ok(Self { major, minor })
    }
}

impl std::fmt::Display for SchemaVersion {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}.{}", self.major, self.minor)
    }
}

impl Serialize for SchemaVersion {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        serializer.serialize_str(&self.to_string())
    }
}

impl<'de> Deserialize<'de> for SchemaVersion {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        #[derive(Deserialize)]
        #[serde(untagged)]
        enum Repr {
            Str(String),
            Obj { major: u32, minor: u32 },
        }

        match Repr::deserialize(deserializer)? {
            Repr::Str(s) => SchemaVersion::parse(&s).map_err(serde::de::Error::custom),
            Repr::Obj { major, minor } => Ok(SchemaVersion { major, minor }),
        }
    }
}

/// Stable machine-readable reason codes across diagnostic sessions and analysis.
pub mod reasons {
    // Execution & Lifecycle
    pub const CLEAN_COMPLETION: &str = "CLEAN_COMPLETION";
    pub const STARTUP_TIMEOUT: &str = "STARTUP_TIMEOUT";
    pub const SCENARIO_TIMEOUT: &str = "SCENARIO_TIMEOUT";
    pub const CHECKPOINT_TIMEOUT: &str = "CHECKPOINT_TIMEOUT";
    pub const RECOVERY_TIMEOUT: &str = "RECOVERY_TIMEOUT";
    pub const ASSERTION_FAILED: &str = "ASSERTION_FAILED";
    pub const ROUTE_DIVERGENCE: &str = "ROUTE_DIVERGENCE";
    pub const NATIVE_CRASH: &str = "NATIVE_CRASH";
    pub const PROCESS_KILLED: &str = "PROCESS_KILLED";
    pub const TRANSPORT_LOST: &str = "TRANSPORT_LOST";
    pub const STORAGE_EXHAUSTED: &str = "STORAGE_EXHAUSTED";
    pub const WRITER_ERROR: &str = "WRITER_ERROR";

    // Comparison & Eligibility
    pub const MISMATCHED_RESOLUTION: &str = "MISMATCHED_RESOLUTION";
    pub const DIFFERENT_SCENARIOS: &str = "DIFFERENT_SCENARIOS";
    pub const DIFFERENT_MAPPING_POLICY: &str = "DIFFERENT_MAPPING_POLICY";
    pub const INCOMPATIBLE_PROFILES: &str = "INCOMPATIBLE_PROFILES";
    pub const FAILED_BASELINE: &str = "FAILED_BASELINE";
    pub const FAILED_CANDIDATE: &str = "FAILED_CANDIDATE";
    pub const PARTIAL_ROUTE: &str = "PARTIAL_ROUTE";
    pub const MISSING_ACKNOWLEDGEMENT: &str = "MISSING_ACKNOWLEDGEMENT";
    pub const CACHE_CLASS_MISMATCH: &str = "CACHE_CLASS_MISMATCH";
    pub const DEVICE_DRIVER_MISMATCH: &str = "DEVICE_DRIVER_MISMATCH";
    pub const BUILD_IDENTITY_MISMATCH: &str = "BUILD_IDENTITY_MISMATCH";
    pub const POLICY_MISMATCH: &str = "POLICY_MISMATCH";

    // Measurement & Capabilities
    pub const MISSING_GPU_SAMPLES: &str = "MISSING_GPU_SAMPLES";
    pub const GPU_QUERY_TIMEOUT: &str = "GPU_QUERY_TIMEOUT";
    pub const CAPABILITY_UNSUPPORTED: &str = "CAPABILITY_UNSUPPORTED";
    pub const CAPABILITY_DISABLED: &str = "CAPABILITY_DISABLED";
    pub const PENDING_SAMPLES: &str = "PENDING_SAMPLES";
    pub const AUDIO_MUTED: &str = "AUDIO_MUTED";
    pub const AUDIO_BACKEND_UNCHECKED: &str = "AUDIO_BACKEND_UNCHECKED";
    pub const THERMAL_UNSUPPORTED: &str = "THERMAL_UNSUPPORTED";
    pub const SEQUENCE_GAP: &str = "SEQUENCE_GAP";
    pub const SEQUENCE_CONFLICT: &str = "SEQUENCE_CONFLICT";
    pub const UNAVAILABLE_COUNTER: &str = "UNAVAILABLE_COUNTER";
}

/// Standard measurement unit definitions.
pub mod units {
    pub const MILLISECONDS: &str = "ms";
    pub const MICROSECONDS: &str = "us";
    pub const NANOSECONDS: &str = "ns";
    pub const FRAMES_PER_SECOND: &str = "fps";
    pub const BYTES: &str = "bytes";
    pub const DEGREES_CELSIUS: &str = "celsius";
    pub const COUNT: &str = "count";
    pub const RATIO: &str = "ratio";
    pub const PERCENTAGE: &str = "percent";
    pub const WORDS: &str = "words";
}

/// Standard metric identifiers for extended bounded diagnostics.
pub mod metrics {
    // Software write-watch & memory tracking
    pub const TRACKER_BACKEND: &str = "ww_tracker_backend";
    pub const TRACKER_PAGE_SIZE: &str = "ww_tracker_page_size";
    pub const TRACKER_COVERAGE_BYTES: &str = "ww_tracker_coverage_bytes";
    pub const TRACKER_FAULT_COUNT: &str = "ww_fault_count";
    pub const TRACKER_REARM_COUNT: &str = "ww_rearm_count";
    pub const TRACKER_LEASE_COUNT: &str = "ww_lease_count";
    pub const TRACKER_QUERY_COUNT: &str = "ww_query_count";
    pub const TRACKER_MEMORY_BYTES: &str = "ww_tracker_memory_bytes";

    // Memory inspection (strictly distinct from measured DRAM traffic)
    pub const HASH_WORDS_VISITED: &str = "hash_words_visited";
    pub const SNAPSHOT_COMPARE_BYTES: &str = "snapshot_compare_requested_bytes";
    pub const MEMORY_COPIES_COUNT: &str = "memory_copies_count";
    pub const MEMORY_UPLOADS_COUNT: &str = "memory_uploads_count";

    // GPU synchronization
    pub const GPU_WAIT_REQUIRED_SERIAL: &str = "gpu_wait_required_serial";
    pub const GPU_WAIT_CHOSEN_SERIAL: &str = "gpu_wait_chosen_serial";
    pub const GPU_WAIT_AVOIDED_COUNT: &str = "gpu_wait_avoided_count";

    // Clear folding
    pub const CLEAR_FOLDED_COUNT: &str = "clear_folded_count";
    pub const CLEAR_ELIGIBLE_COUNT: &str = "clear_eligible_count";
}

/// Validates metric semantics to ensure logical reads never masquerade as physical bandwidth claims:
/// - `hash_words_visited` must use unit "words" (logical read visits, not bytes or DRAM traffic)
/// - `snapshot_compare_requested_bytes` must use unit "bytes" (requested compare size, not physical DRAM traffic)
pub fn validate_metric_semantics(m: &DiagnosticMeasurement) -> Result<(), String> {
    if m.name == metrics::HASH_WORDS_VISITED && m.units != units::WORDS {
        return Err(format!(
            "Metric '{}' must use units '{}', not '{}' (cannot claim physical traffic)",
            m.name, units::WORDS, m.units
        ));
    }
    if m.name == metrics::SNAPSHOT_COMPARE_BYTES && m.units != units::BYTES {
        return Err(format!(
            "Metric '{}' must use units '{}', not '{}'",
            m.name, units::BYTES, m.units
        ));
    }
    Ok(())
}

/// Enforces that heavy capture collectors (e.g. perfetto_trace, deep_memory_dump, raw_frame_capture)
/// stay strictly out of lightweight performance samples.
pub fn validate_profile_collectors(profile: &DiagnosticProfileRequest) -> Result<(), String> {
    if profile.profile_name == "lightweight" {
        for collector in &profile.collectors {
            if collector == "perfetto_trace"
                || collector == "deep_memory_dump"
                || collector == "raw_frame_capture"
            {
                return Err(format!(
                    "Heavy capture collector '{}' is forbidden in lightweight profile",
                    collector
                ));
            }
        }
    }
    Ok(())
}

/// A single diagnostic measurement where null is strictly distinguished from measured zero.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticMeasurement {
    pub name: String,
    /// Must serialize to `null` if unavailable/pending/failed. Must NOT be omitted.
    pub value: Option<f64>,
    pub units: String,
    pub source: String,
    pub capability: String,
    pub sample_count: u64,
    pub coverage: f64,
    pub validity: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason_code: Option<String>,
    #[serde(flatten)]
    pub extra: BTreeMap<String, Value>,
}

impl DiagnosticMeasurement {
    pub fn new_valid(name: &str, value: f64, units: &str, source: &str, sample_count: u64, coverage: f64) -> Self {
        Self {
            name: name.to_string(),
            value: Some(value),
            units: units.to_string(),
            source: source.to_string(),
            capability: "supported".to_string(),
            sample_count,
            coverage,
            validity: "valid".to_string(),
            reason_code: None,
            extra: BTreeMap::new(),
        }
    }

    pub fn new_unavailable(name: &str, units: &str, source: &str, reason_code: &str) -> Self {
        Self {
            name: name.to_string(),
            value: None,
            units: units.to_string(),
            source: source.to_string(),
            capability: "unavailable".to_string(),
            sample_count: 0,
            coverage: 0.0,
            validity: "unsupported".to_string(),
            reason_code: Some(reason_code.to_string()),
            extra: BTreeMap::new(),
        }
    }

    pub fn is_measured_zero(&self) -> bool {
        self.value == Some(0.0)
    }

    pub fn is_null(&self) -> bool {
        self.value.is_none()
    }
}

/// Continuous state counters aggregated across the diagnostic session.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, Default)]
pub struct ContinuousCounters {
    // Thread & wait
    pub total_wait_events: u64,
    pub total_wait_ns: u64,
    // Shader queue
    pub shader_queued_jobs: i64,
    pub shader_completed_jobs: i64,
    pub shader_active_workers: i64,
    pub shader_avg_compile_ns: i64,
    // File I/O
    pub file_ops_count: u64,
    pub file_total_duration_ns: u64,
    pub file_total_bytes: u64,
    // Memory & Streaming
    pub mem_compare_bytes: u64,
    pub mem_copy_bytes: u64,
    pub mem_stream_bytes: u64,
    pub mem_allocated_bytes: i64,
    pub mem_peak_allocated_bytes: i64,
    // Audio
    pub audio_appended_frames: i64,
    pub audio_released_frames: i64,
    pub audio_underruns: i64,
    pub audio_starved: i64,
}

impl ContinuousCounters {
    /// Converts continuous counters into honest DiagnosticMeasurement models,
    /// ensuring that unavailable metrics map strictly to null instead of zero.
    pub fn to_measurements(&self) -> Vec<DiagnosticMeasurement> {
        let mut res = Vec::new();

        // Thread waits
        if self.total_wait_events > 0 {
            res.push(DiagnosticMeasurement::new_valid(
                "thread_wait_total_ns",
                self.total_wait_ns as f64,
                "ns",
                "runtime_thread_wait",
                self.total_wait_events,
                1.0,
            ));
        } else {
            res.push(DiagnosticMeasurement::new_unavailable(
                "thread_wait_total_ns",
                "ns",
                "runtime_thread_wait",
                reasons::UNAVAILABLE_COUNTER,
            ));
        }

        // Shader queue
        if self.shader_queued_jobs >= 0 {
            res.push(DiagnosticMeasurement::new_valid(
                "shader_queued_jobs",
                self.shader_queued_jobs as f64,
                "count",
                "shader_queue",
                1,
                1.0,
            ));
        } else {
            res.push(DiagnosticMeasurement::new_unavailable(
                "shader_queued_jobs",
                "count",
                "shader_queue",
                reasons::UNAVAILABLE_COUNTER,
            ));
        }

        if self.shader_avg_compile_ns >= 0 {
            res.push(DiagnosticMeasurement::new_valid(
                "shader_avg_compile_ns",
                self.shader_avg_compile_ns as f64,
                "ns",
                "shader_queue",
                self.shader_completed_jobs.max(1) as u64,
                1.0,
            ));
        }

        // File I/O
        if self.file_ops_count > 0 {
            res.push(DiagnosticMeasurement::new_valid(
                "file_io_total_bytes",
                self.file_total_bytes as f64,
                "bytes",
                "runtime_nnfs",
                self.file_ops_count,
                1.0,
            ));
            res.push(DiagnosticMeasurement::new_valid(
                "file_io_total_duration_ns",
                self.file_total_duration_ns as f64,
                "ns",
                "runtime_nnfs",
                self.file_ops_count,
                1.0,
            ));
        }

        // Memory & Streaming
        res.push(DiagnosticMeasurement::new_valid(
            "mem_compare_bytes",
            self.mem_compare_bytes as f64,
            "bytes",
            "runtime_mem_stream",
            1,
            1.0,
        ));
        res.push(DiagnosticMeasurement::new_valid(
            "mem_copy_bytes",
            self.mem_copy_bytes as f64,
            "bytes",
            "runtime_mem_stream",
            1,
            1.0,
        ));
        res.push(DiagnosticMeasurement::new_valid(
            "mem_stream_bytes",
            self.mem_stream_bytes as f64,
            "bytes",
            "runtime_mem_stream",
            1,
            1.0,
        ));

        if self.mem_allocated_bytes >= 0 {
            res.push(DiagnosticMeasurement::new_valid(
                "mem_allocated_bytes",
                self.mem_allocated_bytes as f64,
                "bytes",
                "runtime_mem_alloc",
                1,
                1.0,
            ));
        }
        if self.mem_peak_allocated_bytes >= 0 {
            res.push(DiagnosticMeasurement::new_valid(
                "mem_peak_allocated_bytes",
                self.mem_peak_allocated_bytes as f64,
                "bytes",
                "runtime_mem_alloc",
                1,
                1.0,
            ));
        }

        // Audio
        if self.audio_appended_frames >= 0 {
            res.push(DiagnosticMeasurement::new_valid(
                "audio_appended_frames",
                self.audio_appended_frames as f64,
                "frames",
                "runtime_audio",
                1,
                1.0,
            ));
            res.push(DiagnosticMeasurement::new_valid(
                "audio_released_frames",
                self.audio_released_frames as f64,
                "frames",
                "runtime_audio",
                1,
                1.0,
            ));
            res.push(DiagnosticMeasurement::new_valid(
                "audio_underruns",
                self.audio_underruns as f64,
                "count",
                "runtime_audio",
                1,
                1.0,
            ));
            res.push(DiagnosticMeasurement::new_valid(
                "audio_starved",
                self.audio_starved as f64,
                "count",
                "runtime_audio",
                1,
                1.0,
            ));
        } else {
            res.push(DiagnosticMeasurement::new_unavailable(
                "audio_appended_frames",
                "frames",
                "runtime_audio",
                reasons::UNAVAILABLE_COUNTER,
            ));
        }

        res
    }
}

/// Statistical distribution over timing samples.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TimingStats {
    pub min: f64,
    pub max: f64,
    pub median: f64,
    pub p95: f64,
    pub p99: f64,
    pub mean: f64,
    pub stddev: f64,
}

/// Compact live status snapshot serialized atomically to `status.json`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticLiveStatus {
    pub schema_version: SchemaVersion,
    pub run_id: String,
    pub timestamp_ns: u64,
    pub healthy: bool,
    pub state: String,
    pub lifecycle: LiveLifecycleStatus,
    pub surface_generation: u32,
    pub thermal: Option<LiveThermalStatus>,
    pub progress: LiveProgressStatus,
    pub recorder: LiveRecorderHealth,
    #[serde(flatten)]
    pub extra: BTreeMap<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LiveLifecycleStatus {
    pub state: String,
    pub is_foreground: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LiveThermalStatus {
    pub headroom: Option<f64>,
    pub status: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LiveProgressStatus {
    pub render_age_ms: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub semantic_freshness_ms: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_chapter: Option<i32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_verse: Option<i32>,
    pub is_active_stall: bool,
    pub expected_inactivity: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LiveRecorderHealth {
    pub total_emitted: u64,
    pub total_dropped: u64,
    pub chunks_committed: u32,
    pub total_records_written: u64,
    pub total_bytes_written: u64,
    pub remaining_budget: u64,
    pub reserve_held: bool,
    pub incidents_written: u32,
    pub active_producers: u32,
}

/// Incident bundle manifest serialized to `incidents/<id>/incident.json`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticIncidentManifest {
    pub schema_version: SchemaVersion,
    pub incident_id: String,
    pub run_id: String,
    pub trigger_type: String,
    pub reason_code: String,
    pub message: String,
    pub timestamp_ns: u64,
    pub prehistory: IncidentPrehistorySummary,
    pub capture_coverage: IncidentCaptureCoverage,
    pub recovery: Option<IncidentRecovery>,
    #[serde(flatten)]
    pub extra: BTreeMap<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct IncidentPrehistorySummary {
    pub event_count: usize,
    pub start_seq: u64,
    pub end_seq: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct IncidentCaptureCoverage {
    pub pre_trigger_screenshot: String,
    pub deep_trace: String,
    pub post_trigger_coverage: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub collector_start_ns: Option<u64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct IncidentRecovery {
    pub recovered: bool,
    pub reason: String,
    pub resume_timestamp_ns: u64,
}

// -------------------------------------------------------------------------------------------------
// Request Model (`request.json`)
// -------------------------------------------------------------------------------------------------

/// Typed treatment configuration for experimental optimizations and scheduling treatments.
///
/// Only explicitly allowlisted treatments are accepted; arbitrary environment variables
/// or unknown keys are strictly rejected.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(deny_unknown_fields)]
pub struct DiagnosticTreatments {
    /// Experimental software write-watch backend (WT_WW_SOFT). Default: false (off).
    #[serde(default)]
    pub ww_soft: bool,

    /// Producer-scoped GPU waiting (WT_VK_WRITERWAIT). Default: false (off).
    #[serde(default)]
    pub vk_writerwait: bool,

    /// Render pass full clear folding into loadOp CLEAR (WT_VK_LOADCLEAR). Default: false (off).
    #[serde(default)]
    pub vk_loadclear: bool,

    /// Submission split draw threshold (WT_VK_SPLIT). Bound separately from candidate optimizations.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub vk_split: Option<u32>,
}

impl DiagnosticTreatments {
    pub fn all_off() -> Self {
        Self {
            ww_soft: false,
            vk_writerwait: false,
            vk_loadclear: false,
            vk_split: None,
        }
    }

    pub fn are_candidates_off(&self) -> bool {
        !self.ww_soft && !self.vk_writerwait && !self.vk_loadclear
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticRequest {
    pub schema_version: SchemaVersion,
    pub run_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub created_utc: Option<String>,
    pub build: DiagnosticRequestedBuild,
    pub scenario: DiagnosticScenarioRequest,
    pub profile: DiagnosticProfileRequest,
    pub configuration: DiagnosticConfigurationRequest,
    pub policy: DiagnosticPolicyRef,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub treatments: Option<DiagnosticTreatments>,
    #[serde(flatten)]
    pub extra: BTreeMap<String, Value>,
}

impl DiagnosticRequest {
    pub fn effective_treatments(&self) -> DiagnosticTreatments {
        self.treatments.clone().unwrap_or_default()
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticRequestedBuild {
    pub variant: String,
    pub target: String,
    pub native_optimization: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub git_commit: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub build_id: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticScenarioRequest {
    pub scenario_id: String,
    pub route_id: String,
    pub fixture_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub save_fixture: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub replay_fixture: Option<String>,
    pub deadlines: ScenarioDeadlines,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ScenarioDeadlines {
    pub startup_deadline_secs: f64,
    pub scenario_deadline_secs: f64,
    pub recovery_deadline_secs: f64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticProfileRequest {
    pub profile_name: String,
    pub memory_budget_bytes: u64,
    pub storage_budget_bytes: u64,
    pub incident_disk_reserve_bytes: u64,
    pub max_incidents: u32,
    pub collectors: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticConfigurationRequest {
    pub resolution: [u32; 2],
    pub fps_target: f64,
    pub audio_mode: String,
    pub cache_mode: String,
    pub input_policy: DiagnosticInputPolicy,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticInputPolicy {
    pub source: String,
    pub mapping: String,
    pub menu_confirm: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticPolicyRef {
    pub policy_id: String,
    pub policy_hash: String,
    pub required_assertions: Vec<String>,
}

// -------------------------------------------------------------------------------------------------
// Manifest Model (`manifest.json`)
// -------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticManifest {
    pub schema_version: SchemaVersion,
    pub run_id: String,
    pub diagnostic_version: String,
    pub toolchain: DiagnosticToolchain,
    pub source: DiagnosticSourceIdentity,
    pub build_identity: DiagnosticBuildIdentity,
    pub device: DiagnosticDeviceIdentity,
    pub fixtures: DiagnosticFixtureIdentity,
    pub cache: DiagnosticCacheProvenance,
    pub effective_paths: DiagnosticEffectivePaths,
    pub effective_settings: DiagnosticEffectiveSettings,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub effective_treatments: Option<DiagnosticTreatments>,
    #[serde(default)]
    pub capabilities: BTreeMap<String, DiagnosticCapability>,
    #[serde(default)]
    pub mismatches: Vec<DiagnosticMismatch>,
    #[serde(flatten)]
    pub extra: BTreeMap<String, Value>,
}

impl DiagnosticManifest {
    pub fn is_treatment_provenance_known(&self) -> bool {
        self.effective_treatments.is_some()
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticToolchain {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ndk_version: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rustc_version: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub clang_version: Option<String>,
    pub host_os: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticSourceIdentity {
    pub git_commit: String,
    pub is_dirty: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub local_diff_digest: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticBuildIdentity {
    pub package_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub apk_hash: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub runtime_lib_hash: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub game_shim_hash: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub main_executable_hash: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub generated_sdk_identity: Option<String>,
    pub build_variant: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticDeviceIdentity {
    pub model: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub manufacturer: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub android_version: Option<String>,
    pub api_level: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub driver_version: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub vulkan_driver: Option<String>,
    pub page_size: u32,
    pub abi: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticFixtureIdentity {
    pub save_fixture_id: String,
    pub save_hash: String,
    pub replay_fixture_id: String,
    pub replay_hash: String,
    pub scenario_hash: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticCacheProvenance {
    pub cache_class: String,
    pub shader_cache_path: String,
    pub vk_cache_path: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub seed_hash: Option<String>,
    pub os_driver_cache: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticEffectivePaths {
    pub session_root: String,
    pub save_dir: String,
    pub config_file: String,
    pub shader_cache_dir: String,
    pub vk_cache_dir: String,
    pub artifacts_dir: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticEffectiveSettings {
    pub resolution: [u32; 2],
    pub fps_target: f64,
    pub audio_mode: String,
    pub input_source_policy: String,
    pub input_mapping_policy: String,
    pub menu_confirm_policy: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticCapability {
    pub status: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticMismatch {
    pub field: String,
    pub requested: String,
    pub effective: String,
    pub reason_code: String,
}

// -------------------------------------------------------------------------------------------------
// Outcome Model (`outcome.json`)
// -------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticOutcome {
    pub schema_version: SchemaVersion,
    pub run_id: String,
    pub execution_outcome: String,
    pub reason_code: String,
    pub exit_status: DiagnosticExitStatus,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub last_confirmed_checkpoint: Option<String>,
    #[serde(default)]
    pub covered_segments: Vec<String>,
    #[serde(default)]
    pub checkpoints: Vec<DiagnosticCheckpointResult>,
    #[serde(default)]
    pub correctness_assertions: Vec<DiagnosticAssertionResult>,
    #[serde(default)]
    pub storage_exhausted: bool,
    #[serde(default)]
    pub incomplete_recovery: bool,
    #[serde(flatten)]
    pub extra: BTreeMap<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticExitStatus {
    pub clean_exit: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub exit_code: Option<i32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub signal: Option<i32>,
    pub termination_origin: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticCheckpointResult {
    pub checkpoint_id: String,
    pub occurrence: u32,
    pub status: String,
    pub target_segment: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub elapsed_host_ms: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub elapsed_game_ms: Option<f64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason_code: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub evidence_ref: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticAssertionResult {
    pub assertion_id: String,
    pub kind: String,
    pub required: bool,
    pub status: String,
    pub observation_boundary: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tolerance: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reason_code: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub evidence_ref: Option<String>,
}

// -------------------------------------------------------------------------------------------------
// Summary Model (`summary.json`)
// -------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticSummary {
    pub schema_version: SchemaVersion,
    pub run_id: String,
    pub overall_outcome: String,
    pub validity: String,
    pub correctness: String,
    #[serde(default)]
    pub segments: BTreeMap<String, SegmentStatistics>,
    #[serde(default)]
    pub measurements: BTreeMap<String, DiagnosticMeasurement>,
    #[serde(default)]
    pub evidence_references: Vec<DiagnosticEvidenceRef>,
    #[serde(default)]
    pub hypotheses: Vec<DiagnosticHypothesis>,
    #[serde(flatten)]
    pub extra: BTreeMap<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SegmentStatistics {
    pub segment_id: String,
    pub target_fps: f64,
    pub sample_count: u64,
    pub coverage: f64,
    pub duration_ms: f64,
    #[serde(default)]
    pub frame_time_ms: Option<TimingStats>,
    #[serde(default)]
    pub gpu_time_ms: Option<TimingStats>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gpu_absence_reason: Option<String>,
    #[serde(default)]
    pub held_present_count: u64,
    #[serde(default)]
    pub progress_present_count: u64,
    #[serde(default)]
    pub dropped_frame_count: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticEvidenceRef {
    pub ref_id: String,
    pub path: String,
    pub chunk_type: String,
    pub record_count: u64,
    pub byte_size: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticHypothesis {
    pub hypothesis_id: String,
    pub status: String,
    pub rationale: String,
    #[serde(default)]
    pub evidence_refs: Vec<String>,
}

// -------------------------------------------------------------------------------------------------
// Comparison Model (`comparison.json`)
// -------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DiagnosticComparison {
    pub schema_version: SchemaVersion,
    pub comparison_id: String,
    pub baseline_run_id: String,
    pub candidate_run_id: String,
    pub policy_id: String,
    pub policy_hash: String,
    pub eligibility: ComparisonEligibility,
    pub dimensions: ComparisonDimensions,
    #[serde(default)]
    pub metric_comparisons: Vec<MetricComparison>,
    #[serde(default)]
    pub trade_offs: Vec<ComparisonTradeOff>,
    #[serde(default)]
    pub hypotheses: Vec<DiagnosticHypothesis>,
    #[serde(flatten)]
    pub extra: BTreeMap<String, Value>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ComparisonEligibility {
    pub is_eligible: bool,
    #[serde(default)]
    pub ineligibility_reasons: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ComparisonDimensions {
    pub execution: String,
    pub correctness: String,
    pub performance: String,
    pub overall_verdict: String,
    #[serde(default)]
    pub verdict_reasons: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MetricComparison {
    pub metric: String,
    pub segment: String,
    #[serde(default)]
    pub baseline_value: Option<f64>,
    #[serde(default)]
    pub candidate_value: Option<f64>,
    #[serde(default)]
    pub delta: Option<f64>,
    #[serde(default)]
    pub pct_change: Option<f64>,
    pub direction: String,
    pub noise_margin: f64,
    pub uncertainty: f64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ComparisonTradeOff {
    pub improved_metric: String,
    pub regressed_metric: String,
    pub details: String,
}
