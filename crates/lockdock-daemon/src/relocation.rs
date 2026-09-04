use std::thread;
use std::time::Duration;

use lockdock_display::{DisplayId, DockOrientation};
use lockdock_geometry::{Point, Rect};
use lockdock_mouse::EventSource;

use crate::{Error, Result};

const RELOCATION_NUDGE_ATTEMPTS: usize = 40;
const RELOCATION_PROBE_INTERVAL: usize = 3;
const RELOCATION_NUDGE_DELAY: Duration = Duration::from_millis(15);
const RELOCATION_VERIFY_ATTEMPTS: usize = 5;
const RELOCATION_VERIFY_DELAY: Duration = Duration::from_millis(10);
const RELOCATION_APPROACH_DELAY: Duration = Duration::from_millis(30);
const RELOCATION_EDGE_MOVE_STEPS: usize = 10;
const RELOCATION_EDGE_MOVE_DELAY: Duration = Duration::from_millis(15);
/// Distance above the bottom edge where the smooth cursor approach starts.
const RELOCATION_APPROACH_OFFSET: f64 = 10.0;
/// How far the cursor may drift from where relocation placed it before the
/// movement is attributed to the user and the relocation is aborted.
const RELOCATION_USER_MOVE_TOLERANCE: f64 = 24.0;

#[derive(Clone, Copy, Debug, Default)]
struct SafeSegment {
    start: f64,
    end: f64,
    width: f64,
    center: f64,
}

enum RelocationOutcome {
    Relocated,
    Interrupted,
    TimedOut,
}

pub(crate) fn relocate_display(display_id: DisplayId) -> Result<()> {
    let bounds = lockdock_display::display_bounds(display_id)?;
    let orientation = lockdock_display::dock_orientation();
    if orientation != DockOrientation::Bottom {
        return Ok(());
    }

    if lockdock_display::dock_overlay_active(display_id) {
        return Err(Error::RelocationInterrupted(
            "a Dock overlay (Mission Control, Exposé or Launchpad) is covering the display"
                .to_owned(),
        ));
    }

    let old_position = current_mouse_location();
    let source = EventSource::new();
    let edge_min = bounds.x;
    let edge_max = bounds.x + bounds.width;
    let edge_y = bounds.y + bounds.height;
    let overlaps = collect_edge_overlaps(display_id, edge_min, edge_max, edge_y);
    let safe_segment = find_safe_edge_segment(edge_min, edge_max, &overlaps);
    let trigger_x = choose_trigger_coordinate(bounds, &overlaps, safe_segment);
    let approach = Point { x: trigger_x, y: edge_y - RELOCATION_APPROACH_OFFSET };
    let edge = Point { x: trigger_x, y: edge_y - 1.0 };

    if let Some(source) = source.as_ref() {
        source.set_suppression_interval(0.0);
    }

    move_cursor(source.as_ref(), approach);
    thread::sleep(RELOCATION_APPROACH_DELAY);

    let outcome = if cursor_taken_by_user(approach) {
        RelocationOutcome::Interrupted
    } else if smooth_move(
        source.as_ref(),
        approach,
        edge,
        RELOCATION_EDGE_MOVE_STEPS,
    ) {
        wait_for_dock_relocation(source.as_ref(), edge, display_id)
    } else {
        RelocationOutcome::Interrupted
    };

    match outcome {
        RelocationOutcome::Relocated => {
            restore_cursor(source.as_ref(), edge, old_position);
            Ok(())
        }
        RelocationOutcome::Interrupted => Err(Error::RelocationInterrupted(
            "the cursor was moved by the user".to_owned(),
        )),
        RelocationOutcome::TimedOut => {
            restore_cursor(source.as_ref(), edge, old_position);
            Err(Error::Operation(format!(
                "Could not move Dock to display {display_id}"
            )))
        }
    }
}

fn find_safe_edge_segment(
    edge_min: f64,
    edge_max: f64,
    overlaps: &[SafeSegment],
) -> SafeSegment {
    let mut best = SafeSegment::default();
    let mut cursor = edge_min;
    for overlap in overlaps {
        if overlap.start > cursor {
            update_best_segment(&mut best, cursor, overlap.start);
        }
        if overlap.end > cursor {
            cursor = overlap.end;
        }
    }
    update_best_segment(&mut best, cursor, edge_max);
    best
}

