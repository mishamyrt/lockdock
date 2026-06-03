pub type DisplayId = u32;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DisplayIdentity {
    pub is_builtin: bool,
    pub vendor_number: u32,
    pub model_number: u32,
    pub serial_number: u32,
    pub uuid: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Status {
    pub displays: Vec<DisplayId>,
    pub location_index: usize,
}
