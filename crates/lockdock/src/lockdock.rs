use std::{env, path::PathBuf};

use lockdock_ipc::Client;
use lunchd::{KeepAlive, LaunchAgent};
use std::fmt::Write as _;

#[derive(Debug, thiserror::Error)]

pub(crate) enum Error {
    #[error(transparent)]
    Daemon(#[from] lockdock_daemon::Error),
    #[error(transparent)]
    Ipc(#[from] lockdock_ipc::Error),
    #[error(transparent)]
    Lunchd(#[from] lunchd::AgentError),
    #[error("failed to build launch agent: {0}")]
    LaunchAgentBuild(String),
    #[error("service is not enabled")]
    NotEnabled,
    #[error("service is already enabled")]
    AlreadyEnabled,
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error("HOME environment variable not found")]
    HomeNotFound,
    #[error(transparent)]
    Format(#[from] std::fmt::Error),
}

pub(crate) enum Output {
    None,
    Message(String),
}

type Result<T> = std::result::Result<T, Error>;

pub(crate) const BUNDLE_ID: &str = "co.myrt.lockdock";

pub(crate) struct Lockdock {
    verbose: bool,
    cache_dir: PathBuf,
}

impl Lockdock {
    pub(crate) fn new<P: Into<PathBuf>>(verbose: bool, cache_dir: P) -> Self {
        Self {
            verbose,
            cache_dir: cache_dir.into(),
        }
    }

    pub(crate) fn cache_dir() -> Result<PathBuf> {
        let home = env::var_os("HOME").ok_or(Error::HomeNotFound)?;
        Ok(PathBuf::from(home)
            .join("Library")
            .join("Caches")
            .join(BUNDLE_ID))
    }

    #[allow(clippy::unnecessary_wraps)]
    pub(crate) fn version() -> Result<Output> {
        let mut version = "lockdock ".to_string();
        version.push_str(env!("CARGO_PKG_VERSION"));
        Ok(Output::Message(version))
    }

    pub(crate) fn run(&self) -> Result<Output> {
        let config = lockdock_daemon::Config {
            socket_path: self.socket_path(),
            pid_path: self.pid_path(),
            verbose: self.verbose,
        };
        lockdock_daemon::run(&config)?;

        Ok(Output::None)
    }

    #[allow(clippy::unused_self)]
    pub(crate) fn disable_agent(&self) -> Result<Output> {
        let agent = LaunchAgent::new(BUNDLE_ID);
        if !agent.exists() {
            return Err(Error::NotEnabled);
        }
        agent.uninstall()?;

        Ok(Output::Message("Service successfully disabled".into()))
    }

    pub(crate) fn enable_agent(&self) -> Result<Output> {
        let args = self.program_arguments()?;
        let agent = LaunchAgent::builder(BUNDLE_ID)
            .program_arguments(args)
            .keep_alive(KeepAlive::Crashed)
            .run_at_load(true)
            .build()
            .map_err(|error| Error::LaunchAgentBuild(error.to_string()))?;

        if agent.exists() {
            return Err(Error::AlreadyEnabled);
        }
        agent.install()?;

        Ok(Output::Message("Service successfully enabled".into()))
    }

    pub(crate) fn list(&self) -> Result<Output> {
        let state = self.client().get_state()?;
        let mut output = String::new();

        for (index, display) in state.displays.iter().enumerate() {
            let is_current = index == state.location;
            let is_locked = state.target == Some(index);

            write!(output, "{index} - {display}")?;
            if is_current && is_locked {
                write!(output, " [current, locked]")?;
            } else if is_current {
                write!(output, " [current]")?;
            } else if is_locked {
                write!(output, " [locked]")?;
            }
            output.push('\n');
        }

        Ok(Output::Message(output))
    }

    pub(crate) fn lock(&self, index: usize) -> Result<Output> {
        self.client().lock(index)?;

        Ok(Output::Message(format!("Dock locked to display {index}")))
    }

    pub(crate) fn unlock(&self) -> Result<Output> {
        self.client().unlock()?;

        Ok(Output::Message("Dock position unlocked".to_string()))
    }

    fn program_arguments(&self) -> Result<Vec<String>> {
        let program = env::current_exe()?;
        let mut args = vec![];
        args.push(program.to_string_lossy().into_owned());
        if self.verbose {
            args.push("-v".to_string());
        }
        args.push("run".to_string());

        Ok(args)
    }

    fn client(&self) -> Client {
        Client::new(self.socket_path())
    }

    fn pid_path(&self) -> PathBuf {
        self.cache_dir.join("daemon.pid")
    }

    fn socket_path(&self) -> PathBuf {
        self.cache_dir.join("control.sock")
    }
}
