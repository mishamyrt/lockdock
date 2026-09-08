use std::ffi::CStr;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Mutex, OnceLock};

use crate::{ffi, Error, Point, Result};

const ERROR_BUFFER_SIZE: usize = 512;

type Handler = Box<dyn Fn(Point) -> bool + Send + 'static>;

static HANDLER: OnceLock<Mutex<Option<Handler>>> = OnceLock::new();

pub struct EventTap;

impl EventTap {
    pub fn start(handler: impl Fn(Point) -> bool + Send + 'static) -> Result<Self> {
        let handlers = HANDLER.get_or_init(|| Mutex::new(None));
        let mut handlers = handlers.lock().map_err(|_| {
            Error::Native("mouse event handler mutex poisoned".to_owned())
        })?;
        if handlers.is_some() {
            return Err(Error::AlreadyRunning);
        }

        *handlers = Some(Box::new(handler));
        drop(handlers);

        let mut error = [0; ERROR_BUFFER_SIZE];
        if unsafe {
            ffi::lockdock_mouse_start_event_tap(error.as_mut_ptr(), error.len())
        } {
            Ok(Self)
        } else {
            clear_handler();
            let message =
                unsafe { CStr::from_ptr(error.as_ptr()) }.to_string_lossy();
            Err(Error::Native(if message.is_empty() {
                "native mouse operation failed".to_owned()
            } else {
                message.into_owned()
            }))
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
    x: f64,
    y: f64,
) -> bool {
    let handlers = HANDLER.get_or_init(|| Mutex::new(None));
    let Ok(handlers) = handlers.lock() else {
        return false;
    };
    let Some(handler) = handlers.as_ref() else {
        return false;
    };

    catch_unwind(AssertUnwindSafe(|| handler(Point { x, y }))).unwrap_or(false)
}

fn clear_handler() {
    if let Some(handlers) = HANDLER.get() {
        if let Ok(mut handlers) = handlers.lock() {
            *handlers = None;
        }
    }
}
