mod displays;
mod dock;
mod ffi;
mod system_profiler;

pub use displays::{active_displays, display_bounds};
pub use dock::{dock_display, dock_orientation, dock_window_display, DockOrientation};
pub use system_profiler::{load_display_info, DisplayInfo};

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0}")]
    Native(String),
    #[error("display {0} not found")]
    DisplayNotFound(DisplayId),

    #[error("could not determine current Dock display")]
    MissingDockDisplay,
    #[error("could not determine current Dock display (Accessibility permission is not granted)")]
    AccessibilityNotTrusted,
}

pub type Result<T> = std::result::Result<T, Error>;

pub type DisplayId = u32;
