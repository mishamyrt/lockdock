use std::thread;
use std::time::Duration;

use lockdock_display::{DisplayId, DockOrientation};
use lockdock_geometry::{Point, Rect};
use lockdock_mouse::EventSource;

use crate::{Error, Result};

const RELOCATION_NUDGE_ATTEMPTS: usize = 60;
const RELOCATION_PROBE_INTERVAL: usize = 3;
const RELOCATION_NUDGE_DELAY: Duration = Duration::from_micros(15_000);
const RELOCATION_VERIFY_ATTEMPTS: usize = 8;
const RELOCATION_VERIFY_DELAY: Duration = Duration::from_micros(10_000);
const RELOCATION_APPROACH_DELAY: Duration = Duration::from_micros(30_000);
const RELOCATION_EDGE_MOVE_STEPS: usize = 10;
const RELOCATION_EDGE_MOVE_DELAY: Duration = Duration::from_micros(15_000);

#[derive(Clone, Copy, Debug, Default)]
struct SafeSegment {
    start: f64,
    end: f64,
    width: f64,
    center: f64,
}

pub(crate) fn relocate_display(display_id: DisplayId) -> Result<()> {
    let bounds = lockdock_display::display_bounds(display_id)?;
    let orientation = lockdock_display::dock_orientation();
    let old_position = current_mouse_location();
    let source = EventSource::new();
    let safe_segment = find_safe_edge_segment(display_id, orientation);
    let edge_offset = 1.0;

    let (approach, edge) = match orientation {
        DockOrientation::Bottom => {
            let edge_y = bounds.y + bounds.height;
            let trigger_x = choose_trigger_coordinate(display_id, orientation, safe_segment);
            (
                Point {
                    x: trigger_x,
                    y: edge_y - edge_offset,
                },
                Point {
                    x: trigger_x,
                    y: edge_y - 1.0,
                },
            )
        }
        DockOrientation::Left => {
            let edge_x = bounds.x;
            let trigger_y = choose_trigger_coordinate(display_id, orientation, safe_segment);
            (
                Point {
                    x: edge_x + edge_offset,
                    y: trigger_y,
                },
                Point {
                    x: edge_x + 1.0,
                    y: trigger_y,
                },
            )
        }
        DockOrientation::Right => {
            let edge_x = bounds.x + bounds.width;
            let trigger_y = choose_trigger_coordinate(display_id, orientation, safe_segment);
            (
                Point {
                    x: edge_x - edge_offset,
                    y: trigger_y,
                },
                Point {
                    x: edge_x - 1.0,
                    y: trigger_y,
                },
            )
        }
    };

    if let Some(source) = source.as_ref() {
        source.set_suppression_interval(0.0);
    }

    move_cursor(source.as_ref(), approach);
    thread::sleep(RELOCATION_APPROACH_DELAY);
    smooth_move(source.as_ref(), approach, edge, RELOCATION_EDGE_MOVE_STEPS);

    let relocated = wait_for_dock_relocation(source.as_ref(), edge, orientation, display_id);

    move_cursor(source.as_ref(), old_position);

    if relocated {
        Ok(())
    } else {
        Err(Error::Operation(format!(
            "Could not move Dock to display {display_id}"
        )))
    }
}

fn find_safe_edge_segment(target_id: DisplayId, edge: DockOrientation) -> SafeSegment {
    let Ok(target) = lockdock_display::display_bounds(target_id) else {
        return SafeSegment::default();
    };
    let (edge_min, edge_max, edge_cross_pos) = edge_geometry(target, edge);
    let mut overlaps = collect_edge_overlaps(target_id, edge, edge_min, edge_max, edge_cross_pos);
    overlaps.sort_by(|left, right| {
        left.start
            .partial_cmp(&right.start)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| {
                left.end
                    .partial_cmp(&right.end)
                    .unwrap_or(std::cmp::Ordering::Equal)
            })
    });

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

fn edge_point_has_contact(
    target_id: DisplayId,
    edge: DockOrientation,
    point_along_edge: f64,
) -> bool {
    let Ok(target) = lockdock_display::display_bounds(target_id) else {
        return false;
    };
    let (edge_min, edge_max, edge_cross_pos) = edge_geometry(target, edge);
    if point_along_edge < edge_min || point_along_edge > edge_max {
        return false;
    }

    collect_edge_overlaps(target_id, edge, edge_min, edge_max, edge_cross_pos)
        .iter()
        .any(|overlap| point_along_edge >= overlap.start && point_along_edge <= overlap.end)
}

