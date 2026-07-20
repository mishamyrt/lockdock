use std::thread;
use std::time::Duration;

use lockdock_display::DisplayId;
use lockdock_ipc::{CommandResult, Request, Response};

use crate::display_lock::{clear_lock_target, lock_target, refresh_dock_support, set_lock_target};
use crate::display_state::{
    build_state, display_identity, find_active_display_by_identity, find_display_index,
    query_display_status, DisplaySnapshot,
};
use crate::preferences::DisplayPreferences;
use crate::relocation::relocate_display;
use crate::{Error, Result};

const RELOCATION_RETRY_ATTEMPTS: usize = 3;
const RELOCATION_RETRY_DELAY: Duration = Duration::from_millis(500);
const RELOCATION_SETTLE_DELAY: Duration = Duration::from_millis(250);

pub(crate) fn handle_request(
    request: &Request,
    preferences: &DisplayPreferences,
    snapshot: &mut DisplaySnapshot,
) -> Response {
    match request {
        Request::GetState => Response::State(build_state(snapshot)),
        Request::SetState { target } => match apply_set_state(*target, preferences, snapshot) {
            Ok(()) => succeeded(),
            Err(error) => failed(&error),
        },
        Request::Unlock => match apply_unlock(preferences) {
            Ok(()) => succeeded(),
            Err(error) => failed(&error),
        },
    }
}

fn apply_set_state(
    target_index: usize,
    preferences: &DisplayPreferences,
    snapshot: &mut DisplaySnapshot,
) -> Result<()> {
    let display_id =
        *snapshot.status.displays.get(target_index).ok_or_else(|| {
            Error::Operation(format!("Display index {target_index} is out of range"))
        })?;

    if !dock_is_supported() {
        return Err(Error::Operation(
            "Dock orientation is not supported; only bottom placement can be locked".to_owned(),
        ));
    }

    let identity = display_identity(display_id, &snapshot.info)?;

    if snapshot.status.displays[snapshot.status.location_index] != display_id {
        clear_lock_target();
        relocate_display_until_current(display_id)?;
    }

    set_lock_target(display_id)?;
    preferences.save(&identity)?;
    snapshot.refresh_status()?;
    Ok(())
}

fn apply_unlock(preferences: &DisplayPreferences) -> Result<()> {
    preferences.clear()?;
    clear_lock_target();
    Ok(())
}

pub(crate) fn reconcile_display_state(
    preferences: &DisplayPreferences,
    snapshot: &DisplaySnapshot,
) -> Result<()> {
    if !dock_is_supported() {
        disable_lock(preferences)?;
        return Ok(());
    }

    if let Some(locked_display) = lock_target() {
        if find_display_index(locked_display, &snapshot.status).is_none() {
            clear_lock_target();
            return Ok(());
        }

        let status = query_display_status()?;
        if status.displays[status.location_index] == locked_display {
            return Ok(());
        }

        return relocate_display_until_current(locked_display);
    }

    let Some(identity) = preferences.load()? else {
        return Ok(());
    };
    let Some(preferred_display) = find_active_display_by_identity(&identity, snapshot) else {
        return Ok(());
    };

    let status = query_display_status()?;
    if status.displays[status.location_index] != preferred_display {
        relocate_display_until_current(preferred_display)?;
    }
    set_lock_target(preferred_display)?;
    Ok(())
}

/// Reports whether the Dock is away from where the lock (or the saved
/// preference, when the lock is not yet restored) wants it.
pub(crate) fn dock_needs_healing(
    preferences: &DisplayPreferences,
    snapshot: &DisplaySnapshot,
) -> bool {
    match lock_target() {
        Some(target) => lockdock_display::dock_window_display()
            .is_some_and(|current| current != target),
        None => preferred_active_display(preferences, snapshot).is_some(),
    }
}

fn preferred_active_display(
    preferences: &DisplayPreferences,
    snapshot: &DisplaySnapshot,
) -> Option<DisplayId> {
    let identity = preferences.load().ok().flatten()?;
    find_active_display_by_identity(&identity, snapshot)
}

fn disable_lock(preferences: &DisplayPreferences) -> Result<()> {
    preferences.clear()?;
    clear_lock_target();
    Ok(())
}

fn dock_is_supported() -> bool {
    refresh_dock_support()
}

fn relocate_display_until_current(display_id: DisplayId) -> Result<()> {
    let mut last_error = None;

    for attempt in 0..RELOCATION_RETRY_ATTEMPTS {
        if attempt > 0 {
            thread::sleep(RELOCATION_RETRY_DELAY);
        }

        match relocate_display(display_id) {
            Ok(()) => {}
            // The user grabbed the cursor or an overlay covers the display;
            // stop fighting and let the poll-driven reconcile retry later.
            Err(error @ Error::RelocationInterrupted(_)) => return Err(error),
            Err(error) => {
                last_error = Some(error.to_string());
                continue;
            }
        }

        thread::sleep(RELOCATION_SETTLE_DELAY);

        match query_display_status() {
            Ok(status) if status.displays[status.location_index] == display_id => return Ok(()),
            Ok(status) => {
                let current = status.displays[status.location_index];
                last_error = Some(format!(
                    "Dock is on display {current} (expected {display_id})"
                ));
            }
            Err(error) => last_error = Some(error.to_string()),
        }
    }

    Err(Error::Operation(last_error.unwrap_or_else(|| {
        format!("Dock did not move to display {display_id}")
    })))
}

fn succeeded() -> Response {
    Response::Result(CommandResult {
        success: true,
        reason: None,
    })
}

fn failed(error: &Error) -> Response {
    Response::Result(CommandResult {
        success: false,
        reason: Some(error.to_string()),
    })
}
