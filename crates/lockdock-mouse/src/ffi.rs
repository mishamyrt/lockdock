use std::os::raw::{c_char, c_int, c_longlong, c_void};

use crate::Point;

extern "C" {
    pub(crate) fn lockdock_mouse_copy_location(point_out: *mut Point) -> bool;
    pub(crate) fn lockdock_mouse_event_source_create() -> *mut c_void;
    pub(crate) fn lockdock_mouse_event_source_set_suppression_interval(
        source: *mut c_void,
        interval: f64,
    );
    pub(crate) fn lockdock_mouse_release(object: *mut c_void);
    pub(crate) fn lockdock_mouse_warp(point: Point);
    pub(crate) fn lockdock_mouse_post_moved(source: *mut c_void, point: Point);
    pub(crate) fn lockdock_mouse_post_delta(
        source: *mut c_void,
        point: Point,
        delta_x: c_longlong,
        delta_y: c_longlong,
    );
    pub(crate) fn lockdock_mouse_start_event_tap(error: *mut c_char, error_size: usize) -> bool;
    pub(crate) fn lockdock_mouse_stop_event_tap();
}

pub(crate) const EVENT_MOUSE_MOVED: c_int = 1;
pub(crate) const EVENT_MOUSE_DRAGGED: c_int = 2;
