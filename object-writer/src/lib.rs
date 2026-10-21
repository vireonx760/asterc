//! Batched object/flat-image writer with a C ABI. See include/asterasm/object_writer.h.
mod flat;
mod relocations;
use object::write::{Object, Symbol, SymbolSection};
use object::{
    Architecture, BinaryFormat, Endianness, SectionKind, SymbolFlags, SymbolKind, SymbolScope,
};
use std::{collections::HashSet, mem, panic, ptr, slice};

pub const UNDEFINED: u32 = u32::MAX;
pub const ABSOLUTE: u32 = u32::MAX - 1;
type Result<T> = std::result::Result<T, Error>;
#[derive(Debug)]
struct Error(u32, String);
fn invalid(s: &str) -> Error {
    Error(1, s.into())
}
fn unsupported(s: &str) -> Error {
    Error(2, s.into())
}
fn range(s: &str) -> Error {
    Error(3, s.into())
}
fn writer(e: object::write::Error) -> Error {
    Error(5, e.to_string())
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Bytes {
    pub data: *const u8,
    pub size: u64,
}
#[repr(C)]
pub struct Section {
    pub name: Bytes,
    pub data: Bytes,
    pub size: u64,
    pub alignment: u64,
    pub kind: u32,
    pub reserved: u32,
}
#[repr(C)]
pub struct InputSymbol {
    pub name: Bytes,
    pub value: u64,
    pub size: u64,
    pub section: u32,
    pub binding: u32,
    pub kind: u32,
    pub reserved: u32,
}
#[repr(C)]
pub struct Relocation {
    pub offset: u64,
    pub addend: i64,
    pub section: u32,
    pub symbol: u32,
    pub kind: u32,
    pub reserved: u32,
}
#[repr(C)]
pub struct Module {
    pub abi_version: u32,
    pub format: u32,
    pub architecture: u32,
    pub reserved: u32,
    pub sections: *const Section,
    pub section_count: u64,
    pub symbols: *const InputSymbol,
    pub symbol_count: u64,
    pub relocations: *const Relocation,
    pub relocation_count: u64,
    pub base_address: u64,
    pub image_limit: u64,
    pub entry_symbol: u32,
    pub riscv_float_abi: u32,
}
#[repr(C)]
pub struct Output {
    pub data: *mut u8,
    pub size: u64,
    pub entry_address: u64,
    pub status: u32,
    pub message: [u8; 256],
}
impl Default for Output {
    fn default() -> Self {
        Self {
            data: ptr::null_mut(),
            size: 0,
            entry_address: 0,
            status: 0,
            message: [0; 256],
        }
    }
}

// Pointer validity/lifetime is a C caller obligation. Validate what can be
// checked before slice construction: null, alignment and representable size.
unsafe fn borrowed<'a, T>(p: *const T, n: u64) -> Result<&'a [T]> {
    if n == 0 {
        return Ok(&[]);
    }
    if p.is_null()
        || (p as usize) % mem::align_of::<T>() != 0
        || n > (isize::MAX as u64) / mem::size_of::<T>() as u64
    {
        return Err(invalid("Invalid input pointer, alignment or length"));
    }
    Ok(unsafe { slice::from_raw_parts(p, n as usize) })
}
struct Checked<'a> {
    module: &'a Module,
    sections: &'a [Section],
    symbols: &'a [InputSymbol],
    relocs: &'a [Relocation],
    section_names: Vec<&'a [u8]>,
    symbol_names: Vec<&'a [u8]>,
    data: Vec<&'a [u8]>,
}
unsafe fn check(m: &Module) -> Result<Checked<'_>> {
    if m.abi_version != 1 || m.reserved != 0 {
        return Err(invalid("Unsupported ABI version or reserved field"));
    }
    if !(1..=4).contains(&m.format) || !(1..=4).contains(&m.architecture) {
        return Err(unsupported("Unknown format or architecture"));
    }
    if m.architecture == 3 && (m.format == 2 || m.format == 3) {
        return Err(unsupported("RV64 supports ELF and FLAT"));
    }
    if m.riscv_float_abi > 2 || (m.architecture != 3 && m.riscv_float_abi != 0) {
        return Err(invalid("Invalid RISC-V floating ABI"));
    }
    if m.section_count >= ABSOLUTE as u64
        || m.symbol_count >= UNDEFINED as u64
        || m.relocation_count > u32::MAX as u64
    {
        return Err(range("Too many input records"));
    }
    let sections = unsafe { borrowed(m.sections, m.section_count)? };
    let symbols = unsafe { borrowed(m.symbols, m.symbol_count)? };
    let relocs = unsafe { borrowed(m.relocations, m.relocation_count)? };
    let mut c = Checked {
        module: m,
        sections,
        symbols,
        relocs,
        section_names: Vec::with_capacity(sections.len()),
        symbol_names: Vec::with_capacity(symbols.len()),
        data: Vec::with_capacity(sections.len()),
    };
    let mut names = HashSet::with_capacity(sections.len());
    for s in sections {
        let name = unsafe { borrowed(s.name.data, s.name.size)? };
        let data = unsafe { borrowed(s.data.data, s.data.size)? };
        if name.is_empty() || name.contains(&0) || !names.insert(name) || s.reserved != 0 {
            return Err(invalid("Invalid or duplicate section name"));
        }
        if !(1..=4).contains(&s.kind)
            || !s.alignment.is_power_of_two()
            || s.alignment > (1 << 31)
            || s.size > isize::MAX as u64
        {
            return Err(invalid("Invalid section kind, size or alignment"));
        }
        if (s.kind == 4 && !data.is_empty()) || (s.kind != 4 && s.data.size != s.size) {
            return Err(invalid("Section size/data mismatch"));
        }
        if m.format == 2 && s.alignment > 8192 {
            return Err(unsupported("COFF section alignment exceeds 8192"));
        }
        if m.format == 3 && name.len() > 16 {
            return Err(unsupported("Mach-O section names are limited to 16 bytes"));
        }
        c.section_names.push(name);
        c.data.push(data);
    }
    let mut globals = HashSet::with_capacity(symbols.len());
    for s in symbols {
        let name = unsafe { borrowed(s.name.data, s.name.size)? };
        if name.is_empty()
            || name.contains(&0)
            || s.binding > 2
            || !(1..=2).contains(&s.kind)
            || s.reserved != 0
        {
            return Err(invalid("Invalid symbol record"));
        }
        if s.binding != 0 && !globals.insert(name) {
            return Err(invalid("Duplicate externally visible symbol"));
        }
        if s.section < ABSOLUTE {
            let section = sections
                .get(s.section as usize)
                .ok_or_else(|| invalid("Invalid symbol section"))?;
            if s.value > section.size || s.size > section.size - s.value {
                return Err(range("Symbol exceeds section"));
            }
        } else if s.section == UNDEFINED && (s.value != 0 || s.size != 0 || s.binding == 0) {
            return Err(invalid("Invalid undefined symbol"));
        }
        c.symbol_names.push(name);
    }
    if m.entry_symbol != UNDEFINED {
        let s = symbols
            .get(m.entry_symbol as usize)
            .ok_or_else(|| invalid("Invalid entry symbol"))?;
        if s.section >= ABSOLUTE
            || s.kind != 1
            || sections[s.section as usize].kind != 1
            || s.value == sections[s.section as usize].size
        {
            return Err(invalid("Entry must name a defined instruction"));
        }
    }
    let mut ranges = Vec::with_capacity(relocs.len());
    for r in relocs {
        if r.reserved != 0 || r.symbol as usize >= symbols.len() {
            return Err(invalid("Invalid relocation symbol/reserved field"));
        }
        let s = sections
            .get(r.section as usize)
            .ok_or_else(|| invalid("Invalid relocation section"))?;
        let width = relocations::width(m.architecture, r.kind)?;
        if s.kind == 4 || r.offset > s.size || width > s.size - r.offset {
            return Err(range("Relocation outside initialized section"));
        }
        relocations::validate(r, &c.data[r.section as usize][r.offset as usize..])?;
        ranges.push((r.section, r.offset, r.offset + width));
    }
    ranges.sort_unstable();
    for pair in ranges.windows(2) {
        if pair[0].0 == pair[1].0 && pair[0].2 > pair[1].1 {
            return Err(invalid("Overlapping relocation fields"));
        }
    }
    Ok(c)
}

