use std::os::raw::c_longlong;

use crate::{ffi, Point};

pub struct EventSource {
    raw: *mut std::os::raw::c_void,
}

impl EventSource {
    #[must_use]
    pub fn new() -> Option<Self> {
        let raw = unsafe { ffi::lockdock_mouse_event_source_create() };
        (!raw.is_null()).then_some(Self { raw })
    }

    pub fn set_suppression_interval(&self, interval: f64) {
        unsafe { ffi::lockdock_mouse_event_source_set_suppression_interval(self.raw, interval) };
    }

    pub fn post_moved(&self, point: Point) {
        unsafe { ffi::lockdock_mouse_post_moved(self.raw, point) };
    }

    pub fn post_delta(&self, point: Point, delta_x: c_longlong, delta_y: c_longlong) {
        unsafe { ffi::lockdock_mouse_post_delta(self.raw, point, delta_x, delta_y) };
    }
}

impl Drop for EventSource {
    fn drop(&mut self) {
        unsafe { ffi::lockdock_mouse_release(self.raw) };
    }
}
