//! Raw `extern "C"` FFI declarations matching `c_api/nyxstone_c.h`.
//!
//! Hand-written rather than bindgen-generated to keep the build dependency
//! footprint small (no clang at build time).  If the C ABI header changes,
//! the matching declarations here must be updated.

#![allow(non_camel_case_types, non_upper_case_globals)]

use std::os::raw::{c_char, c_int};

#[repr(C)]
pub struct nyxstone_handle_t {
    _private: [u8; 0],
}

pub type nyxstone_status_t = c_int;
pub const NYXSTONE_OK:                     nyxstone_status_t = 0;
pub const NYXSTONE_ERR_INIT:               nyxstone_status_t = 1;
pub const NYXSTONE_ERR_NULL_ARG:           nyxstone_status_t = 2;
pub const NYXSTONE_ERR_ASSEMBLE_FAILED:    nyxstone_status_t = 3;
pub const NYXSTONE_ERR_SECTION_VIOLATION:  nyxstone_status_t = 4;
pub const NYXSTONE_ERR_DISASM_FAILED:      nyxstone_status_t = 5;
pub const NYXSTONE_ERR_ALLOC:              nyxstone_status_t = 6;

#[repr(C)]
pub struct nyxstone_label_def_t {
    pub name:    *const c_char,
    pub address: u64,
}

#[repr(C)]
pub struct nyxstone_instruction_t {
    pub address:   u64,
    pub assembly:  *mut c_char,
    pub bytes:     *mut u8,
    pub bytes_len: usize,
}

#[repr(C)]
pub struct nyxstone_reloc_symbol_t {
    pub name:    *mut c_char,
    pub address: u64,
}

#[repr(C)]
pub struct nyxstone_reloc_t {
    pub offset:          u64,
    pub addend:          i64,
    pub has_addend:      c_int,
    pub symbol:          nyxstone_reloc_symbol_t,
    pub relocation_type: u32,
}

extern "C" {
    pub fn nyxstone_create(out_err: *mut *mut c_char) -> *mut nyxstone_handle_t;
    pub fn nyxstone_destroy(h: *mut nyxstone_handle_t);

    pub fn nyxstone_assemble(
        h: *mut nyxstone_handle_t,
        source: *const c_char, src_len: usize,
        address: u64,
        labels: *const nyxstone_label_def_t, labels_len: usize,
        out_bytes: *mut *mut u8, out_len: *mut usize,
        out_err: *mut *mut c_char,
    ) -> nyxstone_status_t;

    pub fn nyxstone_assemble_to_instructions(
        h: *mut nyxstone_handle_t,
        source: *const c_char, src_len: usize,
        address: u64,
        labels: *const nyxstone_label_def_t, labels_len: usize,
        out: *mut *mut nyxstone_instruction_t, out_n: *mut usize,
        out_err: *mut *mut c_char,
    ) -> nyxstone_status_t;

    pub fn nyxstone_disassemble(
        h: *mut nyxstone_handle_t,
        bytes: *const u8, bytes_len: usize,
        address: u64,
        count: usize,
        out_text: *mut *mut c_char,
        out_err: *mut *mut c_char,
    ) -> nyxstone_status_t;

    pub fn nyxstone_disassemble_to_instructions(
        h: *mut nyxstone_handle_t,
        bytes: *const u8, bytes_len: usize,
        address: u64,
        count: usize,
        out: *mut *mut nyxstone_instruction_t, out_n: *mut usize,
        out_err: *mut *mut c_char,
    ) -> nyxstone_status_t;

    pub fn nyxstone_assemble_with_relocs(
        h: *mut nyxstone_handle_t,
        source: *const c_char, src_len: usize,
        address: u64,
        labels: *const nyxstone_label_def_t, labels_len: usize,
        out_bytes: *mut *mut u8, out_bytes_len: *mut usize,
        out_relocs: *mut *mut nyxstone_reloc_t, out_relocs_n: *mut usize,
        out_err: *mut *mut c_char,
    ) -> nyxstone_status_t;

    pub fn nyxstone_assemble_to_instructions_with_relocs(
        h: *mut nyxstone_handle_t,
        source: *const c_char, src_len: usize,
        address: u64,
        labels: *const nyxstone_label_def_t, labels_len: usize,
        out_ins: *mut *mut nyxstone_instruction_t, out_ins_n: *mut usize,
        out_relocs: *mut *mut nyxstone_reloc_t, out_relocs_n: *mut usize,
        out_err: *mut *mut c_char,
    ) -> nyxstone_status_t;

    pub fn nyxstone_free_bytes(p: *mut u8);
    pub fn nyxstone_free_string(p: *mut c_char);
    pub fn nyxstone_free_instructions(p: *mut nyxstone_instruction_t, n: usize);
    pub fn nyxstone_free_relocations(p: *mut nyxstone_reloc_t, n: usize);
}
