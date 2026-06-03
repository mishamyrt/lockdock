use std::collections::HashMap;
use std::os::raw::c_uint;
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

use crate::error::{Error, Result};
use crate::ffi;
use crate::geometry::Rect;
use crate::system_profiler::{get_displays, DisplayInfo};
use crate::types::{DisplayId, DisplayIdentity};

const MAX_DISPLAYS: usize = 32;
const MAX_DISPLAYS_C: c_uint = 32;
const SYSTEM_PROFILER_CACHE_TTL: Duration = Duration::from_secs(5);

#[derive(Debug, Default)]
struct DisplayInfoCache {
    displays: HashMap<DisplayId, DisplayInfo>,
    last_refresh_attempt: Option<Instant>,
    needs_refresh: bool,
}

static DISPLAY_INFO_CACHE: OnceLock<Mutex<DisplayInfoCache>> = OnceLock::new();

#[must_use]
pub fn display_label(display_id: DisplayId) -> String {
    let Some(info) = display_info(display_id) else {
        return format!("Display-{display_id}");
    };

    if info.is_builtin {
        return "Built-in Display".to_owned();
    }

    if info.name.is_empty() {
        format!("Display-{display_id}")
    } else {
        info.name
    }
}

pub fn display_identity(display_id: DisplayId) -> Result<DisplayIdentity> {
    let Some(info) = display_info(display_id) else {
        return Err(Error::MissingIdentity);
    };

    let identity = DisplayIdentity {
        is_builtin: info.is_builtin,
        vendor_number: info.vendor_number,
        model_number: info.model_number,
        serial_number: info.serial_number,
        uuid: String::new(),
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

fn display_info(display_id: DisplayId) -> Option<DisplayInfo> {
    let cache = DISPLAY_INFO_CACHE.get_or_init(|| {
        Mutex::new(DisplayInfoCache {
            displays: HashMap::new(),
            last_refresh_attempt: None,
            needs_refresh: true,
        })
    });
    let mut cache = cache.lock().expect("display name cache mutex poisoned");

    if let Some(info) = cache.displays.get(&display_id) {
        return Some(info.clone());
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
    if let Ok(displays) = get_displays() {
        cache.displays = displays;
    }

    cache.displays.get(&display_id).cloned()
}
