use serde_json::Value;
use std::collections::HashMap;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_uint, c_void};
use std::process::Command;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant};

const MAX_DISPLAYS: usize = 32;
const MAX_DISPLAYS_C: c_uint = 32;
const DISPLAY_UUID_BUFFER_SIZE: usize = 64;
const DOCK_ORIENTATION_BUFFER_SIZE: usize = 32;
const ERROR_BUFFER_SIZE: usize = 512;
const SYSTEM_PROFILER_CACHE_TTL: Duration = Duration::from_secs(5);
const LOCK_EDGE_ZONE: f64 = 4.0;
const RELOCATION_NUDGE_ATTEMPTS: usize = 60;
const RELOCATION_PROBE_INTERVAL: usize = 3;
const RELOCATION_NUDGE_DELAY: Duration = Duration::from_micros(15_000);
const RELOCATION_VERIFY_ATTEMPTS: usize = 8;
const RELOCATION_VERIFY_DELAY: Duration = Duration::from_micros(10_000);
const RELOCATION_APPROACH_DELAY: Duration = Duration::from_micros(30_000);
const RELOCATION_EDGE_MOVE_STEPS: usize = 10;
const RELOCATION_EDGE_MOVE_DELAY: Duration = Duration::from_micros(15_000);

pub type DisplayId = u32;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DisplayIdentity {
    pub is_builtin: bool,
    pub vendor_number: u32,
    pub model_number: u32,
    pub serial_number: u32,
    pub uuid: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Status {
    pub displays: Vec<DisplayId>,
    pub location_index: usize,
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0}")]
    Native(String),
    #[error("invalid display status")]
    InvalidStatus,
    #[error("could not identify target display")]
    MissingIdentity,
    #[error("display {0} not found")]
    DisplayNotFound(DisplayId),
    #[error("could not determine current Dock display")]
    MissingDockDisplay,
    #[error("could not determine current Dock display (Accessibility permission is not granted)")]
    AccessibilityNotTrusted,
}

