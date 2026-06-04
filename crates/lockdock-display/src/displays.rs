use std::os::raw::c_uint;

use crate::error::{Error, Result};
use crate::ffi;
use crate::geometry::Rect;
use crate::types::DisplayId;

const MAX_DISPLAYS: usize = 32;
const MAX_DISPLAYS_C: c_uint = 32;

#[must_use]
pub fn find_display_index(display_id: DisplayId) -> Option<usize> {
    active_displays()
        .iter()
        .position(|display| *display == display_id)
}

#[must_use]
pub fn active_displays() -> Vec<DisplayId> {
    let mut displays = [0; MAX_DISPLAYS];
    let count =
        unsafe { ffi::lockdock_display_get_active_displays(displays.as_mut_ptr(), MAX_DISPLAYS_C) }
            as usize;

    displays[..count.min(MAX_DISPLAYS)].to_vec()
}

pub fn display_bounds(display_id: DisplayId) -> Result<Rect> {
    let mut bounds = Rect::default();
    if unsafe { ffi::lockdock_display_copy_bounds(display_id, &raw mut bounds) } {
        Ok(bounds)
    } else {
        Err(Error::DisplayNotFound(display_id))
    }
}
