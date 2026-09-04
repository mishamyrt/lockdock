use std::collections::HashMap;

use lockdock_display::{DisplayId, DisplayInfo};
use lockdock_ipc::State;

use crate::display_lock::lock_target;
use crate::{Error, Result};

pub(crate) struct DisplaySnapshot {
    pub(crate) status: DisplayStatus,
    pub(crate) info: HashMap<DisplayId, DisplayInfo>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct DisplayStatus {
    pub(crate) displays: Vec<DisplayId>,
    pub(crate) location_index: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct DisplayIdentity {
    pub(crate) is_builtin: bool,
    pub(crate) vendor_number: u32,
    pub(crate) model_number: u32,
    pub(crate) serial_number: u32,
}

impl DisplaySnapshot {
    pub(crate) fn load() -> Result<Self> {
        Ok(Self {
            status: query_display_status()?,
            info: lockdock_display::load_display_info()?,
        })
    }

    pub(crate) fn refresh_status(&mut self) -> Result<()> {
        self.status = query_display_status()?;
        Ok(())
    }

    pub(crate) fn refresh_info(&mut self) -> Result<()> {
        self.info = lockdock_display::load_display_info()?;
        Ok(())
    }
}

pub(crate) fn build_state(snapshot: &DisplaySnapshot) -> State {
    let target = lock_target().and_then(|display_id| {
        snapshot.status.displays.iter().position(|display| *display == display_id)
    });

    State {
        displays: snapshot
            .status
            .displays
            .iter()
            .map(|display_id| display_label(*display_id, &snapshot.info))
            .collect(),
        location: snapshot.status.location_index,
        target,
    }
}

fn display_label(
    display_id: DisplayId,
    info: &HashMap<DisplayId, DisplayInfo>,
) -> String {
    let Some(info) = info.get(&display_id) else {
        return format!("Display-{display_id}");
    };

    if info.is_builtin {
        return "Built-in Display".to_owned();
    }

    if info.name.is_empty() {
        format!("Display-{display_id}")
    } else {
        info.name.clone()
    }
}

pub(crate) fn display_identity(
    display_id: DisplayId,
    info: &HashMap<DisplayId, DisplayInfo>,
) -> Result<DisplayIdentity> {
    let Some(info) = info.get(&display_id) else {
        return Err(missing_identity());
    };

    let identity = DisplayIdentity {
        is_builtin: info.is_builtin,
        vendor_number: info.vendor_number,
        model_number: info.model_number,
        serial_number: info.serial_number,
    };

    if display_identity_is_valid(&identity) {
        Ok(identity)
    } else {
        Err(missing_identity())
    }
}

pub(crate) fn query_display_status() -> Result<DisplayStatus> {
    let displays = lockdock_display::active_displays();
    let dock_display = lockdock_display::dock_display()?;
    let location_index = displays
        .iter()
        .position(|display| *display == dock_display)
        .ok_or_else(|| Error::Operation("invalid display status".to_owned()))?;

    Ok(DisplayStatus { displays, location_index })
}

fn missing_identity() -> Error {
    Error::Operation("could not identify target display".to_owned())
}

pub(crate) fn find_active_display_by_identity(
    identity: &DisplayIdentity,
    snapshot: &DisplaySnapshot,
) -> Option<DisplayId> {
    if !display_identity_is_valid(identity) {
        return None;
    }

    snapshot.status.displays.iter().copied().find(|display_id| {
        display_identity(*display_id, &snapshot.info)
            .map(|current| current == *identity)
            .unwrap_or(false)
    })
}

pub(crate) fn find_display_index(
    display_id: DisplayId,
    status: &DisplayStatus,
) -> Option<usize> {
    status.displays.iter().position(|display| *display == display_id)
}

pub(crate) fn display_identity_is_valid(identity: &DisplayIdentity) -> bool {
    identity.is_builtin
        || identity.vendor_number != 0
        || identity.model_number != 0
        || identity.serial_number != 0
}