fn collect_edge_overlaps(
    target_id: DisplayId,
    edge: DockOrientation,
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
        let (other_min_along, other_max_along, other_min_cross, other_max_cross) =
            if edge == DockOrientation::Bottom {
                (
                    other.x,
                    other.x + other.width,
                    other.y,
                    other.y + other.height,
                )
            } else {
                (
                    other.y,
                    other.y + other.height,
                    other.x,
                    other.x + other.width,
                )
            };

        if other_max_cross < edge_cross_pos - 1.0 || other_min_cross > edge_cross_pos + 1.0 {
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

    overlaps
}

fn edge_geometry(bounds: Rect, edge: DockOrientation) -> (f64, f64, f64) {
    match edge {
        DockOrientation::Bottom => (bounds.x, bounds.x + bounds.width, bounds.y + bounds.height),
        DockOrientation::Left => (bounds.y, bounds.y + bounds.height, bounds.x),
        DockOrientation::Right => (bounds.y, bounds.y + bounds.height, bounds.x + bounds.width),
    }
}

fn update_best_segment(best: &mut SafeSegment, start: f64, end: f64) {
    if end <= start {
        return;
    }
    let width = end - start;
    if width > best.width {
        *best = SafeSegment {
            start,
            end,
            width,
            center: start + width / 2.0,
        };
    }
}

fn choose_trigger_coordinate(
    target_display_id: DisplayId,
    orientation: DockOrientation,
    safe_segment: SafeSegment,
) -> f64 {
    let Ok(bounds) = lockdock_display::display_bounds(target_display_id) else {
        return safe_segment.center;
    };
    let preferred = if orientation == DockOrientation::Bottom {
        (bounds.x + bounds.width - 10.0).max(bounds.x)
    } else {
        (bounds.y + bounds.height - 10.0).max(bounds.y)
    };

    if edge_point_has_contact(target_display_id, orientation, preferred) {
        safe_segment.center
    } else {
        preferred
    }
}

fn wait_for_dock_relocation(
    source: Option<&EventSource>,
    edge: Point,
    orientation: DockOrientation,
    display_id: DisplayId,
) -> bool {
    for attempt in 0..RELOCATION_NUDGE_ATTEMPTS {
        if let Some(source) = source {
            post_edge_nudge(source, edge, orientation);
        }
        thread::sleep(RELOCATION_NUDGE_DELAY);

        if (attempt + 1) % RELOCATION_PROBE_INTERVAL == 0 && dock_probe_matches(display_id) {
            return true;
        }
    }

    for _ in 0..RELOCATION_VERIFY_ATTEMPTS {
        if dock_probe_matches(display_id) {
            return true;
        }
        thread::sleep(RELOCATION_VERIFY_DELAY);
    }

    false
}

fn dock_probe_matches(display_id: DisplayId) -> bool {
    lockdock_display::dock_window_display() == Some(display_id)
}

fn current_mouse_location() -> Point {
    lockdock_mouse::current_location().unwrap_or_default()
}

fn move_cursor(source: Option<&EventSource>, point: Point) {
    lockdock_mouse::warp(point);
    if let Some(source) = source {
        source.post_moved(point);
    }
}

fn smooth_move(source: Option<&EventSource>, from: Point, to: Point, steps: usize) {
    for step in 1..=steps {
        let step = u32::try_from(step).unwrap_or(u32::MAX);
        let steps = u32::try_from(steps).unwrap_or(u32::MAX);
        let progress = f64::from(step) / f64::from(steps);
        let point = Point {
            x: from.x + (to.x - from.x) * progress,
            y: from.y + (to.y - from.y) * progress,
        };
        lockdock_mouse::warp(point);
        if let Some(source) = source {
            source.post_moved(point);
        }
        thread::sleep(RELOCATION_EDGE_MOVE_DELAY);
    }
}

fn post_edge_nudge(source: &EventSource, point: Point, orientation: DockOrientation) {
    match orientation {
        DockOrientation::Bottom => source.post_delta(point, 0, 1),
        DockOrientation::Left => source.post_delta(point, -1, 0),
        DockOrientation::Right => source.post_delta(point, 1, 0),
    }
}
