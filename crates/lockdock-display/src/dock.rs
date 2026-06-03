use std::os::raw::c_char;

use crate::displays::{active_displays, display_bounds};
use crate::error::{Error, Result};
use crate::ffi;
use crate::geometry::{rect_contains_point, rect_intersection, Point, Rect};
use crate::types::{DisplayId, Status};
use crate::util::c_string;

const DOCK_ORIENTATION_BUFFER_SIZE: usize = 32;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DockOrientation {
    Bottom,
    Left,
    Right,
}

pub fn query_status() -> Result<Status> {
    let displays = active_displays();
    let dock_display = dock_display()?;
    let location_index = displays
        .iter()
        .position(|display| *display == dock_display)
        .ok_or(Error::InvalidStatus)?;

    Ok(Status {
        displays,
        location_index,
    })
}

#[must_use]
pub fn dock_orientation() -> DockOrientation {
    let mut buffer = [0 as c_char; DOCK_ORIENTATION_BUFFER_SIZE];
    let copied =
        unsafe { ffi::lockdock_display_copy_dock_orientation(buffer.as_mut_ptr(), buffer.len()) };
    if !copied {
        return DockOrientation::Bottom;
    }

    match c_string(&buffer).as_str() {
        "left" => DockOrientation::Left,
        "right" => DockOrientation::Right,
        _ => DockOrientation::Bottom,
    }
}

#[must_use]
pub fn display_for_rect(rect: Rect) -> Option<DisplayId> {
    if rect.width <= 0.0 || rect.height <= 0.0 {
        return None;
    }

    let mut best = None;
    let mut best_area = 0.0;

    for display_id in active_displays() {
        let Ok(bounds) = display_bounds(display_id) else {
            continue;
        };
        let intersection = rect_intersection(rect, bounds);
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

fn dock_display() -> Result<DisplayId> {
    let Some(display_id) = capture_dock_display() else {
        return if unsafe { ffi::lockdock_display_is_accessibility_trusted() } {
            Err(Error::MissingDockDisplay)
        } else {
            Err(Error::AccessibilityNotTrusted)
        };
    };

    Ok(display_id)
}

#[must_use]
pub fn dock_window_display() -> Option<DisplayId> {
    let mut bounds = Rect::default();
    if unsafe { ffi::lockdock_display_copy_dock_window_bounds(&raw mut bounds) } {
        display_for_rect(bounds)
    } else {
        None
    }
}

fn capture_dock_display() -> Option<DisplayId> {
    let mut bounds = Rect::default();

    if unsafe { ffi::lockdock_display_copy_dock_window_bounds(&raw mut bounds) } {
        if let Some(display_id) = display_for_rect(bounds) {
            return Some(display_id);
        }
    }

    if unsafe { ffi::lockdock_display_copy_accessibility_dock_window_bounds(&raw mut bounds) } {
        return display_for_rect(bounds);
    }

    None
}

fn display_at_point(point: Point) -> Option<DisplayId> {
    active_displays().into_iter().find(|display_id| {
        display_bounds(*display_id)
            .map(|bounds| rect_contains_point(bounds, point))
            .unwrap_or(false)
    })
}
