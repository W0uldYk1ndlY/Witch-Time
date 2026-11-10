use crate::diag::models::{
    DiagnosticCheckpointResult, DiagnosticExitStatus, DiagnosticOutcome, SchemaVersion,
    reasons,
};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ClockPolicy {
    HostTime,
    GameActiveTime,
    InputPollCount,
}

#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
pub struct SemanticMatcher {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub chapter: Option<i32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub verse: Option<i32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub difficulty: Option<i32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub area: Option<i32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub movie_active: Option<bool>,
}

impl SemanticMatcher {
    pub fn matches(&self, obs: &SemanticObservation) -> bool {
        if let Some(ch) = self.chapter {
            if obs.chapter != ch {
                return false;
            }
        }
        if let Some(vs) = self.verse {
            if obs.verse != vs {
                return false;
            }
        }
        if let Some(df) = self.difficulty {
            if obs.difficulty != df {
                return false;
            }
        }
        if let Some(ar) = self.area {
            if obs.area != ar {
                return false;
            }
        }
        if let Some(mv) = self.movie_active {
            if obs.movie_active != mv {
                return false;
            }
        }
        true
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CheckpointSpec {
    pub checkpoint_id: String,
    pub occurrence: u32,
    pub target_segment: String,
    pub expected: SemanticMatcher,
    pub clock_policy: ClockPolicy,
    pub deadline_ms: u64,
    pub required: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ScenarioDefinition {
    pub scenario_id: String,
    pub checkpoints: Vec<CheckpointSpec>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct SemanticObservation {
    pub chapter: i32,
    pub verse: i32,
    pub difficulty: i32,
    pub area: i32,
    pub movie_active: bool,
    pub draws_seen: i32,
    pub produced_at_host_ms: f64,
    pub produced_at_game_ms: f64,
}

#[derive(Debug, Clone)]
pub struct ScenarioStateMachine {
    pub scenario: ScenarioDefinition,
    pub current_checkpoint_index: usize,
    pub last_confirmed_checkpoint: Option<String>,
    pub covered_segments: Vec<String>,
    pub results: Vec<DiagnosticCheckpointResult>,
    pub execution_outcome: String,
    pub reason_code: String,

    /* Clocks */
    pub host_elapsed_ms: f64,
    pub game_active_ms: f64,
    pub input_poll_count: u64,
    pub is_backgrounded: bool,

    /* Checkpoint activation anchor */
    pub checkpoint_activated_host_ms: f64,
    pub checkpoint_activated_game_ms: f64,
    pub checkpoint_activated_polls: u64,

    pub is_finished: bool,
}

impl ScenarioStateMachine {
    pub fn new(scenario: ScenarioDefinition) -> Self {
        Self {
            scenario,
            current_checkpoint_index: 0,
            last_confirmed_checkpoint: None,
            covered_segments: Vec::new(),
            results: Vec::new(),
            execution_outcome: "PENDING".to_string(),
            reason_code: "INITIAL".to_string(),
            host_elapsed_ms: 0.0,
            game_active_ms: 0.0,
            input_poll_count: 0,
            is_backgrounded: false,
            checkpoint_activated_host_ms: 0.0,
            checkpoint_activated_game_ms: 0.0,
            checkpoint_activated_polls: 0,
            is_finished: false,
        }
    }

    pub fn set_backgrounded(&mut self, backgrounded: bool) {
        self.is_backgrounded = backgrounded;
    }

    pub fn advance_time(&mut self, delta_host_ms: f64, delta_game_ms: f64, delta_polls: u64) {
        if self.is_finished {
            return;
        }

        self.host_elapsed_ms += delta_host_ms;
        if !self.is_backgrounded {
            self.game_active_ms += delta_game_ms;
        }
        self.input_poll_count += delta_polls;

        self.check_deadlines();
    }

    fn check_deadlines(&mut self) {
        if self.is_finished || self.current_checkpoint_index >= self.scenario.checkpoints.len() {
            return;
        }

        let cp = &self.scenario.checkpoints[self.current_checkpoint_index];
        let elapsed = match cp.clock_policy {
            ClockPolicy::HostTime => self.host_elapsed_ms - self.checkpoint_activated_host_ms,
            ClockPolicy::GameActiveTime => self.game_active_ms - self.checkpoint_activated_game_ms,
            ClockPolicy::InputPollCount => (self.input_poll_count - self.checkpoint_activated_polls) as f64,
        };

        if elapsed > cp.deadline_ms as f64 {
            let res = DiagnosticCheckpointResult {
                checkpoint_id: cp.checkpoint_id.clone(),
                occurrence: cp.occurrence,
                status: "TIMED_OUT".to_string(),
                target_segment: cp.target_segment.clone(),
                elapsed_host_ms: Some(self.host_elapsed_ms),
                elapsed_game_ms: Some(self.game_active_ms),
                reason_code: Some(reasons::CHECKPOINT_TIMEOUT.to_string()),
                evidence_ref: None,
            };
            self.results.push(res);

            if cp.required {
                self.execution_outcome = "TIMED_OUT".to_string();
                self.reason_code = reasons::CHECKPOINT_TIMEOUT.to_string();
                self.is_finished = true;
            } else {
                self.advance_to_next_checkpoint();
            }
        }
    }

    pub fn on_semantic_observation(&mut self, obs: &SemanticObservation) -> bool {
        if self.is_finished || self.current_checkpoint_index >= self.scenario.checkpoints.len() {
            return false;
        }

        // Freshness check: observation must have been produced AFTER the current checkpoint activated
        if obs.produced_at_host_ms < self.checkpoint_activated_host_ms {
            // Stale sampled state cannot satisfy a new checkpoint
            return false;
        }

        let cp = &self.scenario.checkpoints[self.current_checkpoint_index];

        // Draw counts alone do not satisfy checkpoints
        if !cp.expected.matches(obs) {
            return false;
        }

        // Checkpoint satisfied!
        let res = DiagnosticCheckpointResult {
            checkpoint_id: cp.checkpoint_id.clone(),
            occurrence: cp.occurrence,
            status: "CONFIRMED".to_string(),
            target_segment: cp.target_segment.clone(),
            elapsed_host_ms: Some(self.host_elapsed_ms),
            elapsed_game_ms: Some(self.game_active_ms),
            reason_code: None,
            evidence_ref: Some(format!("semantic_obs:ch{}_v{}", obs.chapter, obs.verse)),
        };

        self.last_confirmed_checkpoint = Some(cp.checkpoint_id.clone());
        if !self.covered_segments.contains(&cp.target_segment) {
            self.covered_segments.push(cp.target_segment.clone());
        }
        self.results.push(res);

        self.advance_to_next_checkpoint();
        true
    }

    fn advance_to_next_checkpoint(&mut self) {
        self.current_checkpoint_index += 1;
        self.checkpoint_activated_host_ms = self.host_elapsed_ms;
        self.checkpoint_activated_game_ms = self.game_active_ms;
        self.checkpoint_activated_polls = self.input_poll_count;

        if self.current_checkpoint_index >= self.scenario.checkpoints.len() {
            self.execution_outcome = "COMPLETED".to_string();
            self.reason_code = reasons::CLEAN_COMPLETION.to_string();
            self.is_finished = true;
        }
    }

    pub fn on_replay_exhausted(&mut self) {
        if self.is_finished {
            return;
        }

        if self.current_checkpoint_index < self.scenario.checkpoints.len() {
            let cp = &self.scenario.checkpoints[self.current_checkpoint_index];
            self.results.push(DiagnosticCheckpointResult {
                checkpoint_id: cp.checkpoint_id.clone(),
                occurrence: cp.occurrence,
                status: "MISSED".to_string(),
                target_segment: cp.target_segment.clone(),
                elapsed_host_ms: Some(self.host_elapsed_ms),
                elapsed_game_ms: Some(self.game_active_ms),
                reason_code: Some("REPLAY_EXHAUSTED".to_string()),
                evidence_ref: None,
            });

            self.execution_outcome = "DIVERTED".to_string();
            self.reason_code = "REPLAY_EXHAUSTED".to_string();
            self.is_finished = true;
        }
    }

    pub fn on_divergence(&mut self, reason: &str) {
        if self.is_finished {
            return;
        }

        self.execution_outcome = "FAILED_ASSERTION".to_string();
        self.reason_code = reason.to_string();
        self.is_finished = true;
    }

    pub fn generate_outcome(&self, run_id: &str) -> DiagnosticOutcome {
        DiagnosticOutcome {
            schema_version: SchemaVersion::CURRENT,
            run_id: run_id.to_string(),
            execution_outcome: self.execution_outcome.clone(),
            reason_code: self.reason_code.clone(),
            exit_status: DiagnosticExitStatus {
                clean_exit: self.execution_outcome == "COMPLETED",
                exit_code: if self.execution_outcome == "COMPLETED" { Some(0) } else { Some(1) },
                signal: None,
                termination_origin: "scenario_runner".to_string(),
            },
            last_confirmed_checkpoint: self.last_confirmed_checkpoint.clone(),
            covered_segments: self.covered_segments.clone(),
            checkpoints: self.results.clone(),
            correctness_assertions: Vec::new(),
            storage_exhausted: false,
            incomplete_recovery: false,
            extra: std::collections::BTreeMap::new(),
        }
    }
}
