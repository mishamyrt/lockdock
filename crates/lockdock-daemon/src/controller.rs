use std::thread;
use std::time::Duration;

use lockdock_display::{DisplayId, DockOrientation};
use lockdock_ipc::{CommandResult, Request, Response};

use crate::display_lock::{clear_lock_target, lock_target, set_lock_target};
use crate::display_state::{
    build_state, display_identity, find_active_display_by_identity, find_display_index,
    query_display_status, DisplaySnapshot,
};
use crate::preferences::DisplayPreferences;
use crate::relocation::relocate_display;
use crate::{Error, Result};

const RELOCATION_RETRY_ATTEMPTS: usize = 5;
const RELOCATION_RETRY_DELAY: Duration = Duration::from_secs(1);

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
        disable_lock(preferences)?;
        snapshot.refresh_status()?;
        return Ok(());
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

    relocate_display_until_current(preferred_display)?;
    set_lock_target(preferred_display)?;
    Ok(())
}

fn disable_lock(preferences: &DisplayPreferences) -> Result<()> {
    preferences.clear()?;
    clear_lock_target();
    Ok(())
}

fn dock_is_supported() -> bool {
    lockdock_display::dock_orientation() == DockOrientation::Bottom
}

fn relocate_display_until_current(display_id: DisplayId) -> Result<()> {
    let mut last_error = None;

    for _ in 0..RELOCATION_RETRY_ATTEMPTS {
        relocate_display(display_id)?;

        thread::sleep(RELOCATION_RETRY_DELAY);

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
