use std::ffi::{c_int, CStr};

const SYSTEM_DOCK: &str =
    "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock";

#[unsafe(no_mangle)]
pub(crate) extern "C" fn lockdock_display_process_is_dock(pid: c_int) -> bool {
    let mut executable = [0; libc::PROC_PIDPATHINFO_MAXSIZE as usize];
    is_dock_process(pid, |pid| {
        // proc_pidpath writes a NUL-terminated path into the supplied buffer.
        let length = unsafe {
            libc::proc_pidpath(
                pid,
                executable.as_mut_ptr().cast(),
                libc::PROC_PIDPATHINFO_MAXSIZE.cast_unsigned(),
            )
        };
        if length <= 0 {
            return None;
        }
        CStr::from_bytes_until_nul(&executable).ok()?.to_str().ok()
    })
}

fn is_dock_process<'a>(
    pid: c_int,
    executable: impl FnOnce(c_int) -> Option<&'a str>,
) -> bool {
    // Owner names are localized; paths must be rechecked because PIDs are reused.
    pid > 0 && executable(pid) == Some(SYSTEM_DOCK)
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use super::is_dock_process;

    const DOCK: &str = "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock";

    #[test]
    fn recognizes_system_dock_executable() {
        assert!(is_dock_process(42, |_| Some(DOCK)));
    }

    #[test]
    fn rejects_other_executables() {
        for path in [
            "/Applications/Other.app/Contents/MacOS/Dock",
            "/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock-helper",
        ] {
            assert!(!is_dock_process(42, |_| Some(path)), "path: {path:?}");
        }
    }

    #[test]
    fn rejects_invalid_pids_without_looking_them_up() {
        for pid in [0, -1] {
            assert!(!is_dock_process(pid, |_| panic!("invalid PID lookup")));
        }
    }

    #[test]
    fn follows_process_changes() {
        let mut processes = HashMap::from([(42, DOCK)]);
        assert!(is_dock_process(42, |pid| processes.get(&pid).copied()));

        processes.insert(42, "/Applications/Other.app/Contents/MacOS/Dock");
        assert!(!is_dock_process(42, |pid| processes.get(&pid).copied()));

        processes.remove(&42);
        assert!(!is_dock_process(42, |pid| processes.get(&pid).copied()));

        processes.insert(43, DOCK);
        assert!(is_dock_process(43, |pid| processes.get(&pid).copied()));
    }
}
