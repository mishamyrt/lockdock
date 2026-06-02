use serde::{Deserialize, Serialize};
use std::fs;
use std::io::{Read, Write};
use std::net::Shutdown;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};

pub const MAX_MESSAGE_SIZE: usize = 4096;
pub const MAX_DISPLAYS: usize = 32;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "cmd", rename_all = "snake_case")]
pub enum Request {
    GetState,
    SetState { target: usize },
    Unlock,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct State {
    pub displays: Vec<String>,
    pub location: usize,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub target: Option<usize>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CommandResult {
    pub success: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(untagged)]
pub enum Response {
    State(State),
    Result(CommandResult),
}

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

pub fn parse_request(json: &str) -> Result<Request> {
    Ok(serde_json::from_str(json)?)
}

pub fn serialize_request(request: &Request) -> Result<String> {
    serialize_message(request)
}

pub fn parse_response(json: &str) -> Result<Response> {
    Ok(serde_json::from_str(json)?)
}

pub fn serialize_response(response: &Response) -> Result<String> {
    if let Response::State(state) = response {
        validate_state(state)?;
    }

    serialize_message(response)
}

pub struct Client {
    socket_path: PathBuf,
}

impl Client {
    #[must_use]
    pub fn new(socket_path: impl Into<PathBuf>) -> Self {
        Self {
            socket_path: socket_path.into(),
        }
    }

    pub fn get_state(&self) -> Result<State> {
        match self.request(&Request::GetState)? {
            Response::State(state) => Ok(state),
            Response::Result(result) => Err(result_error(result)),
        }
    }

    pub fn lock(&self, target: usize) -> Result<()> {
        self.expect_success(&Request::SetState { target })
    }

    pub fn unlock(&self) -> Result<()> {
        self.expect_success(&Request::Unlock)
    }

    pub fn request(&self, request: &Request) -> Result<Response> {
        validate_socket_path(&self.socket_path)?;

        let request_json = serialize_request(request)?;
        let mut stream = UnixStream::connect(&self.socket_path)?;
        stream.write_all(request_json.as_bytes())?;
        stream.shutdown(Shutdown::Write)?;

        let response_json = read_message(&mut stream)?;
        parse_response(&response_json)
    }

    fn expect_success(&self, request: &Request) -> Result<()> {
        match self.request(request)? {
            Response::Result(result) if result.success => Ok(()),
            Response::Result(result) => Err(result_error(result)),
            Response::State(_) => Err(Error::UnexpectedResponse),
        }
    }
}

pub struct Listener {
    listener: UnixListener,
    socket_path: PathBuf,
}

impl Listener {
    pub fn bind(socket_path: impl Into<PathBuf>) -> Result<Self> {
        let socket_path = socket_path.into();
        validate_socket_path(&socket_path)?;

        if let Some(parent) = socket_path.parent() {
            fs::create_dir_all(parent)?;
        }

        if socket_path.exists() {
            fs::remove_file(&socket_path)?;
        }

        let listener = UnixListener::bind(&socket_path)?;
        Ok(Self {
            listener,
            socket_path,
        })
    }

    pub fn accept(&self) -> Result<Event> {
        let incoming = self.accept_incoming()?;
        let request = incoming.request.map_err(Error::Daemon)?;

        Ok(Event {
            request,
            stream: incoming.stream,
        })
    }

    pub fn accept_incoming(&self) -> Result<Incoming> {
        let (mut stream, _) = self.listener.accept()?;
        let message = read_message(&mut stream)?;
        let request = parse_request(&message).map_err(|error| error.to_string());

        Ok(Incoming { request, stream })
    }

    #[must_use]
    pub fn socket_path(&self) -> &Path {
        &self.socket_path
    }
}

impl Drop for Listener {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.socket_path);
    }
}

pub struct Incoming {
    pub request: std::result::Result<Request, String>,
    stream: UnixStream,
}

impl Incoming {
    pub fn respond(mut self, response: &Response) -> Result<()> {
        let response_json = serialize_response(response)?;
        self.stream.write_all(response_json.as_bytes())?;
        Ok(())
    }
}

pub struct Event {
    pub request: Request,
    stream: UnixStream,
}

impl Event {
    pub fn respond(mut self, response: &Response) -> Result<()> {
        let response_json = serialize_response(response)?;
        self.stream.write_all(response_json.as_bytes())?;
        Ok(())
    }
}

fn serialize_message<T: Serialize>(message: &T) -> Result<String> {
    let json = serde_json::to_string(message)?;
    if json.len() > MAX_MESSAGE_SIZE {
        return Err(Error::MessageTooLarge);
    }

    Ok(json)
}

fn validate_state(state: &State) -> Result<()> {
    if state.displays.len() > MAX_DISPLAYS {
        return Err(Error::TooManyDisplays);
    }

    Ok(())
}

fn validate_socket_path(path: &Path) -> Result<()> {
    if path.as_os_str().len() >= 104 {
        return Err(Error::SocketPathTooLong);
    }

    Ok(())
}

fn read_message(stream: &mut UnixStream) -> Result<String> {
    let mut buffer = Vec::with_capacity(MAX_MESSAGE_SIZE);
    stream
        .take((MAX_MESSAGE_SIZE + 1) as u64)
        .read_to_end(&mut buffer)?;
    if buffer.len() > MAX_MESSAGE_SIZE {
        return Err(Error::MessageTooLarge);
    }

    let text = String::from_utf8_lossy(&buffer).trim().to_owned();
    Ok(text)
}

fn result_error(result: CommandResult) -> Error {
    if let Some(reason) = result.reason.filter(|reason| !reason.is_empty()) {
        Error::Daemon(reason)
    } else {
        Error::Daemon("daemon request failed".to_owned())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serializes_get_state_request() {
        let json = serialize_request(&Request::GetState).unwrap();

        assert_eq!(json, r#"{"cmd":"get_state"}"#);
    }

    #[test]
    fn parses_set_state_request() {
        let request = parse_request(r#"{"cmd":"set_state","target":1}"#).unwrap();

        assert_eq!(request, Request::SetState { target: 1 });
    }

    #[test]
    fn serializes_state_without_target() {
        let response = Response::State(State {
            displays: vec!["Mi 27 NU".to_owned(), "Built-in Display".to_owned()],
            location: 1,
            target: None,
        });

        let json = serialize_response(&response).unwrap();

        assert_eq!(
            json,
            r#"{"displays":["Mi 27 NU","Built-in Display"],"location":1}"#
        );
    }

    #[test]
    fn parses_state_with_target() {
        let response = parse_response(
            r#"{"displays":["Mi 27 NU","Built-in Display"],"location":1,"target":1}"#,
        )
        .unwrap();

        assert_eq!(
            response,
            Response::State(State {
                displays: vec!["Mi 27 NU".to_owned(), "Built-in Display".to_owned()],
                location: 1,
                target: Some(1),
            })
        );
    }

    #[test]
    fn serializes_failed_result() {
        let response = Response::Result(CommandResult {
            success: false,
            reason: Some("error reason".to_owned()),
        });

        let json = serialize_response(&response).unwrap();

        assert_eq!(json, r#"{"success":false,"reason":"error reason"}"#);
    }
}
