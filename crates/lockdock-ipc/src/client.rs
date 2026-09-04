use std::{io::Write as _, net::Shutdown, os::unix::net::UnixStream, path::PathBuf};

use crate::{
    parse_response, serialize_request,
    transport::{read_message, validate_socket_path},
    CommandResult, Error, Request, Response, Result, State,
};

pub struct Client {
    socket_path: PathBuf,
}

impl Client {
    #[must_use]
    pub fn new(socket_path: impl Into<PathBuf>) -> Self {
        Self { socket_path: socket_path.into() }
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

fn result_error(result: CommandResult) -> Error {
    if let Some(reason) = result.reason.filter(|reason| !reason.is_empty()) {
        Error::Daemon(reason)
    } else {
        Error::Daemon("daemon request failed".to_owned())
    }
}
