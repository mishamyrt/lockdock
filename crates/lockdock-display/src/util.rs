use std::ffi::CStr;
use std::os::raw::c_char;

pub(crate) fn c_string(buffer: &[c_char]) -> String {
    unsafe { CStr::from_ptr(buffer.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}
