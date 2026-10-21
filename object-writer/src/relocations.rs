use super::*;
use object::write::{Relocation as R, SectionId, SymbolId};
use object::{RelocationEncoding as E, RelocationFlags as F, RelocationKind as K, elf, macho, pe};

pub(super) fn width(arch: u32, kind: u32) -> Result<u64> {
    match (arch, kind) {
        (1..=3, 1) => Ok(8),
        (4, 0 | 7 | 8) => Ok(4),
        (1, 0 | 7) | (2, 2 | 3) => Ok(4),
        (2, 5) | (3, 4 | 6) => Ok(8),
        _ => Err(unsupported(
            "Relocation is not supported by this architecture",
        )),
    }
}
pub(super) fn word(data: &[u8]) -> u32 {
    u32::from_le_bytes(data[..4].try_into().unwrap())
}
pub(super) fn validate(r: &Relocation, data: &[u8]) -> Result<()> {
    let hi = word(data);
    let valid = match r.kind {
        0 | 1 | 7 | 8 => true,
        2 => hi & 0xfc000000 == 0x94000000,
        3 => hi & 0x9f000000 == 0x10000000 && hi & 31 != 31,
        5 => {
            let lo = word(&data[4..]);
            hi & 0x9f000000 == 0x90000000
                && lo & 0xffc00000 == 0x91000000
                && hi & 31 != 31
                && lo & 31 == hi & 31
                && (lo >> 5) & 31 == hi & 31
        }
        4 | 6 => {
            let lo = word(&data[4..]);
            let rd = (hi >> 7) & 31;
            hi & 127 == 0x17
                && rd != 0
                && lo & 0x707f == if r.kind == 4 { 0x67 } else { 0x13 }
                && (lo >> 15) & 31 == rd
                && (lo >> 7) & 31 == rd
                && (r.kind != 4 || rd == 1)
        }
        _ => false,
    };
    if !valid || (matches!(r.kind, 2..=6) && r.offset & 3 != 0) {
        return Err(invalid("Relocation does not match its instruction field"));
    }
    // Object relocations carry explicit addends in the descriptor. Require
    // unpatched instruction immediates; otherwise an implicit addend can leak.
    let zero = match r.kind {
        0 | 7 | 8 => hi == 0,
        1 => data[..8] == [0; 8],
        2 => hi & 0x03ffffff == 0,
        3 => hi & 0x60ffffe0 == 0,
        5 => hi & 0x60ffffe0 == 0 && word(&data[4..]) & 0x003ffc00 == 0,
        4 | 6 => hi & 0xfffff000 == 0 && word(&data[4..]) & 0xfff00000 == 0,
        _ => false,
    };
    if !zero {
        return Err(invalid(
            "Relocation fields must have zero addends before writing",
        ));
    }
    Ok(())
}
fn generic(kind: K, encoding: E, size: u8) -> F {
    F::Generic {
        kind,
        encoding,
        size,
    }
}
fn elf_flags(r_type: elf::RelocationType) -> F {
    F::Elf { r_type }
}
fn macho_flags(r_type: macho::RelocationType, r_pcrel: bool) -> F {
    F::MachO {
        r_type,
        r_pcrel,
        r_length: 2,
    }
}

pub(super) fn write(
    c: &Checked<'_>,
    object: &mut Object<'_>,
    sections: &[SectionId],
    symbols: &[SymbolId],
) -> Result<()> {
    for (index, r) in c.relocs.iter().enumerate() {
        let section = sections[r.section as usize];
        let symbol = symbols[r.symbol as usize];
        let mut addend = r.addend;
        let mut extra = None;
        let flags = match r.kind {
            0 | 7 => {
                addend = addend
                    .checked_sub(4)
                    .ok_or_else(|| range("PC-relative addend overflow"))?;
                // object 0.40 does not translate generic i386 PC-relative
                // Mach-O relocations. The native vanilla relocation carries
                // its PC-relative bit; the writer still adjusts the addend.
                if c.module.architecture == 4 && c.module.format == 3 {
                    macho_flags(macho::GENERIC_RELOC_VANILLA, true)
                } else {
                    generic(
                        K::Relative,
                        if c.module.architecture == 4 {
                            E::Generic
                        } else if r.kind == 7 {
                            E::X86Branch
                        } else {
                            E::X86RipRelative
                        },
                        32,
                    )
                }
            }
            1 => generic(K::Absolute, E::Generic, 64),
            8 => generic(K::Absolute, E::Generic, 32),
            2 => {
                if r.addend % 4 != 0 {
                    return Err(invalid("A64 call addend is not instruction aligned"));
                }
                generic(K::Relative, E::AArch64Call, 26)
            }
            3 if c.module.format == 1 => elf_flags(elf::R_AARCH64_ADR_PREL_LO21),
            4 => elf_flags(elf::R_RISCV_CALL_PLT),
            5 => {
                let (hi, lo) = match c.module.format {
                    1 => (
                        elf_flags(elf::R_AARCH64_ADR_PREL_PG_HI21),
                        elf_flags(elf::R_AARCH64_ADD_ABS_LO12_NC),
                    ),
                    2 => (
                        F::Coff {
                            typ: pe::IMAGE_REL_ARM64_PAGEBASE_REL21,
                        },
                        F::Coff {
                            typ: pe::IMAGE_REL_ARM64_PAGEOFFSET_12A,
                        },
                    ),
                    _ => (
                        macho_flags(macho::ARM64_RELOC_PAGE21, true),
                        macho_flags(macho::ARM64_RELOC_PAGEOFF12, false),
                    ),
                };
                extra = Some(R {
                    offset: r.offset + 4,
                    symbol,
                    addend,
                    flags: lo,
                });
                hi
            }
            6 => {
                // The LO12 relocation refers to the HI20 instruction's local
                // label, NOT the target symbol. This is required by the psABI.
                let label = object.add_symbol(Symbol {
                    name: format!(".Laster_pcrel_{index}").into_bytes(),
                    value: r.offset,
                    size: 0,
                    kind: SymbolKind::Label,
                    scope: SymbolScope::Compilation,
                    weak: false,
                    section: SymbolSection::Section(section),
                    flags: SymbolFlags::None,
                });
                extra = Some(R {
                    offset: r.offset + 4,
                    symbol: label,
                    addend: 0,
                    flags: elf_flags(elf::R_RISCV_PCREL_LO12_I),
                });
                elf_flags(elf::R_RISCV_PCREL_HI20)
            }
            _ => {
                return Err(unsupported(
                    "Relocation has no mapping for this object format",
                ));
            }
        };
        if c.module.format == 2 && matches!(r.kind, 2 | 5) && addend != 0 {
            return Err(unsupported(
                "COFF A64 instruction relocations currently require zero addends",
            ));
        }
        if c.module.format == 3
            && matches!(r.kind, 2 | 5)
            && !(-0x800000..=0x7fffff).contains(&addend)
        {
            return Err(range("Mach-O ARM64 explicit addend exceeds signed 24 bits"));
        }
        object
            .add_relocation(
                section,
                R {
                    offset: r.offset,
                    symbol,
                    addend,
                    flags,
                },
            )
            .map_err(writer)?;
        if let Some(extra) = extra {
            object.add_relocation(section, extra).map_err(writer)?;
        }
    }
    Ok(())
}
