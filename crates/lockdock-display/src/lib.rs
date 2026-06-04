mod displays;
mod dock;
mod error;
mod ffi;
mod geometry;
mod system_profiler;
mod types;
mod util;

pub use displays::{active_displays, display_bounds, find_display_index};
pub use dock::{
    display_for_rect, dock_orientation, dock_window_display, query_status, DockOrientation,
};
pub use error::{Error, Result};
pub use geometry::Rect;
pub use system_profiler::{load_display_info, DisplayInfo};
pub use types::{DisplayId, DisplayIdentity, Status};
