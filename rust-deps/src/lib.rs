// TODO: remove conditional compliation stuff from inside addr2line/util modules since not using it

#[cfg(not(target_os = "windows"))]
pub mod addr2line;

#[cfg(not(target_os = "windows"))]
pub mod util;

pub use lolhtml::*;
