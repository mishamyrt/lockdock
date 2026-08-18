use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

use lockdock_display::DisplayId;
use lockdock_ipc::{CommandResult, Incoming, Response, Server};

use crate::controller::{dock_needs_healing, handle_request, reconcile_display_state};
use crate::display_lock::{refresh_display_cache, refresh_dock_support, shutdown};
use crate::display_state::DisplaySnapshot;
use crate::preferences::DisplayPreferences;
use crate::{log_error, log_info, Config, Result};

const DISPLAY_POLL_INTERVAL: Duration = Duration::from_secs(2);
/// How long to wait after a failed reconcile before the poll loop tries to
/// move the Dock again, so retries do not keep grabbing the cursor.
const RECONCILE_RETRY_COOLDOWN: Duration = Duration::from_secs(15);

#[derive(Default)]
struct PollState {
    /// A changed display list is acted upon only after it is observed on two
    /// consecutive polls, so reconcile does not run while macOS is still
    /// rearranging displays.
    pending_displays: Option<Vec<DisplayId>>,
    reconcile_cooldown_until: Option<Instant>,
}

impl PollState {
    fn in_cooldown(&self) -> bool {
        self.reconcile_cooldown_until
            .is_some_and(|until| Instant::now() < until)
    }

    fn start_cooldown(&mut self) {
        self.reconcile_cooldown_until = Some(Instant::now() + RECONCILE_RETRY_COOLDOWN);
    }

    fn clear_cooldown(&mut self) {
        self.reconcile_cooldown_until = None;
    }
}

static SHUTDOWN_REQUESTED: AtomicBool = AtomicBool::new(false);

extern "C" fn handle_termination_signal(_signal: libc::c_int) {
    SHUTDOWN_REQUESTED.store(true, Ordering::SeqCst);
}

fn install_termination_handler() {
    let handler = handle_termination_signal as extern "C" fn(libc::c_int);
    unsafe {
        libc::signal(libc::SIGTERM, handler as libc::sighandler_t);
        libc::signal(libc::SIGINT, handler as libc::sighandler_t);
    }
}

pub fn run(config: &Config) -> Result<()> {
    crate::logging::set_verbose(config.verbose);
    install_termination_handler();
    let listener = Server::bind(&config.socket_path)?;
    write_pid_file(&config.pid_path)?;
    let preferences = DisplayPreferences::new()?;
    let mut snapshot = DisplaySnapshot::load()?;
    let (sender, receiver) = mpsc::channel();

    let mut poll_state = PollState::default();

    if let Err(error) = reconcile_display_state(&preferences, &snapshot) {
        log_error!("Display reconcile failed: {error}");
        poll_state.start_cooldown();
    } else if let Err(error) = snapshot.refresh_status() {
        log_error!("Display status refresh failed: {error}");
    }

    log_info!(
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
            Err(error) => log_error!("IPC accept failed: {error}"),
        }
    });

    while !SHUTDOWN_REQUESTED.load(Ordering::SeqCst) {
        match receiver.recv_timeout(DISPLAY_POLL_INTERVAL) {
            Ok(incoming) => handle_incoming(incoming, &preferences, &mut snapshot),
            Err(mpsc::RecvTimeoutError::Timeout) => {
                poll_display_changes(&preferences, &mut snapshot, &mut poll_state);
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }

    shutdown();
    let _ = fs::remove_file(&config.pid_path);
    Ok(())
}

fn poll_display_changes(
    preferences: &DisplayPreferences,
    snapshot: &mut DisplaySnapshot,
    state: &mut PollState,
) {
    let displays = lockdock_display::active_displays();
    if displays == snapshot.status.displays {
        state.pending_displays = None;
        // Bounds can change while the ID list stays the same (resolution
        // switches, rearrangement); keep the suppression zones in sync.
        refresh_display_cache();
        if !refresh_dock_support() {
            if let Err(error) = reconcile_display_state(preferences, snapshot) {
                log_error!("Display reconcile failed: {error}");
            }
            return;
        }
        heal_dock_position(preferences, snapshot, state);
        return;
    }

    if state.pending_displays.as_deref() != Some(displays.as_slice()) {
        state.pending_displays = Some(displays);
        return;
    }
    state.pending_displays = None;

    refresh_display_cache();
    if let Err(error) = snapshot.refresh_info() {
        log_error!("Display info refresh failed: {error}");
    }
    if let Err(error) = snapshot.refresh_status() {
        log_error!("Display status refresh failed: {error}");
        return;
    }

    if let Err(error) = reconcile_display_state(preferences, snapshot) {
        log_error!("Display reconcile failed: {error}");
        state.start_cooldown();
    } else {
        state.clear_cooldown();
        if let Err(error) = snapshot.refresh_status() {
            log_error!("Display status refresh failed: {error}");
        }
    }
}

/// Moves the Dock back when it drifted away from the locked display without a
/// display change (relocation given up earlier, Dock moved by macOS itself).
fn heal_dock_position(
    preferences: &DisplayPreferences,
    snapshot: &mut DisplaySnapshot,
    state: &mut PollState,
) {
    if state.in_cooldown() || !dock_needs_healing(preferences, snapshot) {
        return;
    }

    log_info!("Dock is away from the locked display; reconciling");
    if let Err(error) = reconcile_display_state(preferences, snapshot) {
        log_error!("Display reconcile failed: {error}");
        state.start_cooldown();
        return;
    }
    state.clear_cooldown();
    if let Err(error) = snapshot.refresh_status() {
        log_error!("Display status refresh failed: {error}");
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
        log_error!("IPC response failed: {error}");
    }
}

fn write_pid_file(pid_path: &PathBuf) -> Result<()> {
    if let Some(parent) = pid_path.parent() {
        fs::create_dir_all(parent)?;
    }

    fs::write(pid_path, format!("{}\n", std::process::id()))?;
    Ok(())
}
