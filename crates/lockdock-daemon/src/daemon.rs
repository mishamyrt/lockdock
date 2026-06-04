use std::fs;
use std::path::PathBuf;
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use lockdock_ipc::{CommandResult, Incoming, Response, Server};

use crate::controller::{handle_request, reconcile_display_state};
use crate::display_lock::{refresh_display_cache, refresh_dock_support, shutdown};
use crate::display_state::DisplaySnapshot;
use crate::preferences::DisplayPreferences;
use crate::{Config, Result};

const DISPLAY_POLL_INTERVAL: Duration = Duration::from_secs(2);

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
        if !refresh_dock_support() {
            if let Err(error) = reconcile_display_state(preferences, snapshot) {
                eprintln!("Display reconcile failed: {error}");
            }
        }
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

fn write_pid_file(pid_path: &PathBuf) -> Result<()> {
    if let Some(parent) = pid_path.parent() {
        fs::create_dir_all(parent)?;
    }

    fs::write(pid_path, format!("{}\n", std::process::id()))?;
    Ok(())
}