fn collect_edge_overlaps(
    target_id: DisplayId,
    edge_min: f64,
    edge_max: f64,
    edge_cross_pos: f64,
) -> Vec<SafeSegment> {
    let mut overlaps = Vec::new();

    for display_id in lockdock_display::active_displays() {
        if display_id == target_id {
            continue;
        }
        let Ok(other) = lockdock_display::display_bounds(display_id) else {
            continue;
        };
        let other_min_along = other.x;
        let other_max_along = other.x + other.width;
        let other_min_cross = other.y;
        let other_max_cross = other.y + other.height;

        if other_max_cross < edge_cross_pos - 1.0
            || other_min_cross > edge_cross_pos + 1.0
        {
            continue;
        }

        let start = edge_min.max(other_min_along);
        let end = edge_max.min(other_max_along);
        if end > start && end - start > 2.0 {
            overlaps.push(SafeSegment {
                start,
                end,
                width: end - start,
                center: start + (end - start) / 2.0,
            });
        }
    }

    overlaps.sort_by(|left, right| {
        left.start
            .partial_cmp(&right.start)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| {
                left.end.partial_cmp(&right.end).unwrap_or(std::cmp::Ordering::Equal)
            })
    });

    overlaps
}

fn update_best_segment(best: &mut SafeSegment, start: f64, end: f64) {
    if end <= start {
        return;
    }
    let width = end - start;
    if width > best.width {
        *best = SafeSegment { start, end, width, center: start + width / 2.0 };
    }
}

fn choose_trigger_coordinate(
    bounds: Rect,
    overlaps: &[SafeSegment],
    safe_segment: SafeSegment,
) -> f64 {
    let preferred = (bounds.x + bounds.width - 10.0).max(bounds.x);
    let preferred_has_contact = overlaps
        .iter()
        .any(|overlap| preferred >= overlap.start && preferred <= overlap.end);

    if preferred_has_contact {
        safe_segment.center
    } else {
        preferred
    }
}

fn wait_for_dock_relocation(
    source: Option<&EventSource>,
    edge: Point,
    display_id: DisplayId,
) -> RelocationOutcome {
    for attempt in 0..RELOCATION_NUDGE_ATTEMPTS {
        if let Some(source) = source {
            post_edge_nudge(source, edge);
        }
        thread::sleep(RELOCATION_NUDGE_DELAY);

        if cursor_taken_by_user(edge) {
            return RelocationOutcome::Interrupted;
        }

        if (attempt + 1) % RELOCATION_PROBE_INTERVAL == 0
            && dock_probe_matches(display_id)
        {
            return RelocationOutcome::Relocated;
        }
    }

    for _ in 0..RELOCATION_VERIFY_ATTEMPTS {
        if dock_probe_matches(display_id) {
            return RelocationOutcome::Relocated;
        }
        thread::sleep(RELOCATION_VERIFY_DELAY);
    }

    RelocationOutcome::TimedOut
}

fn dock_probe_matches(display_id: DisplayId) -> bool {
    lockdock_display::dock_window_display() == Some(display_id)
}

fn current_mouse_location() -> Point {
    lockdock_mouse::current_location().unwrap_or_default()
}

fn cursor_taken_by_user(expected: Point) -> bool {
    let current = current_mouse_location();
    (current.x - expected.x).abs() > RELOCATION_USER_MOVE_TOLERANCE
        || (current.y - expected.y).abs() > RELOCATION_USER_MOVE_TOLERANCE
}

fn move_cursor(source: Option<&EventSource>, point: Point) {
    lockdock_mouse::warp(point);
    if let Some(source) = source {
        source.post_moved(point);
    }
}

/// Returns the cursor to where it was before relocation, unless the user has
/// already taken it somewhere else.
fn restore_cursor(
    source: Option<&EventSource>,
    expected: Point,
    old_position: Point,
) {
    if cursor_taken_by_user(expected) {
        return;
    }
    move_cursor(source, old_position);
}

/// Walks the cursor from `from` to `to` with posted events; returns `false`
/// when the user grabs the cursor mid-way.
fn smooth_move(
    source: Option<&EventSource>,
    from: Point,
    to: Point,
    steps: usize,
) -> bool {
    for step in 1..=steps {
        let step = u32::try_from(step).unwrap_or(u32::MAX);
        let steps = u32::try_from(steps).unwrap_or(u32::MAX);
        let progress = f64::from(step) / f64::from(steps);
        let point = Point {
            x: from.x + (to.x - from.x) * progress,
            y: from.y + (to.y - from.y) * progress,
        };
        match source {
            Some(source) => source.post_moved(point),
            None => lockdock_mouse::warp(point),
        }
        thread::sleep(RELOCATION_EDGE_MOVE_DELAY);

        if cursor_taken_by_user(point) {
            return false;
        }
    }
    true
}

fn post_edge_nudge(source: &EventSource, point: Point) {
    source.post_delta(point, 0, 1);
}
