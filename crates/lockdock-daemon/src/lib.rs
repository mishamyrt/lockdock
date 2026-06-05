mod controller;
mod daemon;
mod display_lock;
mod display_state;
mod logging;
mod preferences;
mod relocation;

use std::path::PathBuf;

pub use daemon::run;

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
