mod display_lock;
mod relocation;

use display_lock::{
    clear_lock_target, lock_target, refresh_display_cache, set_lock_target, shutdown,
};
use lockdock_display::{DisplayId, DisplayIdentity, DisplayInfo, Status as DisplayStatus};
use lockdock_ipc::{CommandResult, Incoming, Request, Response, Server, State};
use prefs::{Key, Preferences};
use relocation::relocate_display;
use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

const BUNDLE_ID: &str = "co.myrt.lockdock";
const DISPLAY_POLL_INTERVAL: Duration = Duration::from_secs(2);
const RELOCATION_RETRY_ATTEMPTS: usize = 5;
const RELOCATION_RETRY_DELAY: Duration = Duration::from_secs(1);

const PREFERRED_UUID: Key<String> = Key::new("preferredDisplayUUID");
const PREFERRED_BUILTIN: Key<bool> = Key::new("preferredDisplayBuiltin");
const PREFERRED_VENDOR: Key<i64> = Key::new("preferredDisplayVendor");
const PREFERRED_MODEL: Key<i64> = Key::new("preferredDisplayModel");
const PREFERRED_SERIAL: Key<i64> = Key::new("preferredDisplaySerial");

#[derive(Debug, Clone)]
pub struct Config {
    pub socket_path: PathBuf,
    pub pid_path: PathBuf,
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0}")]
    Operation(String),
    #[error(transparent)]
    Display(#[from] lockdock_display::Error),
    #[error(transparent)]
    Ipc(#[from] lockdock_ipc::Error),
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error(transparent)]
    Prefs(#[from] prefs::Error),
    #[error(transparent)]
    PrefsDomain(#[from] std::ffi::NulError),
}

pub type Result<T> = std::result::Result<T, Error>;

struct DisplaySnapshot {
    status: DisplayStatus,
    info: HashMap<DisplayId, DisplayInfo>,
}

impl DisplaySnapshot {
    fn load() -> Result<Self> {
        Ok(Self {
            status: lockdock_display::query_status()?,
            info: lockdock_display::load_display_info()?,
        })
    }

    fn refresh_status(&mut self) -> Result<()> {
        self.status = lockdock_display::query_status()?;
        Ok(())
    }

    fn refresh_info(&mut self) -> Result<()> {
        self.info = lockdock_display::load_display_info()?;
        Ok(())
    }
}

pub fn run(config: &Config) -> Result<()> {
    let listener = Server::bind(&config.socket_path)?;
    write_pid_file(&config.pid_path)?;
    let preferences = DisplayPreferences::new()?;
    let mut snapshot = DisplaySnapshot::load()?;
    let (sender, receiver) = mpsc::channel();

    if let Err(error) = reconcile_display_state(&preferences, &snapshot) {
        eprintln!("Display reconcile failed: {error}");
    } else if let Err(error) = snapshot.refresh_status() {
        eprintln!("Display status refresh failed: {error}");
    }

    println!(
        "lockdock daemon listening on {}",
        listener.socket_path().display()
    );

    thread::spawn(move || loop {
        match listener.accept_incoming() {
            Ok(incoming) => {
                if sender.send(incoming).is_err() {
                    break;
                }
            }
            Err(error) => eprintln!("IPC accept failed: {error}"),
        }
    });

    loop {
        match receiver.recv_timeout(DISPLAY_POLL_INTERVAL) {
            Ok(incoming) => handle_incoming(incoming, &preferences, &mut snapshot),
            Err(mpsc::RecvTimeoutError::Timeout) => {
                poll_display_changes(&preferences, &mut snapshot);
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }

    shutdown();
    let _ = fs::remove_file(&config.pid_path);
    Ok(())
}

fn poll_display_changes(preferences: &DisplayPreferences, snapshot: &mut DisplaySnapshot) {
    let displays = lockdock_display::active_displays();
    if displays == snapshot.status.displays {
        return;
    }

    refresh_display_cache();
    if let Err(error) = snapshot.refresh_info() {
        eprintln!("Display info refresh failed: {error}");
    }
    if let Err(error) = snapshot.refresh_status() {
        eprintln!("Display status refresh failed: {error}");
        return;
    }

    if let Err(error) = reconcile_display_state(preferences, snapshot) {
        eprintln!("Display reconcile failed: {error}");
    } else if let Err(error) = snapshot.refresh_status() {
        eprintln!("Display status refresh failed: {error}");
    }
}

fn handle_incoming(
    incoming: Incoming,
    preferences: &DisplayPreferences,
    snapshot: &mut DisplaySnapshot,
) {
    let response = match &incoming.request {
        Ok(request) => handle_request(request, preferences, snapshot),
        Err(error) => Response::Result(CommandResult {
            success: false,
            reason: Some(error.clone()),
        }),
    };

    if let Err(error) = incoming.respond(&response) {
        eprintln!("IPC response failed: {error}");
    }
}

fn handle_request(
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

fn build_state(snapshot: &DisplaySnapshot) -> State {
    let target = lock_target().and_then(|display_id| {
        snapshot
            .status
            .displays
            .iter()
            .position(|display| *display == display_id)
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

fn apply_set_state(
    target_index: usize,
    preferences: &DisplayPreferences,
    snapshot: &mut DisplaySnapshot,
) -> Result<()> {
    let display_id =
        *snapshot.status.displays.get(target_index).ok_or_else(|| {
            Error::Operation(format!("Display index {target_index} is out of range"))
        })?;
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

fn display_label(display_id: DisplayId, info: &HashMap<DisplayId, DisplayInfo>) -> String {
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

fn display_identity(
    display_id: DisplayId,
    info: &HashMap<DisplayId, DisplayInfo>,
) -> Result<DisplayIdentity> {
    let Some(info) = info.get(&display_id) else {
        return Err(lockdock_display::Error::MissingIdentity.into());
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
        Err(lockdock_display::Error::MissingIdentity.into())
    }
}

fn find_active_display_by_identity(
    identity: &DisplayIdentity,
    snapshot: &DisplaySnapshot,
) -> Option<DisplayId> {
    if !display_identity_is_valid(identity) {
        return None;
    }

    snapshot.status.displays.iter().copied().find(|display_id| {
        display_identity(*display_id, &snapshot.info)
            .map(|current| display_identity_fallback_matches(&current, identity))
            .unwrap_or(false)
    })
}

fn find_display_index(display_id: DisplayId, status: &DisplayStatus) -> Option<usize> {
    status
        .displays
        .iter()
        .position(|display| *display == display_id)
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

fn reconcile_display_state(
    preferences: &DisplayPreferences,
    snapshot: &DisplaySnapshot,
) -> Result<()> {
    if let Some(locked_display) = lock_target() {
        if find_display_index(locked_display, &snapshot.status).is_none() {
            clear_lock_target();
            return Ok(());
        }

        let status = lockdock_display::query_status()?;
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

fn relocate_display_until_current(display_id: DisplayId) -> Result<()> {
    let mut last_error = None;

    for _ in 0..RELOCATION_RETRY_ATTEMPTS {
        relocate_display(display_id)?;

        thread::sleep(RELOCATION_RETRY_DELAY);

        match lockdock_display::query_status() {
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

fn write_pid_file(pid_path: &PathBuf) -> Result<()> {
    if let Some(parent) = pid_path.parent() {
        fs::create_dir_all(parent)?;
    }

    fs::write(pid_path, format!("{}\n", std::process::id()))?;
    Ok(())
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

struct DisplayPreferences {
    preferences: Preferences,
}

impl DisplayPreferences {
    fn new() -> Result<Self> {
        Ok(Self {
            preferences: Preferences::new(BUNDLE_ID)?,
        })
    }

    fn save(&self, identity: &DisplayIdentity) -> Result<()> {
        self.preferences.set(PREFERRED_UUID, &identity.uuid)?;
        self.preferences
            .set(PREFERRED_BUILTIN, &identity.is_builtin)?;
        self.preferences
            .set(PREFERRED_VENDOR, &i64::from(identity.vendor_number))?;
        self.preferences
            .set(PREFERRED_MODEL, &i64::from(identity.model_number))?;
        self.preferences
            .set(PREFERRED_SERIAL, &i64::from(identity.serial_number))?;
        Ok(())
    }

    fn load(&self) -> Result<Option<DisplayIdentity>> {
        let uuid = self.preferences.get(PREFERRED_UUID)?.unwrap_or_default();
        let is_builtin = self.preferences.get(PREFERRED_BUILTIN)?.unwrap_or(false);
        let vendor_number = self.preferences.get(PREFERRED_VENDOR)?.unwrap_or(0);
        let model_number = self.preferences.get(PREFERRED_MODEL)?.unwrap_or(0);
        let serial_number = self.preferences.get(PREFERRED_SERIAL)?.unwrap_or(0);

        let identity = DisplayIdentity {
            is_builtin,
            vendor_number: u32::try_from(vendor_number).unwrap_or(0),
            model_number: u32::try_from(model_number).unwrap_or(0),
            serial_number: u32::try_from(serial_number).unwrap_or(0),
            uuid,
        };

        if identity.uuid.is_empty()
            && !identity.is_builtin
            && identity.vendor_number == 0
            && identity.model_number == 0
            && identity.serial_number == 0
        {
            Ok(None)
        } else {
            Ok(Some(identity))
        }
    }

    fn clear(&self) -> Result<()> {
        self.preferences.remove(PREFERRED_UUID)?;
        self.preferences.remove(PREFERRED_BUILTIN)?;
        self.preferences.remove(PREFERRED_VENDOR)?;
        self.preferences.remove(PREFERRED_MODEL)?;
        self.preferences.remove(PREFERRED_SERIAL)?;
        Ok(())
    }
}
