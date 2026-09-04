use std::os::raw::c_uint;

use lockdock_geometry::Rect;

extern "C" {
    pub(crate) fn lockdock_display_get_active_displays(
        displays: *mut c_uint,
        max_displays: c_uint,
    ) -> c_uint;
    pub(crate) fn lockdock_display_copy_bounds(
        display_id: c_uint,
        rect_out: *mut Rect,
    ) -> bool;
    pub(crate) fn lockdock_display_is_accessibility_trusted() -> bool;
    pub(crate) fn lockdock_display_copy_dock_window_bounds(
        rect_out: *mut Rect,
    ) -> bool;
    pub(crate) fn lockdock_display_copy_accessibility_dock_window_bounds(
        rect_out: *mut Rect,
    ) -> bool;
    pub(crate) fn lockdock_display_dock_overlay_active(display_id: c_uint) -> bool;
}
