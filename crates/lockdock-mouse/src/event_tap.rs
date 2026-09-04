use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Mutex, OnceLock};

use crate::{ffi, Error, Point, Result};

const ERROR_BUFFER_SIZE: usize = 512;

type Handler = Box<dyn Fn(MouseEvent) -> bool + Send + 'static>;

static HANDLER: OnceLock<Mutex<Option<Handler>>> = OnceLock::new();

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MouseEventKind {
    Other,
    Moved,
    Dragged,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MouseEvent {
    pub kind: MouseEventKind,
    pub location: Point,
}

pub struct EventTap;

impl EventTap {
    pub fn start(
        handler: impl Fn(MouseEvent) -> bool + Send + 'static,
    ) -> Result<Self> {
        let handlers = HANDLER.get_or_init(|| Mutex::new(None));
        let mut handlers = handlers.lock().map_err(|_| {
            Error::Native("mouse event handler mutex poisoned".to_owned())
        })?;
        if handlers.is_some() {
            return Err(Error::AlreadyRunning);
        }

        *handlers = Some(Box::new(handler));
        drop(handlers);

        let mut error = ErrorBuffer::new();
        if unsafe {
            ffi::lockdock_mouse_start_event_tap(error.as_mut_ptr(), error.len())
        } {
            Ok(Self)
        } else {
            clear_handler();
            Err(Error::Native(error.to_string()))
        }
    }
}

impl Drop for EventTap {
    fn drop(&mut self) {
        unsafe { ffi::lockdock_mouse_stop_event_tap() };
        clear_handler();
    }
}

#[no_mangle]
pub(crate) extern "C" fn lockdock_mouse_should_suppress_event(
    event_kind: c_int,
    x: f64,
    y: f64,
) -> bool {
    let event = MouseEvent {
        kind: event_kind_from_raw(event_kind),
        location: Point { x, y },
    };

    let handlers = HANDLER.get_or_init(|| Mutex::new(None));
    let Ok(handlers) = handlers.lock() else {
        return false;
    };
    let Some(handler) = handlers.as_ref() else {
        return false;
    };

    catch_unwind(AssertUnwindSafe(|| handler(event))).unwrap_or(false)
}

fn event_kind_from_raw(kind: c_int) -> MouseEventKind {
    match kind {
        ffi::EVENT_MOUSE_MOVED => MouseEventKind::Moved,
        ffi::EVENT_MOUSE_DRAGGED => MouseEventKind::Dragged,
        _ => MouseEventKind::Other,
    }
}

fn clear_handler() {
    if let Some(handlers) = HANDLER.get() {
        if let Ok(mut handlers) = handlers.lock() {
            *handlers = None;
        }
    }
}

struct ErrorBuffer([c_char; ERROR_BUFFER_SIZE]);

impl ErrorBuffer {
    fn new() -> Self {
        Self([0; ERROR_BUFFER_SIZE])
    }

    fn as_mut_ptr(&mut self) -> *mut c_char {
        self.0.as_mut_ptr()
    }

    fn len(&self) -> usize {
        self.0.len()
    }
}

impl std::fmt::Display for ErrorBuffer {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let message = unsafe { CStr::from_ptr(self.0.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        if message.is_empty() {
            formatter.write_str("native mouse operation failed")
        } else {
            formatter.write_str(&message)
        }
    }
}
