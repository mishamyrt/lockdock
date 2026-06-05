mod lockdock;

use clap::{Parser, Subcommand};
use std::path::PathBuf;
use std::process::ExitCode;

use crate::lockdock::{Lockdock, Output};

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
}

#[derive(Debug, Parser)]
#[command(name = "lockdock", version, about = "Dock position locker")]
struct Cli {
    /// Turn debugging information on
    #[arg(short, long)]
    pub verbose: bool,

    /// Cache directory override. Defaults to ~/Library/Caches/co.myrt.lockdock
    #[arg(long)]
    pub cache: Option<PathBuf>,

    #[command(subcommand)]
    command: Command,
}

#[allow(clippy::print_stderr, clippy::print_stdout)]
fn main() -> ExitCode {
    let cli = Cli::parse();
    let cache_dir = match cli.cache {
        Some(cache_dir) => cache_dir,
        None => match Lockdock::cache_dir() {
            Ok(cache_dir) => cache_dir,
            Err(e) => {
                eprintln!("Failed to get cache directory: {e}");
                return ExitCode::FAILURE;
            }
        },
    };

    let lockdock = Lockdock::new(cli.verbose, cache_dir);

    let result = match cli.command {
        Command::Run => lockdock.run(),
        Command::Enable => lockdock.enable_agent(),
        Command::Disable => lockdock.disable_agent(),
        Command::List => lockdock.list(),
        Command::Lock { index } => lockdock.lock(index),
        Command::Unlock => lockdock.unlock(),
        Command::Version => Lockdock::version(),
    };

    match result {
        Ok(output) => match output {
            Output::Message(output) => {
                println!("{output}");
            }
            Output::None => {}
        },
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::FAILURE;
        }
    }

    ExitCode::SUCCESS
}
