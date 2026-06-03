use std::os::raw::{c_char, c_uint};

use crate::geometry::Rect;

extern "C" {
    pub(crate) fn lockdock_display_get_active_displays(
        displays: *mut c_uint,
        max_displays: c_uint,
    ) -> c_uint;
    pub(crate) fn lockdock_display_copy_bounds(display_id: c_uint, rect_out: *mut Rect) -> bool;
    pub(crate) fn lockdock_display_is_accessibility_trusted() -> bool;
    pub(crate) fn lockdock_display_copy_dock_window_bounds(rect_out: *mut Rect) -> bool;
    pub(crate) fn lockdock_display_copy_accessibility_dock_window_bounds(
        rect_out: *mut Rect,
    ) -> bool;
    pub(crate) fn lockdock_display_copy_dock_orientation(
        buffer: *mut c_char,
        buffer_size: usize,
    ) -> bool;
}
