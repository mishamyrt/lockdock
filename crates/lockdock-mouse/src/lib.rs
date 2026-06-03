mod event_source;
mod event_tap;
mod ffi;
mod point;

pub use event_source::EventSource;
pub use event_tap::{EventTap, MouseEvent, MouseEventKind};
pub use point::Point;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0}")]
    Native(String),
    #[error("mouse event tap is already running")]
    AlreadyRunning,
}

pub type Result<T> = std::result::Result<T, Error>;

#[must_use]
pub fn current_location() -> Option<Point> {
    let mut point = Point::default();
    unsafe { ffi::lockdock_mouse_copy_location(&raw mut point) }.then_some(point)
}

pub fn warp(point: Point) {
    unsafe { ffi::lockdock_mouse_warp(point) };
}
