use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, PartialEq)]
pub struct AdbDeviceInfo {
    pub serial: String,
    pub state: String,
}

#[derive(Debug, Clone, PartialEq)]
pub struct AdbCommandOutput {
    pub exit_code: i32,
    pub stdout: String,
    pub stderr: String,
}

pub trait AdbTransport: Send + Sync {
    fn list_devices(&self) -> Result<Vec<AdbDeviceInfo>, String>;
    fn shell(&self, serial: &str, args: &[&str]) -> Result<AdbCommandOutput, String>;
    fn push(&self, serial: &str, local_path: &Path, remote_path: &str) -> Result<(), String>;
    fn pull(&self, serial: &str, remote_path: &str, local_path: &Path) -> Result<(), String>;
}

/// Real ADB transport using the host's adb binary via std::process::Command without shell dependencies.
pub struct RealAdb {
    pub adb_path: PathBuf,
}

impl RealAdb {
    pub fn new() -> Self {
        // Locate adb from ANDROID_HOME or PATH
        let adb_path = if let Ok(sdk) = std::env::var("ANDROID_HOME") {
            let p = PathBuf::from(sdk).join("platform-tools").join(if cfg!(windows) { "adb.exe" } else { "adb" });
            if p.is_file() {
                p
            } else {
                PathBuf::from(if cfg!(windows) { "adb.exe" } else { "adb" })
            }
        } else {
            PathBuf::from(if cfg!(windows) { "adb.exe" } else { "adb" })
        };
        Self { adb_path }
    }
}

impl AdbTransport for RealAdb {
    fn list_devices(&self) -> Result<Vec<AdbDeviceInfo>, String> {
        let output = Command::new(&self.adb_path)
            .arg("devices")
            .output()
            .map_err(|e| format!("Failed to run adb devices: {}", e))?;

        let text = String::from_utf8_lossy(&output.stdout);
        let mut devices = Vec::new();
        for line in text.lines().skip(1) {
            let line = line.trim();
            if line.is_empty() {
                continue;
            }
            let mut parts = line.split_whitespace();
            if let Some(serial) = parts.next() {
                let state = parts.next().unwrap_or("unknown").to_string();
                devices.push(AdbDeviceInfo {
                    serial: serial.to_string(),
                    state,
                });
            }
        }
        Ok(devices)
    }

    fn shell(&self, serial: &str, args: &[&str]) -> Result<AdbCommandOutput, String> {
        let mut cmd = Command::new(&self.adb_path);
        cmd.arg("-s").arg(serial).arg("shell");
        for a in args {
            cmd.arg(a);
        }
        let output = cmd
            .output()
            .map_err(|e| format!("Failed to run adb shell on {}: {}", serial, e))?;

        Ok(AdbCommandOutput {
            exit_code: output.status.code().unwrap_or(-1),
            stdout: String::from_utf8_lossy(&output.stdout).to_string(),
            stderr: String::from_utf8_lossy(&output.stderr).to_string(),
        })
    }

    fn push(&self, serial: &str, local_path: &Path, remote_path: &str) -> Result<(), String> {
        let output = Command::new(&self.adb_path)
            .arg("-s")
            .arg(serial)
            .arg("push")
            .arg(local_path)
            .arg(remote_path)
            .output()
            .map_err(|e| format!("Failed to push to {}: {}", serial, e))?;

        if !output.status.success() {
            return Err(format!(
                "adb push failed: {}",
                String::from_utf8_lossy(&output.stderr)
            ));
        }
        Ok(())
    }

    fn pull(&self, serial: &str, remote_path: &str, local_path: &Path) -> Result<(), String> {
        let output = Command::new(&self.adb_path)
            .arg("-s")
            .arg(serial)
            .arg("pull")
            .arg(remote_path)
            .arg(local_path)
            .output()
            .map_err(|e| format!("Failed to pull from {}: {}", serial, e))?;

        if !output.status.success() {
            return Err(format!(
                "adb pull failed: {}",
                String::from_utf8_lossy(&output.stderr)
            ));
        }
        Ok(())
    }
}