pub type Result<T> = std::result::Result<T, Error>;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct ShimRect {
    x: f64,
    y: f64,
    width: f64,
    height: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct ShimPoint {
    x: f64,
    y: f64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DockOrientation {
    Bottom = 0,
    Left = 1,
    Right = 2,
}

#[derive(Clone, Copy, Debug, Default)]
struct SafeSegment {
    start: f64,
    end: f64,
    width: f64,
    center: f64,
}

#[derive(Clone, Copy, Debug)]
struct DisplayBounds {
    display_id: DisplayId,
    bounds: ShimRect,
}

#[derive(Debug, Default)]
struct DisplayNameCache {
    names: HashMap<DisplayId, String>,
    last_refresh_attempt: Option<Instant>,
    needs_refresh: bool,
}

static LOCK_TARGET: AtomicU32 = AtomicU32::new(0);
static DISPLAY_CACHE: OnceLock<Mutex<Vec<DisplayBounds>>> = OnceLock::new();
static DISPLAY_NAME_CACHE: OnceLock<Mutex<DisplayNameCache>> = OnceLock::new();

extern "C" {
    fn lockdock_shim_get_active_displays(displays: *mut c_uint, max_displays: c_uint) -> c_uint;
    fn lockdock_shim_copy_display_bounds(display_id: c_uint, rect_out: *mut ShimRect) -> bool;
    fn lockdock_shim_copy_display_uuid(
        display_id: c_uint,
        buffer: *mut c_char,
        buffer_size: usize,
    ) -> bool;
    fn lockdock_shim_display_is_builtin(display_id: c_uint) -> bool;
    fn lockdock_shim_display_vendor_number(display_id: c_uint) -> c_uint;
    fn lockdock_shim_display_model_number(display_id: c_uint) -> c_uint;
    fn lockdock_shim_display_serial_number(display_id: c_uint) -> c_uint;
    fn lockdock_shim_is_accessibility_trusted() -> bool;
    fn lockdock_shim_copy_dock_window_bounds(rect_out: *mut ShimRect) -> bool;
    fn lockdock_shim_copy_accessibility_dock_window_bounds(rect_out: *mut ShimRect) -> bool;
    fn lockdock_shim_copy_dock_orientation(buffer: *mut c_char, buffer_size: usize) -> bool;
    fn lockdock_shim_copy_mouse_location(point_out: *mut ShimPoint) -> bool;
    fn lockdock_shim_event_source_create() -> *mut c_void;
    fn lockdock_shim_event_source_set_suppression_interval(source: *mut c_void, interval: f64);
    fn lockdock_shim_release(object: *mut c_void);
    fn lockdock_shim_set_cursor_association(associated: bool) -> bool;
    fn lockdock_shim_warp_mouse(point: ShimPoint);
    fn lockdock_shim_post_mouse_moved(source: *mut c_void, point: ShimPoint);
    fn lockdock_shim_post_edge_nudge(source: *mut c_void, point: ShimPoint, orientation: c_int);
    fn lockdock_shim_start_event_tap(error: *mut c_char, error_size: usize) -> bool;
    fn lockdock_shim_stop_event_tap();
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
pub fn display_label(display_id: DisplayId) -> String {
    if unsafe { lockdock_shim_display_is_builtin(display_id) } {
        return "Built-in Display".to_owned();
    }

    display_name(display_id).unwrap_or_else(|| format!("Display-{display_id}"))
}

pub fn display_identity(display_id: DisplayId) -> Result<DisplayIdentity> {
    let mut uuid = [0 as c_char; DISPLAY_UUID_BUFFER_SIZE];
    unsafe {
        lockdock_shim_copy_display_uuid(display_id, uuid.as_mut_ptr(), uuid.len());
    }

    let identity = DisplayIdentity {
        is_builtin: unsafe { lockdock_shim_display_is_builtin(display_id) },
        vendor_number: unsafe { lockdock_shim_display_vendor_number(display_id) },
        model_number: unsafe { lockdock_shim_display_model_number(display_id) },
        serial_number: unsafe { lockdock_shim_display_serial_number(display_id) },
        uuid: c_string(&uuid),
    };

    if display_identity_is_valid(&identity) {
        Ok(identity)
    } else {
        Err(Error::MissingIdentity)
    }
}

#[must_use]
pub fn find_active_display_by_identity(identity: &DisplayIdentity) -> Option<DisplayId> {
    if !display_identity_is_valid(identity) {
        return None;
    }

    let mut fallback = None;
    for display_id in active_displays() {
        let Ok(current) = display_identity(display_id) else {
            continue;
        };

        if !identity.uuid.is_empty() && !current.uuid.is_empty() && current.uuid == identity.uuid {
            return Some(display_id);
        }

        if fallback.is_none() && display_identity_fallback_matches(&current, identity) {
            fallback = Some(display_id);
        }
    }

    fallback
}

#[must_use]
pub fn find_display_index(display_id: DisplayId) -> Option<usize> {
    active_displays()
        .iter()
        .position(|display| *display == display_id)
}

pub fn relocate_display(display_id: DisplayId) -> Result<()> {
    validate_display(display_id)?;

    let bounds = display_bounds(display_id)?;
    let orientation = dock_orientation();
    let old_position = current_mouse_location();
    let source = EventSource::new();
    let cursor_locked = unsafe { lockdock_shim_set_cursor_association(false) };
    let safe_segment = find_safe_edge_segment(display_id, orientation);
    let edge_offset = 1.0;

    let (approach, edge) = match orientation {
        DockOrientation::Bottom => {
            let edge_y = bounds.y + bounds.height;
            let trigger_x = choose_trigger_coordinate(display_id, orientation, safe_segment);
            (
                ShimPoint {
                    x: trigger_x,
                    y: edge_y - edge_offset,
                },
                ShimPoint {
                    x: trigger_x,
                    y: edge_y - 1.0,
                },
            )
        }
        DockOrientation::Left => {
            let edge_x = bounds.x;
            let trigger_y = choose_trigger_coordinate(display_id, orientation, safe_segment);
            (
                ShimPoint {
                    x: edge_x + edge_offset,
                    y: trigger_y,
                },
                ShimPoint {
                    x: edge_x + 1.0,
                    y: trigger_y,
                },
            )
        }
        DockOrientation::Right => {
            let edge_x = bounds.x + bounds.width;
            let trigger_y = choose_trigger_coordinate(display_id, orientation, safe_segment);
            (
                ShimPoint {
                    x: edge_x - edge_offset,
                    y: trigger_y,
                },
                ShimPoint {
                    x: edge_x - 1.0,
                    y: trigger_y,
                },
            )
        }
    };

    if let Some(source) = source.as_ref() {
        unsafe { lockdock_shim_event_source_set_suppression_interval(source.raw, 0.0) };
    }

    smooth_move(
        source.as_ref(),
        old_position,
        approach,
        RELOCATION_EDGE_MOVE_STEPS,
    );
    thread::sleep(RELOCATION_APPROACH_DELAY);
    smooth_move(source.as_ref(), approach, edge, RELOCATION_EDGE_MOVE_STEPS);

    let relocated = wait_for_dock_relocation(source.as_ref(), edge, orientation, display_id);

    smooth_move(
        source.as_ref(),
        edge,
        old_position,
        RELOCATION_EDGE_MOVE_STEPS,
    );
    if cursor_locked {
        unsafe { lockdock_shim_set_cursor_association(true) };
    }

    if relocated {
        Ok(())
    } else {
        Err(Error::Native(format!(
            "Could not move Dock to display {display_id}"
        )))
    }
}

pub fn set_lock_target(display_id: DisplayId) -> Result<()> {
    let mut error = ErrorBuffer::new();
    if unsafe { lockdock_shim_start_event_tap(error.as_mut_ptr(), error.len()) } {
        refresh_lock_display_cache();
        LOCK_TARGET.store(display_id, Ordering::SeqCst);
        Ok(())
    } else {
        Err(Error::Native(error.to_string()))
    }
}

pub fn refresh_lock_display_cache() {
    let cache = DISPLAY_CACHE.get_or_init(|| Mutex::new(Vec::new()));
    let mut next = Vec::new();

    for display_id in active_displays() {
        if let Ok(bounds) = display_bounds(display_id) {
            next.push(DisplayBounds { display_id, bounds });
        }
    }

    if let Ok(mut cache) = cache.lock() {
        *cache = next;
    }
}

pub fn clear_lock_target() {
    LOCK_TARGET.store(0, Ordering::SeqCst);
    unsafe { lockdock_shim_stop_event_tap() };
}

#[must_use]
pub fn lock_target() -> Option<DisplayId> {
    match LOCK_TARGET.load(Ordering::SeqCst) {
        0 => None,
        display_id => Some(display_id),
    }
}

pub fn shutdown() {
    clear_lock_target();
}

#[no_mangle]
pub extern "C" fn lockdock_display_should_suppress_event(
    event_kind: c_int,
    x: f64,
    y: f64,
) -> bool {
    if event_kind == 0 {
        return false;
    }

    let locked_display = LOCK_TARGET.load(Ordering::SeqCst);
    if locked_display == 0 {
        return false;
    }

    let point = ShimPoint { x, y };
    let Some(current) = cached_display_at_point(point) else {
        return false;
    };

    if current.display_id == locked_display {
        return false;
    }

    let distance = distance_from_dock_edge(point, current.bounds, dock_orientation());
    (0.0..=LOCK_EDGE_ZONE).contains(&distance)
}

fn active_displays() -> Vec<DisplayId> {
    let mut displays = [0; MAX_DISPLAYS];
    let count = unsafe { lockdock_shim_get_active_displays(displays.as_mut_ptr(), MAX_DISPLAYS_C) }
        as usize;

    displays[..count.min(MAX_DISPLAYS)].to_vec()
}

fn display_bounds(display_id: DisplayId) -> Result<ShimRect> {
    let mut bounds = ShimRect::default();
    if unsafe { lockdock_shim_copy_display_bounds(display_id, &raw mut bounds) } {
        Ok(bounds)
    } else {
        Err(Error::DisplayNotFound(display_id))
    }
}

fn display_identity_is_valid(identity: &DisplayIdentity) -> bool {
    !identity.uuid.is_empty()
        || identity.is_builtin
        || identity.vendor_number != 0
        || identity.model_number != 0
        || identity.serial_number != 0
}

fn display_identity_fallback_matches(left: &DisplayIdentity, right: &DisplayIdentity) -> bool {
    left.is_builtin == right.is_builtin
        && left.vendor_number == right.vendor_number
        && left.model_number == right.model_number
        && left.serial_number == right.serial_number
}

fn validate_display(display_id: DisplayId) -> Result<()> {
    display_bounds(display_id).map(|_| ())
}

fn display_name(display_id: DisplayId) -> Option<String> {
    let cache = DISPLAY_NAME_CACHE.get_or_init(|| {
        Mutex::new(DisplayNameCache {
            names: HashMap::new(),
            last_refresh_attempt: None,
            needs_refresh: true,
        })
    });
    let mut cache = cache.lock().expect("display name cache mutex poisoned");

    if let Some(name) = cache.names.get(&display_id) {
        return Some(name.clone());
    }

    let should_refresh = cache.needs_refresh
        || cache
            .last_refresh_attempt
            .is_none_or(|last| last.elapsed() >= SYSTEM_PROFILER_CACHE_TTL);
    if !should_refresh {
        return None;
    }

    cache.last_refresh_attempt = Some(Instant::now());
    cache.needs_refresh = false;
    if let Some(names) = read_system_profiler_display_names() {
        cache.names = names;
    }

    cache.names.get(&display_id).cloned()
}

fn read_system_profiler_display_names() -> Option<HashMap<DisplayId, String>> {
    let output = Command::new("/usr/sbin/system_profiler")
        .args(["-json", "SPDisplaysDataType"])
        .output()
        .ok()?;

    if !output.status.success() {
        return None;
    }

    let root: Value = serde_json::from_slice(&output.stdout).ok()?;
    let mut names = HashMap::new();
    collect_display_names(&root, &mut names);
    (!names.is_empty()).then_some(names)
}

fn collect_display_names(value: &Value, names: &mut HashMap<DisplayId, String>) {
    match value {
        Value::Object(object) => {
            let name = object
                .get("_name")
                .and_then(Value::as_str)
                .filter(|name| !name.is_empty());
            let display_id = object
                .get("_spdisplays_displayID")
                .or_else(|| object.get("_spdisplays_CGSDID"))
                .and_then(parse_display_id_value);

            if let (Some(name), Some(display_id)) = (name, display_id) {
                names.insert(display_id, name.to_owned());
            }

            for child in object.values() {
                collect_display_names(child, names);
            }
        }
        Value::Array(array) => {
            for child in array {
                collect_display_names(child, names);
            }
        }
        _ => {}
    }
}

fn parse_display_id_value(value: &Value) -> Option<DisplayId> {
    if let Some(text) = value.as_str() {
        return text.parse().ok().filter(|id| *id != 0);
    }

    value
        .as_u64()
        .and_then(|id| DisplayId::try_from(id).ok())
        .filter(|id| *id != 0)
}

fn dock_display() -> Result<DisplayId> {
    let Some(display_id) = capture_dock_display() else {
        return if unsafe { lockdock_shim_is_accessibility_trusted() } {
            Err(Error::MissingDockDisplay)
        } else {
            Err(Error::AccessibilityNotTrusted)
        };
    };

    Ok(display_id)
}

fn capture_dock_display() -> Option<DisplayId> {
    let mut bounds = ShimRect::default();

    if unsafe { lockdock_shim_copy_dock_window_bounds(&raw mut bounds) } {
        if let Some(display_id) = display_for_rect(bounds) {
            return Some(display_id);
        }
    }

    if unsafe { lockdock_shim_copy_accessibility_dock_window_bounds(&raw mut bounds) } {
        return display_for_rect(bounds);
    }

    None
}

fn display_for_rect(rect: ShimRect) -> Option<DisplayId> {
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
        display_at_point(ShimPoint {
            x: rect.x + rect.width / 2.0,
            y: rect.y + rect.height / 2.0,
        })
    })
}

fn display_at_point(point: ShimPoint) -> Option<DisplayId> {
    active_displays().into_iter().find(|display_id| {
        display_bounds(*display_id)
            .map(|bounds| rect_contains_point(bounds, point))
            .unwrap_or(false)
    })
}

fn cached_display_at_point(point: ShimPoint) -> Option<DisplayBounds> {
    let cache = DISPLAY_CACHE.get_or_init(|| Mutex::new(Vec::new()));
    cache
        .lock()
        .expect("display cache mutex poisoned")
        .iter()
        .copied()
        .find(|display| rect_contains_point(display.bounds, point))
}

fn dock_orientation() -> DockOrientation {
    let mut buffer = [0 as c_char; DOCK_ORIENTATION_BUFFER_SIZE];
    let copied = unsafe { lockdock_shim_copy_dock_orientation(buffer.as_mut_ptr(), buffer.len()) };
    if !copied {
        return DockOrientation::Bottom;
    }

    match c_string(&buffer).as_str() {
        "left" => DockOrientation::Left,
        "right" => DockOrientation::Right,
        _ => DockOrientation::Bottom,
    }
}

fn find_safe_edge_segment(target_id: DisplayId, edge: DockOrientation) -> SafeSegment {
    let Ok(target) = display_bounds(target_id) else {
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
    let Ok(target) = display_bounds(target_id) else {
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

    for display_id in active_displays() {
        if display_id == target_id {
            continue;
        }
        let Ok(other) = display_bounds(display_id) else {
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

fn edge_geometry(bounds: ShimRect, edge: DockOrientation) -> (f64, f64, f64) {
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
    let Ok(bounds) = display_bounds(target_display_id) else {
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
    edge: ShimPoint,
    orientation: DockOrientation,
    display_id: DisplayId,
) -> bool {
    for attempt in 0..RELOCATION_NUDGE_ATTEMPTS {
        if let Some(source) = source {
            unsafe { lockdock_shim_post_edge_nudge(source.raw, edge, orientation as c_int) };
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
    let mut bounds = ShimRect::default();
    (unsafe { lockdock_shim_copy_dock_window_bounds(&raw mut bounds) })
        && display_for_rect(bounds) == Some(display_id)
}

fn current_mouse_location() -> ShimPoint {
    let mut point = ShimPoint::default();
    if unsafe { lockdock_shim_copy_mouse_location(&raw mut point) } {
        point
    } else {
        ShimPoint::default()
    }
}

fn smooth_move(source: Option<&EventSource>, from: ShimPoint, to: ShimPoint, steps: usize) {
    for step in 1..=steps {
        let step = u32::try_from(step).unwrap_or(u32::MAX);
        let steps = u32::try_from(steps).unwrap_or(u32::MAX);
        let progress = f64::from(step) / f64::from(steps);
        let point = ShimPoint {
            x: from.x + (to.x - from.x) * progress,
            y: from.y + (to.y - from.y) * progress,
        };
        unsafe { lockdock_shim_warp_mouse(point) };
        if let Some(source) = source {
            unsafe { lockdock_shim_post_mouse_moved(source.raw, point) };
        }
        thread::sleep(RELOCATION_EDGE_MOVE_DELAY);
    }
}

fn distance_from_dock_edge(
    point: ShimPoint,
    bounds: ShimRect,
    orientation: DockOrientation,
) -> f64 {
    match orientation {
        DockOrientation::Left => point.x - bounds.x,
        DockOrientation::Right => (bounds.x + bounds.width) - point.x,
        DockOrientation::Bottom => (bounds.y + bounds.height) - point.y,
    }
}

fn rect_contains_point(rect: ShimRect, point: ShimPoint) -> bool {
    point.x >= rect.x
        && point.x < rect.x + rect.width
        && point.y >= rect.y
        && point.y < rect.y + rect.height
}

fn rect_intersection(left: ShimRect, right: ShimRect) -> ShimRect {
    let x1 = left.x.max(right.x);
    let y1 = left.y.max(right.y);
    let x2 = (left.x + left.width).min(right.x + right.width);
    let y2 = (left.y + left.height).min(right.y + right.height);

    if x2 <= x1 || y2 <= y1 {
        return ShimRect::default();
    }

    ShimRect {
        x: x1,
        y: y1,
        width: x2 - x1,
        height: y2 - y1,
    }
}

fn c_string(buffer: &[c_char]) -> String {
    unsafe { CStr::from_ptr(buffer.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}

struct EventSource {
    raw: *mut c_void,
}

impl EventSource {
    fn new() -> Option<Self> {
        let raw = unsafe { lockdock_shim_event_source_create() };
        (!raw.is_null()).then_some(Self { raw })
    }
}

impl Drop for EventSource {
    fn drop(&mut self) {
        unsafe { lockdock_shim_release(self.raw) };
    }
}

struct ErrorBuffer([c_char; ERROR_BUFFER_SIZE]);

impl ErrorBuffer {
    fn new() -> Self {
        Self([0; ERROR_BUFFER_SIZE])
    }

    fn as_mut_ptr(&mut self) -> *mut c_char {
        self.0.as_mut_ptr()
    }

    fn len(&self) -> usize {
        self.0.len()
    }
}

impl std::fmt::Display for ErrorBuffer {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let message = c_string(&self.0);
        if message.is_empty() {
            formatter.write_str("native display operation failed")
        } else {
            formatter.write_str(&message)
        }
    }
}
