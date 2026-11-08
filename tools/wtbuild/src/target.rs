//! What the build makes differs between Linux, Windows and Android: file names, compiler target and runtime
//! libraries. The player's build is always for the system it runs on; `wtbuild compile / link / runtime --target windows`
//! makes the Windows files elsewhere too (CI builds the Windows runtime on Linux with llvm-mingw).

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Target {
    Linux,
    Windows,
    AndroidArm64,
}

impl Target {
    pub fn host() -> Target {
        if cfg!(windows) {
            Target::Windows
        } else {
            Target::Linux
        }
    }

    /// `--target linux|windows|android-arm64` among the arguments, else the desktop host
    pub fn from_args(args: &[String]) -> Result<Target, String> {
        match args.iter().position(|a| a == "--target").map(|i| args.get(i + 1).map(String::as_str)) {
            None => Ok(Target::host()),
            Some(Some("linux")) => Ok(Target::Linux),
            Some(Some("windows")) => Ok(Target::Windows),
            Some(Some("android-arm64")) => Ok(Target::AndroidArm64),
            Some(v) => Err(format!("--target {}: linux, windows or android-arm64", v.unwrap_or(""))),
        }
    }

    pub fn windows(self) -> bool {
        self == Target::Windows
    }

    /// TOOLCHAIN in build/setup_state.json, the log lines and the compile step's configuration hash
    pub fn name(self) -> &'static str {
        match self {
            Target::Linux => "linux",
            Target::Windows => "windows",
            Target::AndroidArm64 => "android-arm64",
        }
    }

    /// the game's own library, linked on the player's PC
    pub fn game_lib(self) -> &'static str {
        match self {
            Target::Linux => "game.so",
            Target::Windows => "game.dll",
            Target::AndroidArm64 => "libgame.so",
        }
    }

    /// the program that runs the game
    pub fn game_exe(self) -> &'static str {
        match self {
            Target::Linux => "witch-time-game",
            Target::Windows => "witch-time-game.exe",
            Target::AndroidArm64 => "libwitchtime.so", // loaded by the Android app, not a desktop program
        }
    }

    /// clang flags that pick the target. Windows: the MinGW ABI the runtime is built with, for any clang (the bundled one,
    /// LLVM's own release, would make MSVC objects; llvm-mingw's makes these anyway)
    pub fn clang_flags(self) -> Vec<String> {
        match self {
            Target::Linux => vec![],
            Target::Windows => vec!["--target=x86_64-w64-mingw32".into()],
            Target::AndroidArm64 => vec!["--target=aarch64-linux-android29".into()],
        }
    }

    /// code that goes into a shared library: position-independent on ELF; PE has no such flag (clang refuses it)
    pub fn pic(self) -> Vec<String> {
        match self {
            Target::Linux | Target::AndroidArm64 => vec!["-fPIC".into()],
            Target::Windows => vec![],
        }
    }

    pub fn android(self) -> bool {
        self == Target::AndroidArm64
    }

    /// Android artifacts never overwrite desktop objects or libraries. Generated C is target-independent.
    pub fn build_dir(self, repo: &Path) -> PathBuf {
        let build = crate::layout::build(repo);
        if self.android() {
            build.join(self.name())
        } else {
            build
        }
    }

    /// An Android cross build must not search the developer's desktop header/library directories.
    pub fn configure_command(self, cmd: &mut Command) {
        if self.android() {
            for key in ["CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "LIBRARY_PATH", "SDKROOT"] {
                cmd.env_remove(key);
            }
        }
    }

    pub fn link_flags(self) -> Vec<String> {
        if self.android() {
            // Original sources have MSVC #pragma comment(lib, ...); Android dependencies are explicit.
            ["-Wl,-z,max-page-size=16384", "-Wl,-z,common-page-size=16384", "-Wl,--no-dependent-libraries"]
                .map(String::from)
                .into()
        } else {
            vec![]
        }
    }
}

/// the compiler's name: clang, clang.exe
pub fn exe(name: &str) -> String {
    format!("{}{}", name, std::env::consts::EXE_SUFFIX)
}

/// a file's modification time in nanoseconds since 1970, as Python's st_mtime_ns
pub fn mtime_ns(m: &fs::Metadata) -> i128 {
    match m.modified().map(|t| t.duration_since(std::time::UNIX_EPOCH)) {
        Ok(Ok(d)) => d.as_nanos() as i128,
        Ok(Err(e)) => -(e.duration().as_nanos() as i128),
        Err(_) => 0,
    }
}

/// a program file this system can run (shutil.which's test)
pub fn is_program(p: &Path) -> bool {
    let Ok(m) = fs::metadata(p) else { return false };
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        m.is_file() && m.permissions().mode() & 0o111 != 0
    }
    #[cfg(not(unix))]
    {
        m.is_file()
    }
}

/// removes a symbolic link or junction to a folder (older setups linked targets/romfs), never what it points to
pub fn remove_dir_link(link: &Path) -> std::io::Result<()> {
    if cfg!(windows) {
        fs::remove_dir(link) // a junction (or a directory symbolic link) is removed as an empty folder
    } else {
        fs::remove_file(link)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn android_target_is_explicit() {
        let t = Target::from_args(&["--target".into(), "android-arm64".into()]);
        assert!(t.is_ok(), "android-arm64 must be a supported cross target: {t:?}");
        let t = t.unwrap();
        assert_eq!(t.name(), "android-arm64");
        assert_eq!(t.game_lib(), "libgame.so");
        assert_eq!(t.clang_flags(), ["--target=aarch64-linux-android29"]);
        assert_eq!(t.pic(), ["-fPIC"]);
        assert_eq!(t.build_dir(Path::new("/repo")), Path::new("/repo/build/android-arm64"));
        assert_eq!(
            t.link_flags(),
            ["-Wl,-z,max-page-size=16384", "-Wl,-z,common-page-size=16384", "-Wl,--no-dependent-libraries"]
        );
    }
}
