use std::fmt;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

static VERBOSE_LOGGING: AtomicBool = AtomicBool::new(false);

pub(crate) fn set_verbose(enabled: bool) {
    VERBOSE_LOGGING.store(enabled, Ordering::Relaxed);
}

pub(crate) fn verbose_enabled() -> bool {
    VERBOSE_LOGGING.load(Ordering::Relaxed)
}

#[allow(clippy::print_stderr)]
pub(crate) fn log(level: &str, message: fmt::Arguments<'_>) {
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();

    eprintln!("[{timestamp}] {level}: {message}");
}

#[macro_export]
macro_rules! log_info {
    ($($arg:tt)*) => {
        if $crate::logging::verbose_enabled() {
            $crate::logging::log("INFO", format_args!($($arg)*))
        }
    };
}

#[macro_export]
macro_rules! log_warn {
    ($($arg:tt)*) => {
        if $crate::logging::verbose_enabled() {
            $crate::logging::log("WARN", format_args!($($arg)*))
        }
    };
}

#[macro_export]
macro_rules! log_error {
    ($($arg:tt)*) => {
        $crate::logging::log("ERROR", format_args!($($arg)*))
    };
}
