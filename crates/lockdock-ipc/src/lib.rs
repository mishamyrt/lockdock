mod client;
mod protocol;
mod server;
mod transport;

pub use client::Client;
pub use protocol::{
    parse_request, parse_response, serialize_request, serialize_response, CommandResult, Request,
    Response, State, MAX_DISPLAYS,
};
pub use server::{Event, Incoming, Server};
pub use transport::MAX_MESSAGE_SIZE;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("socket path is too long for a Unix domain socket")]
    SocketPathTooLong,
    #[error("message exceeded {MAX_MESSAGE_SIZE} bytes")]
    MessageTooLarge,
    #[error("state contains too many displays")]
    TooManyDisplays,
    #[error("daemon returned an unexpected response")]
    UnexpectedResponse,
    #[error("{0}")]
    Daemon(String),
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error(transparent)]
    Json(#[from] serde_json::Error),
}

pub type Result<T> = std::result::Result<T, Error>;
