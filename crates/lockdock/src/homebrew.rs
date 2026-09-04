use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Resolve brew beside the Cellar containing the canonical executable path.
pub(crate) fn executable_for(program: &Path) -> Option<PathBuf> {
    let bin = program.parent()?;
    let formula = bin.parent()?.parent()?;
    let cellar = formula.parent()?;
    if program.file_name()? != "lockdock"
        || bin.file_name()? != "bin"
        || formula.file_name()? != "lockdock"
        || cellar.file_name()? != "Cellar"
    {
        return None;
    }

    Some(cellar.parent()?.join("bin/brew"))
}

pub(crate) fn service(brew: &Path, action: &str) -> io::Result<()> {
    let status =
        Command::new(brew).args(["services", action, "lockdock"]).status()?;
    if !status.success() {
        return Err(io::Error::other(format!(
            "brew services {action} failed: {status}"
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::os::unix::fs::PermissionsExt as _;
    use std::sync::atomic::{AtomicUsize, Ordering};

    use super::*;

    static NEXT_TEST_ID: AtomicUsize = AtomicUsize::new(0);

    struct TestBrew(PathBuf);

    impl TestBrew {
        fn new(script: &str) -> Self {
            let id = NEXT_TEST_ID.fetch_add(1, Ordering::Relaxed);
            let directory = std::env::temp_dir()
                .join(format!("lockdock-brew-{}-{id}", std::process::id()));
            fs::create_dir(&directory).unwrap();
            let brew = directory.join("brew");
            fs::write(&brew, script).unwrap();
            fs::set_permissions(&brew, fs::Permissions::from_mode(0o755)).unwrap();
            Self(brew)
        }
    }

    impl Drop for TestBrew {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(self.0.parent().unwrap());
        }
    }

    #[test]
    fn finds_brew_for_cellar_installations() {
        for prefix in ["/opt/homebrew", "/usr/local", "/custom/homebrew"] {
            let prefix = Path::new(prefix);
            let program = prefix.join("Cellar/lockdock/0.5.0/bin/lockdock");
            assert_eq!(executable_for(&program), Some(prefix.join("bin/brew")));
        }
    }

    #[test]
    fn ignores_standalone_and_other_formula_installations() {
        for program in [
            "/Users/example/.local/bin/lockdock",
            "/repo/target/release/lockdock",
            "/opt/homebrew/Cellar/other/0.5.0/bin/lockdock",
        ] {
            assert_eq!(executable_for(Path::new(program)), None);
        }
    }

    #[test]
    fn delegates_start_and_stop_to_brew_services() {
        let brew =
            TestBrew::new("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$0.calls\"\n");
        service(&brew.0, "start").unwrap();
        service(&brew.0, "stop").unwrap();
        assert_eq!(
            fs::read_to_string(brew.0.with_extension("calls")).unwrap(),
            "services start lockdock\nservices stop lockdock\n"
        );
    }

    #[test]
    fn reports_brew_failure() {
        let brew = TestBrew::new("#!/bin/sh\nexit 7\n");
        let error = service(&brew.0, "start").unwrap_err();
        assert!(error.to_string().contains("brew services start failed"));
        assert!(error.to_string().contains('7'));
    }
}
