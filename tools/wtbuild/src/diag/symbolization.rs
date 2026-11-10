use serde::{Deserialize, Serialize};
use std::collections::HashMap;

/// 64-byte raw crash breadcrumb written by runtime fatal hook.
pub const DIAG_CRASH_MAGIC: u32 = 0x43525348; /* 'CRSH' */

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct CrashBreadcrumb {
    pub magic: u32,
    pub signal_num: u32,
    pub fault_address: u64,
    pub thread_id: u32,
    pub exit_reason: u32,
    pub monotonic_ns: u64,
    pub global_seq: u64,
}

pub fn parse_crash_breadcrumb(bytes: &[u8]) -> Result<CrashBreadcrumb, String> {
    if bytes.len() < 64 {
        return Err(format!(
            "Crash breadcrumb too small: {} bytes, expected at least 64 bytes",
            bytes.len()
        ));
    }
    let magic = u32::from_le_bytes(bytes[0..4].try_into().unwrap());
    if magic != DIAG_CRASH_MAGIC {
        return Err(format!(
            "Invalid crash breadcrumb magic: 0x{:08X}, expected 0x{:08X}",
            magic, DIAG_CRASH_MAGIC
        ));
    }
    let signal_num = u32::from_le_bytes(bytes[4..8].try_into().unwrap());
    let fault_address = u64::from_le_bytes(bytes[8..16].try_into().unwrap());
    let thread_id = u32::from_le_bytes(bytes[16..20].try_into().unwrap());
    let exit_reason = u32::from_le_bytes(bytes[20..24].try_into().unwrap());
    let monotonic_ns = u64::from_le_bytes(bytes[24..32].try_into().unwrap());
    let global_seq = u64::from_le_bytes(bytes[32..40].try_into().unwrap());

    Ok(CrashBreadcrumb {
        magic,
        signal_num,
        fault_address,
        thread_id,
        exit_reason,
        monotonic_ns,
        global_seq,
    })
}

/// Android ApplicationExitInfo exit reasons from ActivityManager.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum ApplicationExitReason {
    NativeCrash,
    Anr,
    InitializationFailure,
    ExcessiveResourceUsage,
    UserRequested,
    UserStopped,
    DependencyDied,
    Other(i32),
    Unknown,
}

