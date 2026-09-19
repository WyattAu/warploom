//! omnicpp_example_module — Rust gameplay module implementing the engine's
//! C ABI (`#[no_mangle] pub extern "C"`), the exact contract documented in
//! `engine/core/script_module.hpp`. No external crates: the module depends
//! only on `core`, so it links against nothing but libc.
//!
//! Contract:
//!   omnicpp_module_abi()  -> 1
//!   omnicpp_module_name() -> "rust_example"
//!   omnicpp_module_tick(dt, inputs, n, outputs, cap)
//!     Pure function of (dt, inputs): outputs[i] = inputs[i] * 0.5 + dt.
//!     Returns the output count, or a negative error code.
//!
//! The engine test suite dlopens the release build of this crate and proves
//! the same determinism/rejection properties as the C++ fixtures — the Rust
//! and C++ module paths are therefore drop-in interchangeable.

use std::os::raw::{c_char, c_int};

const MODULE_ABI: i32 = 1;

/// Deterministic tick: outputs[i] = inputs[i] * 0.5 + dt.
#[no_mangle]
pub extern "C" fn omnicpp_module_tick(
    dt: f64,
    inputs: *const f64,
    input_count: u32,
    outputs: *mut f64,
    output_capacity: u32,
) -> i32 {
    if inputs.is_null() && input_count != 0 {
        return -1;
    }
    if outputs.is_null() && output_capacity != 0 {
        return -1;
    }
    let n = input_count.min(output_capacity) as usize;
    unsafe {
        let in_slice = std::slice::from_raw_parts(inputs, n);
        let out_slice = std::slice::from_raw_parts_mut(outputs, n);
        for i in 0..n {
            out_slice[i] = in_slice[i] * 0.5 + dt;
        }
    }
    n as i32
}

#[no_mangle]
pub extern "C" fn omnicpp_module_abi() -> c_int {
    MODULE_ABI
}

#[no_mangle]
pub extern "C" fn omnicpp_module_name() -> *const c_char {
    b"rust_example\0".as_ptr() as *const c_char
}
