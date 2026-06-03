use serde::{Deserialize, Serialize};

use crate::{Error, Result, MAX_MESSAGE_SIZE};

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
