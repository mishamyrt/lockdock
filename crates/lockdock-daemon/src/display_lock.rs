use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, OnceLock};

use lockdock_display::{DisplayId, DockOrientation, Rect};
use lockdock_mouse::{EventTap, MouseEvent, MouseEventKind, Point};

use crate::{Error, Result};

const LOCK_EDGE_ZONE: f64 = 4.0;

#[derive(Clone, Copy, Debug)]
struct DisplayBounds {
    display_id: DisplayId,
    bounds: Rect,
}

static LOCK_TARGET: AtomicU32 = AtomicU32::new(0);
static DISPLAY_CACHE: OnceLock<Mutex<Vec<DisplayBounds>>> = OnceLock::new();
static EVENT_TAP: OnceLock<Mutex<Option<EventTap>>> = OnceLock::new();

pub(crate) fn set_lock_target(display_id: DisplayId) -> Result<()> {
    let event_tap = EVENT_TAP.get_or_init(|| Mutex::new(None));
    let mut event_tap = event_tap
        .lock()
        .map_err(|_| Error::Operation("mouse event tap mutex poisoned".to_owned()))?;

    if event_tap.is_none() {
        *event_tap = Some(
            EventTap::start(should_suppress_event)
                .map_err(|error| Error::Operation(error.to_string()))?,
        );
    }

    refresh_display_cache();
    LOCK_TARGET.store(display_id, Ordering::SeqCst);
    Ok(())
}

pub(crate) fn clear_lock_target() {
    LOCK_TARGET.store(0, Ordering::SeqCst);
    if let Some(event_tap) = EVENT_TAP.get() {
        if let Ok(mut event_tap) = event_tap.lock() {
            *event_tap = None;
        }
    }
}

#[must_use]
pub(crate) fn lock_target() -> Option<DisplayId> {
    match LOCK_TARGET.load(Ordering::SeqCst) {
        0 => None,
        display_id => Some(display_id),
    }
}

pub(crate) fn shutdown() {
    clear_lock_target();
}

pub(crate) fn refresh_display_cache() {
    let cache = DISPLAY_CACHE.get_or_init(|| Mutex::new(Vec::new()));
    let mut next = Vec::new();

    for display_id in lockdock_display::active_displays() {
        if let Ok(bounds) = lockdock_display::display_bounds(display_id) {
            next.push(DisplayBounds { display_id, bounds });
        }
    }

    if let Ok(mut cache) = cache.lock() {
        *cache = next;
    }
}

fn should_suppress_event(event: MouseEvent) -> bool {
    if event.kind == MouseEventKind::Other {
        return false;
    }

    let locked_display = LOCK_TARGET.load(Ordering::SeqCst);
    if locked_display == 0 {
        return false;
    }

    let Some(current) = cached_display_at_point(event.location) else {
        return false;
    };

    if current.display_id == locked_display {
        return false;
    }

    let distance = distance_from_dock_edge(
        event.location,
        current.bounds,
        lockdock_display::dock_orientation(),
    );
    (0.0..=LOCK_EDGE_ZONE).contains(&distance)
}

fn cached_display_at_point(point: Point) -> Option<DisplayBounds> {
    let cache = DISPLAY_CACHE.get_or_init(|| Mutex::new(Vec::new()));
    cache
        .lock()
        .expect("display cache mutex poisoned")
        .iter()
        .copied()
        .find(|display| rect_contains_point(display.bounds, point))
}

fn distance_from_dock_edge(point: Point, bounds: Rect, orientation: DockOrientation) -> f64 {
    match orientation {
        DockOrientation::Left => point.x - bounds.x,
        DockOrientation::Right => (bounds.x + bounds.width) - point.x,
        DockOrientation::Bottom => (bounds.y + bounds.height) - point.y,
    }
}

fn rect_contains_point(rect: Rect, point: Point) -> bool {
    point.x >= rect.x
        && point.x < rect.x + rect.width
        && point.y >= rect.y
        && point.y < rect.y + rect.height
}
