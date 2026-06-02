use lockdock_display::{DisplayId, DisplayIdentity};
use lockdock_ipc::{CommandResult, Incoming, Listener, Request, Response, State};
use prefs::{Key, Preferences};
use std::fs;
use std::path::PathBuf;
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

const BUNDLE_ID: &str = "co.myrt.lockdock";
const DISPLAY_POLL_INTERVAL: Duration = Duration::from_secs(3);
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

pub fn run(config: &Config) -> Result<()> {
    let listener = Listener::bind(&config.socket_path)?;
    write_pid_file(&config.pid_path)?;
    let preferences = DisplayPreferences::new()?;

    if let Err(error) = reconcile_display_state(&preferences) {
        eprintln!("Display reconcile failed: {error}");
    }

    println!(
        "lockdock daemon listening on {}",
        listener.socket_path().display()
    );

    let (sender, receiver) = mpsc::channel();
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
            Ok(incoming) => handle_incoming(incoming, &preferences),
            Err(mpsc::RecvTimeoutError::Timeout) => {
                if let Err(error) = reconcile_display_state(&preferences) {
                    eprintln!("Display reconcile failed: {error}");
                }
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }

    lockdock_display::shutdown();
    let _ = fs::remove_file(&config.pid_path);
    Ok(())
}

fn handle_incoming(incoming: Incoming, preferences: &DisplayPreferences) {
    let response = match &incoming.request {
        Ok(request) => handle_request(request, preferences),
        Err(error) => Response::Result(CommandResult {
            success: false,
            reason: Some(error.clone()),
        }),
    };

    if let Err(error) = incoming.respond(&response) {
        eprintln!("IPC response failed: {error}");
    }
}

fn handle_request(request: &Request, preferences: &DisplayPreferences) -> Response {
    match request {
        Request::GetState => match build_state() {
            Ok(state) => Response::State(state),
            Err(error) => failed(&error),
        },
        Request::SetState { target } => match apply_set_state(*target, preferences) {
            Ok(()) => succeeded(),
            Err(error) => failed(&error),
        },
        Request::Unlock => match apply_unlock(preferences) {
            Ok(()) => succeeded(),
            Err(error) => failed(&error),
        },
    }
}

fn build_state() -> Result<State> {
    let status = lockdock_display::query_status()?;
    let target = lockdock_display::lock_target().and_then(|display_id| {
        status
            .displays
            .iter()
            .position(|display| *display == display_id)
    });

    Ok(State {
        displays: status
            .displays
            .iter()
            .map(|display_id| lockdock_display::display_label(*display_id))
            .collect(),
        location: status.location_index,
        target,
    })
}

fn apply_set_state(target_index: usize, preferences: &DisplayPreferences) -> Result<()> {
    let status = lockdock_display::query_status()?;
    let display_id = *status
        .displays
        .get(target_index)
        .ok_or_else(|| Error::Operation(format!("Display index {target_index} is out of range")))?;
    let identity = lockdock_display::display_identity(display_id)?;

    if status.displays[status.location_index] != display_id {
        lockdock_display::clear_lock_target();
        relocate_display_until_current(display_id)?;
    }

    lockdock_display::set_lock_target(display_id)?;
    preferences.save(&identity)?;
    Ok(())
}

fn apply_unlock(preferences: &DisplayPreferences) -> Result<()> {
    preferences.clear()?;
    lockdock_display::clear_lock_target();
    Ok(())
}

fn reconcile_display_state(preferences: &DisplayPreferences) -> Result<()> {
    if let Some(locked_display) = lockdock_display::lock_target() {
        if lockdock_display::find_display_index(locked_display).is_none() {
            lockdock_display::clear_lock_target();
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
    let Some(preferred_display) = lockdock_display::find_active_display_by_identity(&identity)
    else {
        return Ok(());
    };

    relocate_display_until_current(preferred_display)?;
    lockdock_display::set_lock_target(preferred_display)?;
    Ok(())
}

fn relocate_display_until_current(display_id: DisplayId) -> Result<()> {
    let mut last_error = None;

    for _ in 0..RELOCATION_RETRY_ATTEMPTS {
        if let Err(error) = lockdock_display::relocate_display(display_id) {
            return Err(error.into());
        }

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
