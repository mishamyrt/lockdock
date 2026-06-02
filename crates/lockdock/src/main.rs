use clap::{CommandFactory, Parser, Subcommand};
use lockdock_ipc::Client;
use lunchd::{KeepAlive, LaunchAgent};
use std::env;
use std::path::PathBuf;

const BUNDLE_ID: &str = "co.myrt.lockdock";

#[derive(Debug, Parser)]
#[command(
    name = "lockdock",
    version,
    about = "Dock position locker",
    disable_help_subcommand = true
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    /// Run daemon in foreground.
    Run,
    /// Enable the background service.
    Enable,
    /// Disable the background service.
    Disable,
    /// List displays and Dock state.
    List,
    /// Lock the Dock to the display index.
    Lock { index: usize },
    /// Unlock the Dock.
    Unlock,
    /// Show version.
    Version,
    /// Show help.
    Help,
}

#[derive(Debug, thiserror::Error)]
enum Error {
    #[error("could not determine the current home directory")]
    HomeNotFound,
    #[error(transparent)]
    Daemon(#[from] lockdock_daemon::Error),
    #[error(transparent)]
    Ipc(#[from] lockdock_ipc::Error),
    #[error(transparent)]
    Lunchd(#[from] lunchd::AgentError),
    #[error("failed to build launch agent: {0}")]
    LaunchAgentBuild(String),
    #[error(transparent)]
    Io(#[from] std::io::Error),
}

type Result<T> = std::result::Result<T, Error>;

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let mut args = env::args_os();
    let program = args.next();
    let Some(command) = args.next() else {
        print_help();
        std::process::exit(1);
    };

    if let Some(command_text) = command.to_str() {
        if !matches!(
            command_text,
            "run" | "enable" | "disable" | "list" | "lock" | "unlock" | "version" | "help"
        ) {
            eprintln!("Unknown command: {command_text}");
            print_help();
            std::process::exit(1);
        }
    } else {
        eprintln!("Unknown command");
        print_help();
        std::process::exit(1);
    }

    let mut parse_args = Vec::from_iter(program);
    parse_args.push(command);
    parse_args.extend(args);

    let cli = match Cli::try_parse_from(parse_args) {
        Ok(cli) => cli,
        Err(error) => {
            eprint!("{error}");
            std::process::exit(1);
        }
    };

    match cli.command {
        Command::Run => run_daemon(),
        Command::Enable => enable(),
        Command::Disable => disable(),
        Command::List => list(),
        Command::Lock { index } => lock(index),
        Command::Unlock => unlock(),
        Command::Version => {
            println!("lockdock {}", env!("CARGO_PKG_VERSION"));
            Ok(())
        }
        Command::Help => {
            print_help();
            Ok(())
        }
    }
}

fn print_help() {
    let _ = Cli::command().print_help();
    println!();
}

fn enable() -> Result<()> {
    let program = env::current_exe()?;
    let agent = LaunchAgent::builder(BUNDLE_ID)
        .arg(program.to_string_lossy().into_owned())
        .arg("run")
        .keep_alive(KeepAlive::Crashed)
        .run_at_load(true)
        .build()
        .map_err(|error| Error::LaunchAgentBuild(error.to_string()))?;

    agent.install()?;
    println!("Enabled background service");
    Ok(())
}

fn run_daemon() -> Result<()> {
    Ok(lockdock_daemon::run(&lockdock_daemon::Config {
        socket_path: socket_path()?,
        pid_path: pid_path()?,
    })?)
}

fn disable() -> Result<()> {
    let agent = LaunchAgent::new(BUNDLE_ID);
    agent.uninstall()?;
    println!("Disabled background service");
    Ok(())
}

fn list() -> Result<()> {
    let state = client()?.get_state()?;

    for (index, display) in state.displays.iter().enumerate() {
        let is_current = index == state.location;
        let is_locked = state.target == Some(index);

        print!("{index} - {display}");
        if is_current && is_locked {
            print!(" [current, locked]");
        } else if is_current {
            print!(" [current]");
        } else if is_locked {
            print!(" [locked]");
        }
        println!();
    }

    Ok(())
}

fn lock(index: usize) -> Result<()> {
    client()?.lock(index)?;
    println!("Locked Dock to display {index}");
    Ok(())
}

fn unlock() -> Result<()> {
    client()?.unlock()?;
    println!("Unlocked Dock");
    Ok(())
}

fn client() -> Result<Client> {
    Ok(Client::new(socket_path()?))
}

fn socket_path() -> Result<PathBuf> {
    Ok(cache_dir()?.join("control.sock"))
}

#[allow(dead_code)]
fn pid_path() -> Result<PathBuf> {
    Ok(cache_dir()?.join("daemon.pid"))
}

fn cache_dir() -> Result<PathBuf> {
    let home = env::var_os("HOME").ok_or(Error::HomeNotFound)?;
    Ok(PathBuf::from(home)
        .join("Library")
        .join("Caches")
        .join(BUNDLE_ID))
}