/// Simulated Fake ADB for deterministic unit tests.
#[derive(Clone)]
pub struct FakeAdb {
    pub devices: Arc<Mutex<Vec<AdbDeviceInfo>>>,
    pub running_processes: Arc<Mutex<HashMap<String, Vec<(String, u32, String)>>>>, // serial -> (package, pid, run_token)
    pub remote_fs: Arc<Mutex<HashMap<String, HashMap<String, Vec<u8>>>>>, // serial -> (path -> bytes)
    pub transport_connected: Arc<Mutex<HashMap<String, bool>>>,
    pub killed_packages: Arc<Mutex<Vec<String>>>,
    pub am_start_invocations: Arc<Mutex<usize>>,
    pub simulate_process_death_on_pidof: Arc<Mutex<bool>>,
}

impl FakeAdb {
    pub fn new() -> Self {
        Self {
            devices: Arc::new(Mutex::new(Vec::new())),
            running_processes: Arc::new(Mutex::new(HashMap::new())),
            remote_fs: Arc::new(Mutex::new(HashMap::new())),
            transport_connected: Arc::new(Mutex::new(HashMap::new())),
            killed_packages: Arc::new(Mutex::new(Vec::new())),
            am_start_invocations: Arc::new(Mutex::new(0)),
            simulate_process_death_on_pidof: Arc::new(Mutex::new(false)),
        }
    }

    pub fn add_device(&self, serial: &str, state: &str) {
        self.devices.lock().unwrap().push(AdbDeviceInfo {
            serial: serial.to_string(),
            state: state.to_string(),
        });
        self.transport_connected
            .lock()
            .unwrap()
            .insert(serial.to_string(), true);
    }

    pub fn set_transport_connected(&self, serial: &str, connected: bool) {
        self.transport_connected
            .lock()
            .unwrap()
            .insert(serial.to_string(), connected);
    }

    pub fn set_process_running(&self, serial: &str, package: &str, pid: u32, token: &str) {
        let mut procs = self.running_processes.lock().unwrap();
        procs
            .entry(serial.to_string())
            .or_default()
            .push((package.to_string(), pid, token.to_string()));
    }

    pub fn write_remote_file(&self, serial: &str, path: &str, data: Vec<u8>) {
        let mut fs = self.remote_fs.lock().unwrap();
        fs.entry(serial.to_string())
            .or_default()
            .insert(path.to_string(), data);
    }
}

impl AdbTransport for FakeAdb {
    fn list_devices(&self) -> Result<Vec<AdbDeviceInfo>, String> {
        Ok(self.devices.lock().unwrap().clone())
    }

