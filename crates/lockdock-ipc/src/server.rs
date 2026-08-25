use std::{
    ffi::OsString,
    fs::{self, File, OpenOptions},
    io::{self, Write as _},
    os::fd::AsRawFd as _,
    os::unix::fs::FileTypeExt as _,
    os::unix::net::{UnixListener, UnixStream},
    path::{Path, PathBuf},
    time::Duration,
};

use crate::{
    parse_request, serialize_response,
    transport::{read_message, validate_socket_path},
    Error, Request, Response, Result,
};

const CLIENT_IO_TIMEOUT: Duration = Duration::from_secs(2);

pub struct Server {
    listener: UnixListener,
    socket_path: PathBuf,
    _socket_lock: File,
}

impl Server {
    pub fn bind(socket_path: impl Into<PathBuf>) -> Result<Self> {
        let socket_path = socket_path.into();
        validate_socket_path(&socket_path)?;

        if let Some(parent) = socket_path.parent() {
            fs::create_dir_all(parent)?;
        }

        let socket_lock = lock_socket(&socket_path)?;
        remove_stale_socket(&socket_path)?;

        let listener = UnixListener::bind(&socket_path)?;
        Ok(Self {
            listener,
            socket_path,
            _socket_lock: socket_lock,
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

fn lock_socket(socket_path: &Path) -> Result<File> {
    let mut lock_path = OsString::from(socket_path.as_os_str());
    lock_path.push(".lock");
    let lock_file = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(PathBuf::from(lock_path))?;

    let status = unsafe { libc::flock(lock_file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
    if status == 0 {
        return Ok(lock_file);
    }

    let error = io::Error::last_os_error();
    if error.kind() == io::ErrorKind::WouldBlock {
        Err(Error::SocketInUse)
    } else {
        Err(error.into())
    }
}

fn remove_stale_socket(socket_path: &Path) -> Result<()> {
    let metadata = match fs::symlink_metadata(socket_path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error.into()),
    };

    if !metadata.file_type().is_socket() {
        return Err(Error::SocketPathOccupied);
    }

    match UnixStream::connect(socket_path) {
        Ok(_) => Err(Error::SocketInUse),
        Err(error) if error.kind() == io::ErrorKind::ConnectionRefused => {
            fs::remove_file(socket_path)?;
            Ok(())
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error.into()),
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

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicUsize, Ordering};

    use super::*;

    static NEXT_TEST_ID: AtomicUsize = AtomicUsize::new(0);

    struct TestDir(PathBuf);

    impl TestDir {
        fn new() -> Self {
            let id = NEXT_TEST_ID.fetch_add(1, Ordering::Relaxed);
            let path = Path::new("/tmp").join(format!("ldi-{}-{id}", std::process::id()));
            fs::create_dir(&path).expect("test directory should be created");
            Self(path)
        }

        fn socket_path(&self) -> PathBuf {
            self.0.join("control.sock")
        }
    }

    impl Drop for TestDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn rejects_second_server_for_live_socket() {
        let directory = TestDir::new();
        let socket_path = directory.socket_path();
        let server = Server::bind(&socket_path).expect("first server should bind");

        assert!(matches!(
            Server::bind(&socket_path),
            Err(Error::SocketInUse)
        ));
        assert!(socket_path.exists());

        drop(server);
        assert!(!socket_path.exists());
    }

    #[test]
    fn replaces_stale_socket() {
        let directory = TestDir::new();
        let socket_path = directory.socket_path();
        let stale_listener = UnixListener::bind(&socket_path).expect("stale socket should bind");
        drop(stale_listener);

        let server = Server::bind(&socket_path).expect("server should replace stale socket");
        assert!(socket_path.exists());

        drop(server);
        assert!(!socket_path.exists());
    }

    #[test]
    fn preserves_non_socket_path() {
        let directory = TestDir::new();
        let socket_path = directory.socket_path();
        fs::write(&socket_path, "keep me").expect("test file should be written");

        assert!(matches!(
            Server::bind(&socket_path),
            Err(Error::SocketPathOccupied)
        ));
        assert_eq!(
            fs::read_to_string(&socket_path).expect("test file should remain readable"),
            "keep me"
        );
    }
}
