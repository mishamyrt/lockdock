use std::{collections::HashMap, io, process::Command};

use serde_json::Value::{self};
use thiserror::Error;

use crate::error::{Error, Result};
use crate::types::DisplayId;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DisplayInfo {
    pub name: String,
    pub is_builtin: bool,
    pub vendor_number: u32,
    pub model_number: u32,
    pub serial_number: u32,
}

#[derive(Debug, Error)]
pub(crate) enum SystemProfilerError {
    #[error(transparent)]
    Json(#[from] serde_json::Error),

    #[error(transparent)]
    Io(#[from] io::Error),

    #[error("system_profiler failed with output: {0}")]
    CmdFailed(String),
}

type SystemProfilerResult<T> = std::result::Result<T, SystemProfilerError>;

/// Get the display metadata from `system_profiler`.
pub fn load_display_info() -> Result<HashMap<DisplayId, DisplayInfo>> {
    let data = get_sp_displays_data().map_err(|error| Error::Native(error.to_string()))?;
    parse_displays_data(&data).map_err(|error| Error::Native(error.to_string()))
}

/// Parse the output of `system_profiler -json SPDisplaysDataType`.
fn parse_displays_data(data: &[u8]) -> SystemProfilerResult<HashMap<DisplayId, DisplayInfo>> {
    let root: Value = serde_json::from_slice(data)?;
    Ok(parse_displays_value(&root))
}

/// Get the output of `system_profiler` for the `SPDisplaysDataType`.
fn get_sp_displays_data() -> SystemProfilerResult<Vec<u8>> {
    let output = Command::new("/usr/sbin/system_profiler")
        .args(["-json", "SPDisplaysDataType"])
        .output()?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        return Err(SystemProfilerError::CmdFailed(stderr.into()));
    }

    Ok(output.stdout)
}

fn parse_displays_value(root: &Value) -> HashMap<DisplayId, DisplayInfo> {
    root.get("SPDisplaysDataType")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(|gpu| gpu.get("spdisplays_ndrvs").and_then(Value::as_array))
        .flatten()
        .filter_map(parse_display_value)
        .collect()
}

fn parse_display_value(value: &Value) -> Option<(DisplayId, DisplayInfo)> {
    let display_id = value
        .get("_spdisplays_displayID")
        .and_then(parse_display_id_value)?;
    let name = value
        .get("_name")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();

    Some((
        display_id,
        DisplayInfo {
            name,
            is_builtin: value
                .get("spdisplays_connection_type")
                .and_then(Value::as_str)
                == Some("spdisplays_internal"),
            vendor_number: value
                .get("_spdisplays_display-vendor-id")
                .and_then(parse_hex_u32_value)
                .unwrap_or(0),
            model_number: value
                .get("_spdisplays_display-product-id")
                .and_then(parse_hex_u32_value)
                .unwrap_or(0),
            serial_number: value
                .get("_spdisplays_display-serial-number")
                .and_then(parse_hex_u32_value)
                .unwrap_or(0),
        },
    ))
}

/// Parse a display ID from a `system_profiler` value.
fn parse_display_id_value(value: &Value) -> Option<DisplayId> {
    if let Some(text) = value.as_str() {
        return text.parse().ok().filter(|id| *id != 0);
    }

    value
        .as_u64()
        .and_then(|id| DisplayId::try_from(id).ok())
        .filter(|id| *id != 0)
}

fn parse_hex_u32_value(value: &Value) -> Option<u32> {
    if let Some(text) = value.as_str() {
        return u32::from_str_radix(text.trim_start_matches("0x"), 16)
            .ok()
            .filter(|id| *id != 0);
    }

    value
        .as_u64()
        .and_then(|id| u32::try_from(id).ok())
        .filter(|id| *id != 0)
}

#[cfg(test)]
mod tests {
    use super::{parse_displays_data, DisplayInfo};

    const DISPLAYS_JSON: &[u8] = br#"
{
  "SPDisplaysDataType" : [
    {
      "_name" : "Apple M3 Pro",
      "spdisplays_ndrvs" : [
        {
          "_name" : "Mi 27 NU",
          "_spdisplays_display-product-id" : "a005",
          "_spdisplays_display-vendor-id" : "61a9",
          "_spdisplays_displayID" : "2"
        },
        {
          "_name" : "Color LCD",
          "_spdisplays_display-product-id" : "a059",
          "_spdisplays_display-serial-number" : "fd626d62",
          "_spdisplays_display-vendor-id" : "610",
          "_spdisplays_displayID" : "1",
          "spdisplays_connection_type" : "spdisplays_internal"
        },
        {
          "_name" : "DeskPad Display",
          "_spdisplays_display-product-id" : "1234",
          "_spdisplays_display-serial-number" : "1",
          "_spdisplays_display-vendor-id" : "3456",
          "_spdisplays_displayID" : "4"
        }
      ]
    }
  ]
}
"#;

    #[test]
    fn parses_flat_display_metadata() {
        let displays = parse_displays_data(DISPLAYS_JSON).expect("valid display JSON");

        assert_eq!(displays.len(), 3);
        assert_eq!(
            displays.get(&2),
            Some(&DisplayInfo {
                name: "Mi 27 NU".to_owned(),
                is_builtin: false,
                vendor_number: 0x61a9,
                model_number: 0xa005,
                serial_number: 0,
            })
        );
        assert_eq!(
            displays.get(&1),
            Some(&DisplayInfo {
                name: "Color LCD".to_owned(),
                is_builtin: true,
                vendor_number: 0x610,
                model_number: 0xa059,
                serial_number: 0xfd62_6d62,
            })
        );
        assert_eq!(
            displays.get(&4),
            Some(&DisplayInfo {
                name: "DeskPad Display".to_owned(),
                is_builtin: false,
                vendor_number: 0x3456,
                model_number: 0x1234,
                serial_number: 1,
            })
        );
    }
}