    fn shell(&self, serial: &str, args: &[&str]) -> Result<AdbCommandOutput, String> {
        let connected = *self
            .transport_connected
            .lock()
            .unwrap()
            .get(serial)
            .unwrap_or(&false);
        if !connected {
            return Err(format!("Device {} disconnected / transport lost", serial));
        }

        if args.is_empty() {
            return Ok(AdbCommandOutput {
                exit_code: 0,
                stdout: String::new(),
                stderr: String::new(),
            });
        }

        let cmd = args[0];
        match cmd {
            "pidof" => {
                if *self.simulate_process_death_on_pidof.lock().unwrap() {
                    return Ok(AdbCommandOutput {
                        exit_code: 1,
                        stdout: String::new(),
                        stderr: String::new(),
                    });
                }
                let pkg = args.get(1).unwrap_or(&"");
                let procs = self.running_processes.lock().unwrap();
                let list = procs.get(serial);
                if let Some(list) = list {
                    let matching: Vec<String> = list
                        .iter()
                        .filter(|(p, _, _)| p == pkg)
                        .map(|(_, pid, _)| pid.to_string())
                        .collect();
                    if !matching.is_empty() {
                        return Ok(AdbCommandOutput {
                            exit_code: 0,
                            stdout: matching.join(" "),
                            stderr: String::new(),
                        });
                    }
                }
                Ok(AdbCommandOutput {
                    exit_code: 1,
                    stdout: String::new(),
                    stderr: String::new(),
                })
            }
            "am" if args.get(1) == Some(&"force-stop") => {
                let pkg = args.get(2).unwrap_or(&"");
                self.killed_packages.lock().unwrap().push(pkg.to_string());
                let mut procs = self.running_processes.lock().unwrap();
                if let Some(list) = procs.get_mut(serial) {
                    list.retain(|(p, _, _)| p != pkg);
                }
                Ok(AdbCommandOutput {
                    exit_code: 0,
                    stdout: String::new(),
                    stderr: String::new(),
                })
            }
            "am" if args.get(1) == Some(&"start") => {
                *self.am_start_invocations.lock().unwrap() += 1;
                // Parse package from component (e.g. org.witchtime.android/.WitchTimeActivity)
                let comp = args.iter().find(|a| a.contains("/."));
                let pkg = comp
                    .map(|c| c.split('/').next().unwrap_or("org.witchtime.android"))
                    .unwrap_or("org.witchtime.android");
                let token = args
                    .iter()
                    .position(|&a| a == "--es" || a == "-e")
                    .and_then(|idx| args.get(idx + 2))
                    .copied()
                    .unwrap_or("run_token");

                let mut procs = self.running_processes.lock().unwrap();
                procs
                    .entry(serial.to_string())
                    .or_default()
                    .push((pkg.to_string(), 12345, token.to_string()));

                Ok(AdbCommandOutput {
                    exit_code: 0,
                    stdout: "Starting: Intent { cmp=... }\n".to_string(),
                    stderr: String::new(),
                })
            }
            "cat" => {
                let path = args.get(1).unwrap_or(&"");
                let fs = self.remote_fs.lock().unwrap();
                if let Some(dev_fs) = fs.get(serial) {
                    if let Some(bytes) = dev_fs.get(*path) {
                        return Ok(AdbCommandOutput {
                            exit_code: 0,
                            stdout: String::from_utf8_lossy(bytes).to_string(),
                            stderr: String::new(),
                        });
                    }
                }
                Ok(AdbCommandOutput {
                    exit_code: 1,
                    stdout: String::new(),
                    stderr: "No such file or directory\n".to_string(),
                })
            }
            _ => Ok(AdbCommandOutput {
                exit_code: 0,
                stdout: String::new(),
                stderr: String::new(),
            }),
        }
    }

    fn push(&self, serial: &str, local_path: &Path, remote_path: &str) -> Result<(), String> {
        let connected = *self
            .transport_connected
            .lock()
            .unwrap()
            .get(serial)
            .unwrap_or(&false);
        if !connected {
            return Err("Transport lost".to_string());
        }
        let data = std::fs::read(local_path).unwrap_or_default();
        let mut fs = self.remote_fs.lock().unwrap();
        fs.entry(serial.to_string())
            .or_default()
            .insert(remote_path.to_string(), data);
        Ok(())
    }

    fn pull(&self, serial: &str, remote_path: &str, local_path: &Path) -> Result<(), String> {
        let connected = *self
            .transport_connected
            .lock()
            .unwrap()
            .get(serial)
            .unwrap_or(&false);
        if !connected {
            return Err("Transport lost".to_string());
        }
        let fs = self.remote_fs.lock().unwrap();
        if let Some(dev_fs) = fs.get(serial) {
            if let Some(bytes) = dev_fs.get(remote_path) {
                if let Some(parent) = local_path.parent() {
                    let _ = std::fs::create_dir_all(parent);
                }
                std::fs::write(local_path, bytes).map_err(|e| e.to_string())?;
                return Ok(());
            }
        }
        Err(format!("File {} not found on device", remote_path))
    }
}
