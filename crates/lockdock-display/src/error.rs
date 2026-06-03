use crate::types::DisplayId;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0}")]
    Native(String),
    #[error("invalid display status")]
    InvalidStatus,
    #[error("could not identify target display")]
    MissingIdentity,
    #[error("display {0} not found")]
    DisplayNotFound(DisplayId),

    #[error("could not determine current Dock display")]
    MissingDockDisplay,
    #[error("could not determine current Dock display (Accessibility permission is not granted)")]
    AccessibilityNotTrusted,
}

pub type Result<T> = std::result::Result<T, Error>;
