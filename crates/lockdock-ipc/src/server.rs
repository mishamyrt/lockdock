use std::{
    fs,
    io::Write as _,
    os::unix::net::{UnixListener, UnixStream},
    path::{Path, PathBuf},
    time::Duration,
};

use crate::{
    parse_request, serialize_response,
    transport::{read_message, validate_socket_path},
    Request, Response, Result,
};

const CLIENT_IO_TIMEOUT: Duration = Duration::from_secs(2);

pub struct Server {
    listener: UnixListener,
    socket_path: PathBuf,
}

impl Server {
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

    pub fn accept_incoming(&self) -> Result<Incoming> {
        let (mut stream, _) = self.listener.accept()?;
        stream.set_read_timeout(Some(CLIENT_IO_TIMEOUT))?;
        stream.set_write_timeout(Some(CLIENT_IO_TIMEOUT))?;
        let message = read_message(&mut stream)?;
        let request = parse_request(&message).map_err(|error| error.to_string());

        Ok(Incoming { request, stream })
    }

    #[must_use]
    pub fn socket_path(&self) -> &Path {
        &self.socket_path
    }
}

impl Drop for Server {
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