impl ApplicationExitReason {
    pub fn from_i32(val: i32) -> Self {
        match val {
            5 => ApplicationExitReason::NativeCrash,
            6 => ApplicationExitReason::Anr,
            7 => ApplicationExitReason::InitializationFailure,
            9 => ApplicationExitReason::ExcessiveResourceUsage,
            10 => ApplicationExitReason::UserRequested,
            11 => ApplicationExitReason::UserStopped,
            12 => ApplicationExitReason::DependencyDied,
            13 => ApplicationExitReason::Other(13),
            other => {
                if other > 0 {
                    ApplicationExitReason::Other(other)
                } else {
                    ApplicationExitReason::Unknown
                }
            }
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ApplicationExitRecord {
    pub pid: u32,
    pub process_name: String,
    pub reason: ApplicationExitReason,
    pub status: i32,
    pub importance: i32,
    pub timestamp_ms: u64,
    pub description: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum ExitInfoStatus {
    Found(ApplicationExitRecord),
    AbsentOrOverwritten,
    ApiLevelUnsupported(u32),
    CommandFailed(String),
}

/// Parses dumpsys activity exit-info output for target package.
pub fn parse_dumpsys_exit_info(output: &str, target_pkg: &str) -> ExitInfoStatus {
    if output.contains("Unknown service: activity")
        || output.contains("Service activity does not support")
    {
        return ExitInfoStatus::ApiLevelUnsupported(29);
    }
    if output.trim().is_empty()
        || output.contains("No exit info")
        || output.contains("ApplicationExitInfo is not supported")
    {
        return ExitInfoStatus::AbsentOrOverwritten;
    }

    // Look for target package block
    let mut in_target_block = false;
    let mut cur_pid: Option<u32> = None;
    let mut cur_reason: Option<ApplicationExitReason> = None;
    let mut cur_status: i32 = 0;
    let mut cur_importance: i32 = 0;
    let mut cur_timestamp: u64 = 0;
    let mut cur_desc: Option<String> = None;

    for line in output.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with("ApplicationExitInfo #") {
            if in_target_block && cur_pid.is_some() && cur_reason.is_some() {
                break;
            }
            in_target_block = false;
            cur_pid = None;
            cur_reason = None;
            cur_status = 0;
            cur_importance = 0;
            cur_timestamp = 0;
            cur_desc = None;
        }

        if trimmed.starts_with("package=") || trimmed.starts_with("process=") {
            let val = trimmed.split('=').nth(1).unwrap_or("");
            if val == target_pkg || val.starts_with(target_pkg) {
                in_target_block = true;
            }
        }

        if in_target_block {
            for part in trimmed.split_whitespace() {
                if let Some(val) = part.strip_prefix("pid=") {
                    cur_pid = val.parse::<u32>().ok();
                } else if let Some(val) = part.strip_prefix("reason=") {
                    let code_str = val.split_whitespace().next().unwrap_or("0");
                    let code = code_str.parse::<i32>().unwrap_or(0);
                    cur_reason = Some(ApplicationExitReason::from_i32(code));
                } else if let Some(val) = part.strip_prefix("status=") {
                    cur_status = val.parse::<i32>().unwrap_or(0);
                } else if let Some(val) = part.strip_prefix("importance=") {
                    cur_importance = val.parse::<i32>().unwrap_or(0);
                } else if let Some(val) = part.strip_prefix("timestamp=") {
                    cur_timestamp = val.parse::<u64>().unwrap_or(0);
                }
            }
            if trimmed.starts_with("description=") {
                cur_desc = Some(trimmed.trim_start_matches("description=").to_string());
            }
        }
    }

    if let (Some(pid), Some(reason)) = (cur_pid, cur_reason) {
        ExitInfoStatus::Found(ApplicationExitRecord {
            pid,
            process_name: target_pkg.to_string(),
            reason,
            status: cur_status,
            importance: cur_importance,
            timestamp_ms: cur_timestamp,
            description: cur_desc,
        })
    } else {
        ExitInfoStatus::AbsentOrOverwritten
    }
}

/// A parsed backtrace frame from native tombstone or logcat crash dump.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TombstoneFrame {
    pub frame_num: u32,
    pub pc: u64,
    pub module_path: String,
    pub module_offset: u64,
    pub build_id: Option<String>,
    pub function_name: Option<String>,
    pub function_offset: Option<u64>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct NativeTombstone {
    pub pid: u32,
    pub tid: u32,
    pub signal: u32,
    pub signal_name: String,
    pub fault_address: u64,
    pub frames: Vec<TombstoneFrame>,
}

/// Parses logcat crash log or tombstone text.
pub fn parse_logcat_tombstone(logcat_crash: &str) -> Option<NativeTombstone> {
    let mut pid: u32 = 0;
    let mut tid: u32 = 0;
    let mut signal: u32 = 0;
    let mut signal_name = String::new();
    let mut fault_address: u64 = 0;
    let mut frames = Vec::new();
    let mut in_backtrace = false;

    for line in logcat_crash.lines() {
        let trimmed = line.trim();
        if trimmed.contains("Fatal signal") {
            // e.g.: Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0xdeadbeef in tid 1234
            if let Some(idx) = trimmed.find("Fatal signal") {
                let rest = &trimmed[idx..];
                let parts: Vec<&str> = rest.split_whitespace().collect();
                if parts.len() >= 3 {
                    signal = parts[2].parse::<u32>().unwrap_or(0);
                }
                if let Some(open_p) = rest.find('(') {
                    if let Some(close_p) = rest.find(')') {
                        if close_p > open_p {
                            signal_name = rest[open_p + 1..close_p].to_string();
                        }
                    }
                }
                if let Some(addr_idx) = rest.find("fault addr ") {
                    let addr_str = rest[addr_idx + 11..].split_whitespace().next().unwrap_or("0");
                    let clean = addr_str.trim_start_matches("0x").trim_matches(',');
                    fault_address = u64::from_str_radix(clean, 16).unwrap_or(0);
                }
                if let Some(tid_idx) = rest.find("tid ") {
                    let tid_str = rest[tid_idx + 4..].split_whitespace().next().unwrap_or("0");
                    tid = tid_str.trim_matches(',').parse::<u32>().unwrap_or(0);
                }
                if let Some(pid_idx) = rest.find("pid ") {
                    let pid_str = rest[pid_idx + 4..].split_whitespace().next().unwrap_or("0");
                    pid = pid_str.trim_matches(',').parse::<u32>().unwrap_or(0);
                }
            }
        } else if trimmed.contains("backtrace:") {
            in_backtrace = true;
        } else if in_backtrace {
            let Some(hash_idx) = trimmed.find('#') else {
                if !trimmed.is_empty() && !trimmed.starts_with("---") && !trimmed.contains("DEBUG") {
                    in_backtrace = false;
                }
                continue;
            };
            let frame_content = &trimmed[hash_idx..];
            // Parse frame line:
            // e.g.: #00 pc 000000000004a120  /data/app/.../libwitch-time-game.so (BuildId: 0123456789abcdef)
            let parts: Vec<&str> = frame_content.split_whitespace().collect();
            if parts.len() >= 4 {
                let frame_str = parts[0].trim_start_matches('#');
                let frame_num = frame_str.parse::<u32>().unwrap_or(0);
                let offset_str = parts[2].trim_start_matches("0x");
                let offset = u64::from_str_radix(offset_str, 16).unwrap_or(0);
                let module_path = parts[3].to_string();

                let mut build_id = None;
                if let Some(b_idx) = frame_content.find("BuildId: ") {
                    let b_str = &frame_content[b_idx + 9..];
                    if let Some(end_p) = b_str.find(')') {
                        build_id = Some(b_str[..end_p].trim().to_string());
                    }
                }

                frames.push(TombstoneFrame {
                    frame_num,
                    pc: offset,
                    module_path,
                    module_offset: offset,
                    build_id,
                    function_name: None,
                    function_offset: None,
                });
            }
        }
    }

    if signal > 0 || !frames.is_empty() {
        Some(NativeTombstone {
            pid,
            tid,
            signal,
            signal_name,
            fault_address,
            frames,
        })
    } else {
        None
    }
}

/// Vulkan fault and breadcrumb context.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct VulkanFaultInfo {
    pub device_lost: bool,
    pub fault_description: Option<String>,
    pub vulkan_breadcrumbs: Vec<String>,
}

/// Local symbol entry for offline symbol lookup.
#[derive(Debug, Clone, PartialEq)]
pub struct LocalSymbolEntry {
    pub symbol_name: String,
    pub start_offset: u64,
    pub size: u64,
}

/// Local module symbols container with ELF Build-ID and guest address map.
#[derive(Debug, Clone)]
pub struct LocalModuleSymbols {
    pub module_name: String,
    pub expected_build_id: String,
    pub symbols: Vec<LocalSymbolEntry>,
    pub guest_map: HashMap<u64, String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum SymbolResolutionStatus {
    ResolvedExact,
    ResolvedGuestMap,
    BuildIdMismatch { expected: String, actual: String },
    SymbolsNotFound,
    RawOffsetFallback,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct SymbolizedFrame {
    pub frame_num: u32,
    pub pc: u64,
    pub module: String,
    pub offset: u64,
    pub symbol: String,
    pub guest_function: Option<String>,
    pub resolution_status: SymbolResolutionStatus,
}

/// Resolves a single tombstone frame using local private symbols.
pub fn symbolize_frame(
    frame: &TombstoneFrame,
    local_modules: &[LocalModuleSymbols],
) -> SymbolizedFrame {
    let mod_name = frame
        .module_path
        .split('/')
        .last()
        .unwrap_or(&frame.module_path);

    let matching_mod = local_modules.iter().find(|m| {
        m.module_name == mod_name || frame.module_path.ends_with(&m.module_name)
    });

    let Some(local_mod) = matching_mod else {
        return SymbolizedFrame {
            frame_num: frame.frame_num,
            pc: frame.pc,
            module: mod_name.to_string(),
            offset: frame.module_offset,
            symbol: format!("{}+0x{:x}", mod_name, frame.module_offset),
            guest_function: None,
            resolution_status: SymbolResolutionStatus::SymbolsNotFound,
        };
    };

    // Check Build-ID if available in frame
    if let Some(ref actual_bid) = frame.build_id {
        if actual_bid != &local_mod.expected_build_id {
            return SymbolizedFrame {
                frame_num: frame.frame_num,
                pc: frame.pc,
                module: mod_name.to_string(),
                offset: frame.module_offset,
                symbol: format!("{}+0x{:x}", mod_name, frame.module_offset),
                guest_function: None,
                resolution_status: SymbolResolutionStatus::BuildIdMismatch {
                    expected: local_mod.expected_build_id.clone(),
                    actual: actual_bid.clone(),
                },
            };
        }
    }

    // Lookup matching symbol
    for sym in &local_mod.symbols {
        if frame.module_offset >= sym.start_offset
            && (sym.size == 0 || frame.module_offset < sym.start_offset + sym.size)
        {
            let off_in_sym = frame.module_offset - sym.start_offset;
            let guest_fn = if sym.symbol_name.starts_with("f_") {
                Some(sym.symbol_name.clone())
            } else {
                local_mod.guest_map.get(&sym.start_offset).cloned()
            };

            let status = if guest_fn.is_some() {
                SymbolResolutionStatus::ResolvedGuestMap
            } else {
                SymbolResolutionStatus::ResolvedExact
            };

            return SymbolizedFrame {
                frame_num: frame.frame_num,
                pc: frame.pc,
                module: mod_name.to_string(),
                offset: frame.module_offset,
                symbol: format!("{}+0x{:x}", sym.symbol_name, off_in_sym),
                guest_function: guest_fn,
                resolution_status: status,
            };
        }
    }

    // Fallback to raw offset
    SymbolizedFrame {
        frame_num: frame.frame_num,
        pc: frame.pc,
        module: mod_name.to_string(),
        offset: frame.module_offset,
        symbol: format!("{}+0x{:x}", mod_name, frame.module_offset),
        guest_function: None,
        resolution_status: SymbolResolutionStatus::RawOffsetFallback,
    }
}

/// Aggregated crash and exit diagnosis.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct CrashAnalysisOutcome {
    pub breadcrumb: Option<CrashBreadcrumb>,
    pub exit_info: ExitInfoStatus,
    pub tombstone: Option<NativeTombstone>,
    pub symbolized_backtrace: Vec<SymbolizedFrame>,
    pub vulkan_fault: Option<VulkanFaultInfo>,
    pub confirmed_crash_cause: Option<String>,
}

/// Evaluates crash analysis outcome ensuring missing records are never fabricated into causes.
pub fn evaluate_crash_analysis(
    breadcrumb: Option<CrashBreadcrumb>,
    exit_info: ExitInfoStatus,
    tombstone: Option<NativeTombstone>,
    local_modules: &[LocalModuleSymbols],
    vulkan_fault: Option<VulkanFaultInfo>,
) -> CrashAnalysisOutcome {
    let mut symbolized_backtrace = Vec::new();
    let mut confirmed_crash_cause = None;

    if let Some(ref tb) = tombstone {
        for frame in &tb.frames {
            symbolized_backtrace.push(symbolize_frame(frame, local_modules));
        }
        confirmed_crash_cause = Some(format!(
            "Signal {} ({}) at fault address 0x{:x}",
            tb.signal, tb.signal_name, tb.fault_address
        ));
    } else if let Some(ref crumb) = breadcrumb {
        confirmed_crash_cause = Some(format!(
            "Diagnostic breadcrumb signal {} at fault address 0x{:x}",
            crumb.signal_num, crumb.fault_address
        ));
    } else if let ExitInfoStatus::Found(ref rec) = exit_info {
        if rec.reason == ApplicationExitReason::NativeCrash {
            confirmed_crash_cause = Some(format!(
                "ApplicationExitInfo REASON_CRASH_NATIVE status {}",
                rec.status
            ));
        } else if rec.reason == ApplicationExitReason::Anr {
            confirmed_crash_cause = Some("ApplicationExitInfo REASON_ANR".to_string());
        } else if rec.reason == ApplicationExitReason::InitializationFailure {
            confirmed_crash_cause =
                Some("ApplicationExitInfo REASON_INITIALIZATION_FAILURE".to_string());
        }
    }

    // Critical invariant: If tombstone, breadcrumb, and exit_info are absent/overwritten,
    // confirmed_crash_cause MUST remain None. Never turn an absent record into a confirmed cause!

    CrashAnalysisOutcome {
        breadcrumb,
        exit_info,
        tombstone,
        symbolized_backtrace,
        vulkan_fault,
        confirmed_crash_cause,
    }
}