fn write_object(c: &Checked<'_>) -> Result<Vec<u8>> {
    let format = match c.module.format {
        1 => BinaryFormat::Elf,
        2 => BinaryFormat::Coff,
        _ => BinaryFormat::MachO,
    };
    let arch = match c.module.architecture {
        1 => Architecture::X86_64,
        2 => Architecture::Aarch64,
        3 => Architecture::Riscv64,
        _ => Architecture::I386,
    };
    let mut object = Object::new(format, arch, Endianness::Little);
    object.mangling = object::write::Mangling::None; // Names cross the C ABI verbatim.
    if arch == Architecture::Riscv64 {
        object.flags = object::FileFlags::Elf {
            os_abi: object::elf::ELFOSABI_NONE,
            abi_version: 0,
            e_flags: object::elf::FileFlags(c.module.riscv_float_abi * 2),
        };
    }
    let mut sections = Vec::with_capacity(c.sections.len());
    for (i, s) in c.sections.iter().enumerate() {
        let kind = match s.kind {
            1 => SectionKind::Text,
            2 => SectionKind::ReadOnlyData,
            3 => SectionKind::Data,
            _ => SectionKind::UninitializedData,
        };
        let segment = if format == BinaryFormat::MachO {
            if s.kind <= 2 {
                b"__TEXT".to_vec()
            } else {
                b"__DATA".to_vec()
            }
        } else {
            Vec::new()
        };
        let id = object.add_section(segment, c.section_names[i].to_vec(), kind);
        if s.kind == 4 {
            object.section_mut(id).append_bss(s.size, s.alignment);
        } else {
            object.section_mut(id).set_data(c.data[i], s.alignment);
        }
        sections.push(id);
    }
    let mut symbols = Vec::with_capacity(c.symbols.len());
    for (i, s) in c.symbols.iter().enumerate() {
        symbols.push(object.add_symbol(Symbol {
            name: c.symbol_names[i].to_vec(),
            value: s.value,
            size: s.size,
            kind: if s.kind == 1 {
                SymbolKind::Text
            } else {
                SymbolKind::Data
            },
            scope: if s.binding == 0 {
                SymbolScope::Compilation
            } else if s.section == UNDEFINED {
                /* Undefined externs are dynamic-linkage candidates. Defined
                 * global/weak symbols belong to the static linkage unit;
                 * marking them Dynamic can incorrectly request export-like
                 * semantics in formats such as COFF. */
                SymbolScope::Dynamic
            } else {
                SymbolScope::Linkage
            },
            weak: s.binding == 2,
            section: match s.section {
                UNDEFINED => SymbolSection::Undefined,
                ABSOLUTE => SymbolSection::Absolute,
                _ => SymbolSection::Section(sections[s.section as usize]),
            },
            flags: SymbolFlags::None,
        }));
    }
    relocations::write(c, &mut object, &sections, &symbols)?;
    object.write().map_err(writer)
}

