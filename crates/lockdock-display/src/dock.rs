use lockdock_geometry::Rect;
use prefs::{Key, Preferences};

use crate::displays::display_for_rect;
use crate::ffi;
use crate::{DisplayId, Error, Result};

const DOCK_BUNDLE_ID: &str = "com.apple.dock";
const DOCK_ORIENTATION: Key<String> = Key::new("orientation");

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DockOrientation {
    Bottom,
    Left,
    Right,
}

#[must_use]
pub fn dock_orientation() -> DockOrientation {
    let orientation = Preferences::new(DOCK_BUNDLE_ID)
        .ok()
        .and_then(|preferences| preferences.get(DOCK_ORIENTATION).ok())
        .flatten();

    match orientation.as_deref() {
        Some("left") => DockOrientation::Left,
        Some("right") => DockOrientation::Right,
        _ => DockOrientation::Bottom,
    }
}

pub fn dock_display() -> Result<DisplayId> {
    let Some(display_id) = capture_dock_display() else {
        return if unsafe { ffi::lockdock_display_is_accessibility_trusted() } {
            Err(Error::MissingDockDisplay)
        } else {
            Err(Error::AccessibilityNotTrusted)
        };
    };

    Ok(display_id)
}

/// Reports whether a shield window (Mission Control, Exposé or Launchpad)
/// currently covers the given display. While one is up, the Dock cannot be
/// summoned to that display by pushing at its edge.
#[must_use]
pub fn dock_overlay_active(display_id: DisplayId) -> bool {
    unsafe { ffi::lockdock_display_dock_overlay_active(display_id) }
}

#[must_use]
pub fn dock_window_display() -> Option<DisplayId> {
    let mut bounds = Rect::default();
    if unsafe { ffi::lockdock_display_copy_dock_window_bounds(&raw mut bounds) } {
        display_for_rect(bounds)
    } else {
        None
    }
}

