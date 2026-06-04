use lockdock_geometry::Rect;
use prefs::{Key, Preferences};

use crate::displays::display_for_rect;
use crate::ffi;
use crate::{DisplayId, Error, Result};

const DOCK_BUNDLE_ID: &str = "com.apple.dock";
const DOCK_ORIENTATION: Key<String> = Key::new("orientation");

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DockOrientation {
    Bottom,
    Left,
    Right,
}

#[must_use]
pub fn dock_orientation() -> DockOrientation {
    let orientation = Preferences::new(DOCK_BUNDLE_ID)
        .ok()
        .and_then(|preferences| preferences.get(DOCK_ORIENTATION).ok())
        .flatten();

    match orientation.as_deref() {
        Some("left") => DockOrientation::Left,
        Some("right") => DockOrientation::Right,
        _ => DockOrientation::Bottom,
    }
}

pub fn dock_display() -> Result<DisplayId> {
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