/// # Safety
/// All non-null pointers must satisfy the readable/writable memory and lifetime
/// contracts in object_writer.h. `out` must not own an unreleased prior result.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ast_object_write(module: *const Module, out: *mut Output) -> u32 {
    if out.is_null() || (out as usize) % mem::align_of::<Output>() != 0 {
        return 1;
    }
    unsafe { ptr::write(out, Output::default()) };
    let result = panic::catch_unwind(|| -> Result<(Vec<u8>, u64)> {
        let modules = unsafe { borrowed(module, 1)? };
        let c = unsafe { check(&modules[0])? };
        if c.module.format == 4 {
            flat::write(&c)
        } else {
            Ok((write_object(&c)?, 0))
        }
    })
    .unwrap_or_else(|_| Err(Error(6, "Internal writer panic".into())));
    let output = unsafe { &mut *out };
    match result {
        Ok((bytes, entry)) => {
            output.entry_address = entry;
            if !bytes.is_empty() {
                let boxed = bytes.into_boxed_slice();
                output.size = boxed.len() as u64;
                output.data = Box::into_raw(boxed) as *mut u8;
            }
        }
        Err(Error(code, message)) => {
            output.status = code;
            let n = message.len().min(output.message.len() - 1);
            output.message[..n].copy_from_slice(&message.as_bytes()[..n]);
        }
    }
    output.status
}
/// # Safety
/// `out` is null or points to the original, unmodified owned output record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ast_object_output_free(out: *mut Output) {
    if out.is_null() {
        return;
    }
    let output = unsafe { &mut *out };
    if !output.data.is_null() {
        unsafe {
            drop(Box::from_raw(ptr::slice_from_raw_parts_mut(
                output.data,
                output.size as usize,
            )))
        };
    }
    *output = Output::default();
}

#[cfg(test)]
mod tests;
