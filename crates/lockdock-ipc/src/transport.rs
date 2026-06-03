use std::{io::Read as _, os::unix::net::UnixStream, path::Path};

use crate::{Error, Result};

pub const MAX_MESSAGE_SIZE: usize = 4096;
const MAX_SOCKET_PATH_SIZE: usize = 104;

pub(crate) fn read_message(stream: &mut UnixStream) -> Result<String> {
    let mut buffer = Vec::with_capacity(MAX_MESSAGE_SIZE);
    stream
        .take((MAX_MESSAGE_SIZE + 1) as u64)
        .read_to_end(&mut buffer)?;
    if buffer.len() > MAX_MESSAGE_SIZE {
        return Err(Error::MessageTooLarge);
    }

    Ok(String::from_utf8_lossy(&buffer).trim().to_owned())
}

pub(crate) fn validate_socket_path(path: &Path) -> Result<()> {
    if path.as_os_str().len() >= MAX_SOCKET_PATH_SIZE {
        return Err(Error::SocketPathTooLong);
    }

    Ok(())
}