fn capture_dock_display() -> Option<DisplayId> {
    let mut bounds = Rect::default();

    if unsafe { ffi::lockdock_display_copy_dock_window_bounds(&raw mut bounds) } {
        if let Some(display_id) = display_for_rect(bounds) {
            return Some(display_id);
        }
    }

    if unsafe {
        ffi::lockdock_display_copy_accessibility_dock_window_bounds(&raw mut bounds)
    } {
        return display_for_rect(bounds);
    }

    None
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::ffi::{c_char, c_int, c_void, CStr, CString};
    use std::ptr::{self, NonNull};

    const TEST_PID: c_int = 12345;
    const SYSTEM_DOCK: &CStr =
        c"/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock";

    struct MockProcess {
        pid: c_int,
        path: Option<&'static CStr>,
        queries: usize,
    }

    thread_local! {
        static PROCESS: RefCell<MockProcess> = const { RefCell::new(MockProcess {
            pid: TEST_PID,
            path: Some(SYSTEM_DOCK),
            queries: 0,
        }) };
    }

    extern "C" {
        fn CFStringCreateWithCString(
            allocator: *const c_void,
            text: *const c_char,
            encoding: u32,
        ) -> *const c_void;
        fn CFNumberCreate(
            allocator: *const c_void,
            number_type: isize,
            value: *const c_void,
        ) -> *const c_void;
        fn CFDictionaryCreateMutable(
            allocator: *const c_void,
            capacity: isize,
            key_callbacks: *const c_void,
            value_callbacks: *const c_void,
        ) -> *mut c_void;
        fn CFDictionarySetValue(
            dictionary: *mut c_void,
            key: *const c_void,
            value: *const c_void,
        );
        fn CFRelease(value: *const c_void);

        static kCGWindowOwnerPID: *const c_void;
        static kCGWindowOwnerName: *const c_void;
        static kCGWindowLayer: *const c_void;

        fn lockdock_display_test_owner_is_dock(window: *const c_void) -> bool;
        fn lockdock_display_test_dock_bar_window(
            window: *const c_void,
            dock_level: c_int,
        ) -> bool;
    }

    // Called synchronously by the test-only native predicates. Each Rust test
    // gets independent mock process data even when Cargo runs tests in parallel.
    #[unsafe(no_mangle)]
    unsafe extern "C" fn lockdock_display_test_proc_pidpath(
        pid: c_int,
        buffer: *mut c_void,
        capacity: u32,
    ) -> c_int {
        PROCESS.with_borrow_mut(|process| {
            process.queries += 1;
            let Some(path) = process.path.filter(|_| process.pid == pid) else {
                return 0;
            };
            let bytes = path.to_bytes_with_nul();
            if buffer.is_null()
                || bytes.len() > usize::try_from(capacity).unwrap_or(0)
            {
                return 0;
            }
            // The native caller supplies a writable buffer of `capacity` bytes.
            unsafe {
                ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.cast(), bytes.len());
            }
            c_int::try_from(bytes.len() - 1).unwrap_or(0)
        })
    }

    fn set_process(pid: c_int, path: Option<&'static CStr>) {
        PROCESS.with_borrow_mut(|process| {
            *process = MockProcess { pid, path, queries: 0 };
        });
    }

    fn path_queries() -> usize {
        PROCESS.with_borrow(|process| process.queries)
    }

    // Own only objects returned by Core Foundation Create functions. Constant
    // CoreGraphics keys are borrowed separately and must never be released here.
    struct CFValue(NonNull<c_void>);

    impl CFValue {
        fn from_created(value: *const c_void) -> Self {
            Self(NonNull::new(value.cast_mut()).expect("failed to create CF object"))
        }

        fn as_ptr(&self) -> *const c_void {
            self.0.as_ptr().cast_const()
        }

        fn string(value: &str) -> Self {
            const UTF8: u32 = 0x0800_0100;
            let value = CString::new(value).expect("test string contains NUL");
            // CFStringCreateWithCString copies this NUL-terminated UTF-8 buffer.
            Self::from_created(unsafe {
                CFStringCreateWithCString(ptr::null(), value.as_ptr(), UTF8)
            })
        }

        fn number(value: i32) -> Self {
            const SINT32: isize = 3;
            // The input's width matches kCFNumberSInt32Type and is copied.
            Self::from_created(unsafe {
                CFNumberCreate(ptr::null(), SINT32, (&raw const value).cast())
            })
        }

        fn number64(value: i64) -> Self {
            const SINT64: isize = 4;
            // The input's width matches kCFNumberSInt64Type and is copied.
            Self::from_created(unsafe {
                CFNumberCreate(ptr::null(), SINT64, (&raw const value).cast())
            })
        }
    }

    impl Drop for CFValue {
        fn drop(&mut self) {
            // Each owned Create reference is released exactly once.
            unsafe { CFRelease(self.as_ptr()) };
        }
    }

    struct TestWindow<'a> {
        dictionary: CFValue,
        _pid: Option<&'a CFValue>,
        _owner: Option<CFValue>,
        _layer: CFValue,
    }

    fn window<'a>(
        pid: Option<&'a CFValue>,
        owner: Option<&str>,
        layer: i32,
    ) -> TestWindow<'a> {
        // NULL callbacks mean the dictionary borrows its keys and values.
        // Keys are immortal CoreGraphics constants. TestWindow owns the layer
        // and name and borrows the PID, keeping all values alive until after
        // the dictionary is released. Lookups use the same constant key pointers.
        let dictionary = CFValue::from_created(unsafe {
            CFDictionaryCreateMutable(ptr::null(), 0, ptr::null(), ptr::null())
        });
        let layer = CFValue::number(layer);
        let owner = owner.map(CFValue::string);
        unsafe {
            CFDictionarySetValue(
                dictionary.as_ptr().cast_mut(),
                kCGWindowLayer,
                layer.as_ptr(),
            );
            if let Some(pid) = pid {
                CFDictionarySetValue(
                    dictionary.as_ptr().cast_mut(),
                    kCGWindowOwnerPID,
                    pid.as_ptr(),
                );
            }
            if let Some(owner) = &owner {
                CFDictionarySetValue(
                    dictionary.as_ptr().cast_mut(),
                    kCGWindowOwnerName,
                    owner.as_ptr(),
                );
            }
        }
        TestWindow { dictionary, _pid: pid, _owner: owner, _layer: layer }
    }

    fn is_dock(window: &TestWindow<'_>) -> bool {
        // The dictionary and all of its values outlive the synchronous call.
        unsafe { lockdock_display_test_owner_is_dock(window.dictionary.as_ptr()) }
    }

    #[test]
    fn recognizes_localized_and_missing_owner_names() {
        set_process(TEST_PID, Some(SYSTEM_DOCK));
        let pid = CFValue::number(TEST_PID);
        for owner in [Some("Dock"), Some("程序坞"), Some("Arbitrary name"), None]
        {
            assert!(is_dock(&window(Some(&pid), owner, 20)), "owner: {owner:?}");
        }
    }

    #[test]
    fn rejects_other_executables_with_the_dock_name() {
        let pid = CFValue::number(TEST_PID);
        for path in [
            c"/Applications/Other.app/Contents/MacOS/Dock",
            c"/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock-helper",
        ] {
            set_process(TEST_PID, Some(path));
            assert!(!is_dock(&window(Some(&pid), Some("Dock"), 20)));
        }
    }

    #[test]
    fn rechecks_executable_when_pid_is_reused() {
        let pid = CFValue::number(TEST_PID);
        let window = window(Some(&pid), Some("Dock"), 20);
        set_process(TEST_PID, Some(SYSTEM_DOCK));
        assert!(is_dock(&window));
        set_process(TEST_PID, Some(c"/Applications/Other.app/Contents/MacOS/Dock"));
        assert!(!is_dock(&window));
        assert_eq!(path_queries(), 1);
    }

    #[test]
    fn recognizes_new_dock_pid_after_restart() {
        let old_pid = CFValue::number(TEST_PID);
        let new_pid = CFValue::number(TEST_PID + 1);
        set_process(TEST_PID, Some(SYSTEM_DOCK));
        assert!(is_dock(&window(Some(&old_pid), None, 20)));
        set_process(TEST_PID + 1, Some(SYSTEM_DOCK));
        assert!(!is_dock(&window(Some(&old_pid), None, 20)));
        assert!(is_dock(&window(Some(&new_pid), None, 20)));
    }

    #[test]
    fn rejects_exited_processes() {
        set_process(TEST_PID, None);
        let pid = CFValue::number(TEST_PID);
        assert!(!is_dock(&window(Some(&pid), Some("Dock"), 20)));
        assert_eq!(path_queries(), 1);
    }

    #[test]
    fn rejects_invalid_pids_without_querying_processes() {
        set_process(TEST_PID, Some(SYSTEM_DOCK));
        assert!(!is_dock(&window(None, Some("Dock"), 20)));
        for pid in [
            CFValue::string("12345"),
            CFValue::number(0),
            CFValue::number(-1),
            CFValue::number64(i64::MAX),
        ] {
            assert!(!is_dock(&window(Some(&pid), Some("Dock"), 20)));
        }
        assert_eq!(path_queries(), 0);
    }

    #[test]
    fn skips_process_lookups_for_unrelated_window_layers() {
        set_process(TEST_PID, Some(SYSTEM_DOCK));
        let pid = CFValue::number(TEST_PID);
        let window = window(Some(&pid), Some("Dock"), 0);
        // Only the layer is relevant; rejection happens before other fields.
        assert!(!unsafe {
            lockdock_display_test_dock_bar_window(window.dictionary.as_ptr(), 20)
        });
        assert_eq!(path_queries(), 0);
    }
}
