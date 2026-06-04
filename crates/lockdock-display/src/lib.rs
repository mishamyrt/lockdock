mod displays;
mod dock;
mod error;
mod ffi;
mod system_profiler;
mod types;

pub use displays::{active_displays, display_bounds};
pub use dock::{dock_orientation, dock_window_display, query_status, DockOrientation};
pub use error::{Error, Result};
pub use system_profiler::{load_display_info, DisplayInfo};
pub use types::{DisplayId, DisplayIdentity, Status};
