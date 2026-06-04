use std::os::raw::c_uint;

use lockdock_geometry::{Point, Rect};

use crate::ffi;
use crate::{DisplayId, Error, Result};

const MAX_DISPLAYS: usize = 32;
const MAX_DISPLAYS_C: c_uint = 32;

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

fn display_at_point(point: Point) -> Option<DisplayId> {
    active_displays().into_iter().find(|display_id| {
        display_bounds(*display_id)
            .map(|bounds| bounds.contains(point))
            .unwrap_or(false)
    })
}

#[must_use]
pub(crate) fn display_for_rect(rect: Rect) -> Option<DisplayId> {
    if rect.width <= 0.0 || rect.height <= 0.0 {
        return None;
    }

    let mut best = None;
    let mut best_area = 0.0;

    for display_id in active_displays() {
        let Ok(bounds) = display_bounds(display_id) else {
            continue;
        };
        let intersection = rect.intersection(bounds);
        let area = intersection.width * intersection.height;

        if area > best_area {
            best_area = area;
            best = Some(display_id);
        }
    }

    best.or_else(|| {
        display_at_point(Point {
            x: rect.x + rect.width / 2.0,
            y: rect.y + rect.height / 2.0,
        })
    })
}
