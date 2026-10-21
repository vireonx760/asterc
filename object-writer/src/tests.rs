use super::*;
use object::{Object as _, ObjectSection as _, ObjectSymbol as _};

fn bytes(data: &[u8]) -> Bytes {
    Bytes {
        data: data.as_ptr(),
        size: data.len() as u64,
    }
}
fn section(name: &[u8], data: &[u8], kind: u32, alignment: u64) -> Section {
    Section {
        name: bytes(name),
        data: bytes(data),
        size: data.len() as u64,
        alignment,
        kind,
        reserved: 0,
    }
}
fn symbol(name: &[u8], section: u32, value: u64) -> InputSymbol {
    InputSymbol {
        name: bytes(name),
        value,
        size: 0,
        section,
        binding: 1,
        kind: 1,
        reserved: 0,
    }
}
fn module(
    format: u32,
    architecture: u32,
    sections: &[Section],
    symbols: &[InputSymbol],
    relocs: &[Relocation],
) -> Module {
    Module {
        abi_version: 1,
        format,
        architecture,
        reserved: 0,
        sections: sections.as_ptr(),
        section_count: sections.len() as u64,
        symbols: symbols.as_ptr(),
        symbol_count: symbols.len() as u64,
        relocations: relocs.as_ptr(),
        relocation_count: relocs.len() as u64,
        base_address: 0x100000,
        image_limit: 0x100000,
        entry_symbol: 0,
        riscv_float_abi: 0,
    }
}
fn write(m: &Module) -> Result<Vec<u8>> {
    let mut out = Output::default();
    unsafe { ast_object_write(m, &mut out) };
    let result = if out.status == 0 {
        Ok(unsafe { borrowed(out.data, out.size).unwrap() }.to_vec())
    } else {
        assert!(out.data.is_null());
        assert_eq!(out.size, 0);
        Err(Error(
            out.status,
            String::from_utf8_lossy(&out.message)
                .trim_end_matches('\0')
                .into(),
        ))
    };
    unsafe {
        ast_object_output_free(&mut out);
        ast_object_output_free(&mut out);
    }
    assert!(out.data.is_null());
    assert_eq!(out.size, 0);
    result
}
fn forms(arch: u32) -> Vec<(u32, Vec<u8>)> {
    match arch {
        1 => vec![(7, vec![0; 4]), (0, vec![0; 4]), (1, vec![0; 8])],
        2 => vec![
            (2, 0x94000000u32.to_le_bytes().to_vec()),
            (
                5,
                [0x90000009u32.to_le_bytes(), 0x91000129u32.to_le_bytes()].concat(),
            ),
            (1, vec![0; 8]),
        ],
        3 => vec![
            (4, [0x97u32.to_le_bytes(), 0x80e7u32.to_le_bytes()].concat()),
            (
                6,
                [0x517u32.to_le_bytes(), 0x50513u32.to_le_bytes()].concat(),
            ),
            (1, vec![0; 8]),
        ],
        _ => vec![(7, vec![0; 4]), (0, vec![0; 4]), (8, vec![0; 4])],
    }
}
#[test]
fn all_supported_object_pairs_roundtrip() {
    for arch in 1..=4 {
        for format in 1..=3 {
            if arch == 3 && format != 1 {
                continue;
            }
            for (kind, data) in forms(arch) {
                let sections = [
                    section(b".text", &data, 1, 16),
                    section(b".data", &[2; 16], 3, 16),
                    Section {
                        size: 64,
                        ..section(b".bss", &[], 4, 32)
                    },
                ];
                let symbols = [symbol(b"entry", 0, 0), symbol(b"external", UNDEFINED, 0)];
                let relocs = [Relocation {
                    section: 0,
                    symbol: 1,
                    offset: 0,
                    addend: 0,
                    kind,
                    reserved: 0,
                }];
                let m = module(format, arch, &sections, &symbols, &relocs);
                let output = write(&m)
                    .unwrap_or_else(|e| panic!("format={format} arch={arch} kind={kind}: {e:?}"));
                assert_eq!(output, write(&m).unwrap(), "nondeterministic output");
                let file = object::File::parse(&*output).unwrap();
                assert_eq!(
                    file.architecture(),
                    match arch {
                        1 => Architecture::X86_64,
                        2 => Architecture::Aarch64,
                        3 => Architecture::Riscv64,
                        _ => Architecture::I386,
                    }
                );
                let text = file.section_by_name(".text").unwrap();
                assert_eq!(text.size(), data.len() as u64);
                assert_eq!(file.section_by_name(".bss").unwrap().size(), 64);
                let rs: Vec<_> = text.relocations().collect();
                assert_eq!(rs.len(), if kind == 5 || kind == 6 { 2 } else { 1 });
                assert!(
                    file.symbols()
                        .any(|s| s.name() == Ok("external") && s.is_undefined()),
                    "format={format} arch={arch} kind={kind}"
                );
                if kind == 6 {
                    let lo = rs.iter().find(|(offset, _)| *offset == 4).unwrap();
                    let object::RelocationTarget::Symbol(id) = lo.1.target() else {
                        panic!("LO12 must target its HI20 label")
                    };
                    let label = file.symbol_by_index(id).unwrap();
                    assert_eq!(label.address(), 0);
                    assert_eq!(label.section_index(), Some(text.index()));
                }
            }
        }
    }
}
#[test]
fn flat_layout_and_relocations() {
    for arch in 1..=4 {
        for (kind, data) in forms(arch) {
            let bases: &[u64] = if kind == 8 {
                &[0x100003, 0x100000, 0x7fff0000]
            } else {
                &[0x100003, 0x100000, 0xffff_ffff_0000_0000]
            };
            for &base in bases {
                let sections = [
                    section(b".text", &data, 1, 16),
                    section(b".data", &[7; 16], 3, 32),
                    Section {
                        size: 9,
                        ..section(b".bss", &[], 4, 16)
                    },
                ];
                let symbols = [symbol(b"entry", 0, 0), symbol(b"target", 1, 0)];
                let relocs = [Relocation {
                    section: 0,
                    symbol: 1,
                    offset: 0,
                    addend: 0,
                    kind,
                    reserved: 0,
                }];
                let mut m = module(4, arch, &sections, &symbols, &relocs);
                m.base_address = base;
                let out = write(&m).unwrap();
                let pc = (base + 15) & !15;
                let target = (pc + data.len() as u64 + 31) & !31;
                let at = (pc - base) as usize;
                let hi = relocations::word(&out[at..]);
                let decoded = match kind {
                    0 | 7 => (pc as i128 + 4 + hi as i32 as i128) as u64,
                    1 => u64::from_le_bytes(out[at..at + 8].try_into().unwrap()),
                    8 => u32::from_le_bytes(out[at..at + 4].try_into().unwrap()) as u64,
                    2 => (pc as i128 + (((hi << 6) as i32 >> 6) as i128 * 4)) as u64,
                    5 => {
                        let page_imm = (((hi >> 29) & 3) | (((hi >> 5) & 0x7ffff) << 2)) as i32;
                        let delta = (page_imm << 11) >> 11;
                        let lo = relocations::word(&out[at + 4..]);
                        ((pc & !4095) as i128
                            + (delta as i128 * 4096)
                            + ((lo >> 10) & 4095) as i128) as u64
                    }
                    _ => {
                        let lo = relocations::word(&out[at + 4..]);
                        (pc as i128 + (hi & 0xfffff000) as i32 as i128 + (lo as i32 >> 20) as i128)
                            as u64
                    }
                };
                assert_eq!(decoded, target, "arch={arch} kind={kind}");
                assert_eq!(
                    &out[(target - base) as usize..(target - base) as usize + 16],
                    &[7; 16]
                );
                assert_eq!(&out[out.len() - 9..], &[0; 9]);
            }
        }
    }
}
#[test]
fn elf_symbol_binding_and_visibility_roundtrip() {
    let data = [0u8; 4];
    let sections = [section(b".text", &data, 1, 4)];
    let symbols = [symbol(b"defined", 0, 0), symbol(b"external", UNDEFINED, 0)];
    let relocs = [Relocation {
        section: 0,
        symbol: 1,
        offset: 0,
        addend: 0,
        kind: 7,
        reserved: 0,
    }];
    let m = module(1, 1, &sections, &symbols, &relocs);
    let output = write(&m).unwrap();
    let file = object::File::parse(&*output).unwrap();
    let defined = file.symbols().find(|s| s.name() == Ok("defined")).unwrap();
    let external = file.symbols().find(|s| s.name() == Ok("external")).unwrap();
    assert_eq!(defined.scope(), SymbolScope::Linkage);
    // ELF readers intentionally report Unknown for SHN_UNDEF. Check the
    // serialized binding and visibility instead of assuming scope roundtrips.
    assert_eq!(external.scope(), SymbolScope::Unknown);
    assert!(external.is_undefined());
    assert!(external.is_global());
    let SymbolFlags::Elf { st_info, st_other } = external.flags() else {
        panic!("expected ELF symbol flags");
    };
    assert_eq!(st_info.0 >> 4, object::elf::STB_GLOBAL.0);
    assert_eq!(st_other.0 & 3, object::elf::STV_DEFAULT.0);
}

#[test]
fn malformed_inputs_are_transactional() {
    let data = [0; 8];
    let mut sections = [section(b".text", &data, 1, 16)];
    let mut symbols = [symbol(b"entry", 0, 0), symbol(b"external", UNDEFINED, 0)];
    let mut relocs = [Relocation {
        section: 0,
        symbol: 1,
        offset: 0,
        addend: 0,
        kind: 1,
        reserved: 0,
    }];
    let mut m = module(4, 1, &sections, &symbols, &relocs);
    assert_eq!(write(&m).unwrap_err().0, 4);
    symbols[1].section = ABSOLUTE;
    symbols[1].value = 0x1234;
    m.symbols = symbols.as_ptr();
    assert!(write(&m).is_ok());
    relocs[0].offset = 1;
    assert_eq!(write(&m).unwrap_err().0, 3);
    relocs[0].offset = 0;
    relocs[0].symbol = 2;
    assert_eq!(write(&m).unwrap_err().0, 1);
    relocs[0].symbol = 1;
    sections[0].alignment = 3;
    assert_eq!(write(&m).unwrap_err().0, 1);
    sections[0].alignment = 16;
    m.sections = sections.as_ptr();
    m.abi_version = 2;
    assert_eq!(write(&m).unwrap_err().0, 1);
    m.abi_version = 1;
    m.base_address = u64::MAX;
    assert_eq!(write(&m).unwrap_err().0, 3);
    m.base_address = 0x100000;
    m.image_limit = 1;
    assert_eq!(write(&m).unwrap_err().0, 3);
    m.image_limit = 1024;
    relocs[0].addend = i64::MIN;
    assert_eq!(write(&m).unwrap_err().0, 3);
    relocs[0].addend = 0;
    m.relocations = relocs.as_ptr();
    m.sections = ptr::null();
    assert_eq!(write(&m).unwrap_err().0, 1);
    let mut out = Output::default();
    assert_eq!(unsafe { ast_object_write(ptr::null(), &mut out) }, 1);
    assert_eq!(unsafe { ast_object_write(&m, ptr::null_mut()) }, 1);
}

#[test]
fn i386_macho_pcrel_flags_and_implicit_addends() {
    for kind in [0, 7, 8] {
        for addend in [-17, 0, 23] {
            let data = [0u8; 12];
            let sections = [section(b".text", &data, 1, 4)];
            let symbols = [symbol(b"entry", 0, 0), symbol(b"external", UNDEFINED, 0)];
            let relocs = [Relocation {
                section: 0,
                symbol: 1,
                offset: 4,
                addend,
                kind,
                reserved: 0,
            }];
            let output = write(&module(3, 4, &sections, &symbols, &relocs)).unwrap();
            let file = object::File::parse(&*output).unwrap();
            let text = file.section_by_name(".text").unwrap();
            let rs: Vec<_> = text.relocations().collect();
            assert_eq!(rs.len(), 1);
            assert_eq!(rs[0].0, 4);
            assert_eq!(
                rs[0].1.flags(),
                object::RelocationFlags::MachO {
                    r_type: object::macho::GENERIC_RELOC_VANILLA,
                    r_pcrel: kind != 8,
                    r_length: 2,
                }
            );
            let object::RelocationTarget::Symbol(id) = rs[0].1.target() else {
                panic!("external relocation must name its symbol");
            };
            assert_eq!(file.symbol_by_index(id).unwrap().name(), Ok("external"));
            let encoded = text.data().unwrap();
            assert_eq!(
                i32::from_le_bytes(encoded[4..8].try_into().unwrap()),
                addend as i32
            );
            assert_eq!(&encoded[..4], &[0; 4]);
            assert_eq!(&encoded[8..], &[0; 4]);
        }
    }
}

#[test]
fn flat_abs32_boundary_values_and_overflow() {
    let data = [0u8; 4];
    let sections = [section(b".text", &data, 1, 4)];
    for (address, addend, expected) in [
        (0, 0, Some(0)),
        (u32::MAX as u64, 0, Some(u32::MAX)),
        (u32::MAX as u64 + 1, -1, Some(u32::MAX)),
        (u32::MAX as u64, 1, None),
        (u32::MAX as u64 + 1, 0, None),
        (0, -1, None),
    ] {
        let symbols = [symbol(b"entry", 0, 0), symbol(b"target", ABSOLUTE, address)];
        let relocs = [Relocation {
            section: 0,
            symbol: 1,
            offset: 0,
            addend,
            kind: 8,
            reserved: 0,
        }];
        let result = write(&module(4, 4, &sections, &symbols, &relocs));
        match expected {
            Some(bits) => assert_eq!(result.unwrap(), bits.to_le_bytes()),
            None => assert_eq!(result.unwrap_err().0, 3),
        }
        assert_eq!(data, [0; 4], "input was mutated");
    }
}

#[test]
fn overlapping_and_prepatched_relocations_are_rejected() {
    let data = [0u8; 12];
    let sections = [section(b".text", &data, 1, 4)];
    let symbols = [symbol(b"entry", 0, 0), symbol(b"external", UNDEFINED, 0)];
    for second in [0, 3, 4, 7, 8] {
        let relocs = [
            Relocation {
                section: 0,
                symbol: 1,
                offset: 0,
                addend: 0,
                kind: 1,
                reserved: 0,
            },
            Relocation {
                section: 0,
                symbol: 1,
                offset: second,
                addend: 0,
                kind: 0,
                reserved: 0,
            },
        ];
        let result = write(&module(1, 1, &sections, &symbols, &relocs));
        if second < 8 {
            assert_eq!(result.unwrap_err().0, 1);
        } else {
            assert!(result.is_ok());
        }
    }
    for (kind, arch, data) in [(0, 1, vec![1; 4]), (1, 1, vec![1; 8]), (8, 4, vec![1; 4])] {
        let sections = [section(b".text", &data, 1, 4)];
        let relocs = [Relocation {
            section: 0,
            symbol: 1,
            offset: 0,
            addend: 0,
            kind,
            reserved: 0,
        }];
        assert_eq!(
            write(&module(1, arch, &sections, &symbols, &relocs))
                .unwrap_err()
                .0,
            1
        );
    }
}

#[test]
fn unsupported_format_architecture_and_abi_contracts() {
    let data = [0u8; 8];
    let sections = [section(b".text", &data, 1, 4)];
    let symbols = [symbol(b"entry", 0, 0)];
    for (format, arch) in [(0, 1), (5, 1), (1, 0), (1, 5), (2, 3), (3, 3)] {
        assert_eq!(
            write(&module(format, arch, &sections, &symbols, &[]))
                .unwrap_err()
                .0,
            2
        );
    }
    for (arch, float_abi) in [(1, 1), (2, 2), (4, 1), (3, 3)] {
        let mut m = module(1, arch, &sections, &symbols, &[]);
        m.riscv_float_abi = float_abi;
        assert_eq!(write(&m).unwrap_err().0, 1);
    }
    let relocs = [Relocation {
        section: 0,
        symbol: 0,
        offset: 0,
        addend: 0,
        kind: 1,
        reserved: 0,
    }];
    assert_eq!(
        write(&module(1, 4, &sections, &symbols, &relocs))
            .unwrap_err()
            .0,
        2,
        "i386 cannot use ABS64"
    );
}

#[test]
fn seeded_descriptor_and_fixup_mutations_are_transactional() {
    let mut seed = 0x517a37u64;
    let mut next = || {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        seed
    };
    let mut successes = 0;
    let mut rejections = 0;
    for index in 0..10000 {
        let arch = (next() % 4 + 1) as u32;
        let format = (next() % 4 + 1) as u32;
        let mut data = [0u8; 32];
        let choices = forms(arch);
        let (kind, instruction) = &choices[(next() as usize) % choices.len()];
        data[..instruction.len()].copy_from_slice(instruction);
        let mut sections = [section(b".text", &data, 1, 16), section(b".data", &[7; 32], 3, 8)];
        let mut symbols = [symbol(b"entry", 0, 0)];
        let mut relocs = [Relocation {
            section: (next() % 3) as u32,
            symbol: (next() % 2) as u32,
            offset: next() % 40,
            addend: (next() % 65) as i64 - 32,
            kind: *kind,
            reserved: 0,
        }];
        match index % 8 {
            0 => sections[0].alignment = next() % 34,
            1 => symbols[0].section = (next() % 4) as u32,
            2 => symbols[0].value = next() % 40,
            3 => symbols[0].binding = (next() % 5) as u32,
            4 => relocs[0].kind = (next() % 13) as u32,
            5 => relocs[0].offset = u64::MAX,
            6 => sections[1].reserved = 1,
            _ => {}
        }
        let mut m = module(format, arch, &sections, &symbols, if index % 3 == 0 { &relocs } else { &[] });
        m.entry_symbol = UNDEFINED;
        let first = write(&m);
        let second = write(&m);
        match (first, second) {
            (Ok(a), Ok(b)) => {
                assert_eq!(a, b, "nondeterministic serialization, mutation {index}");
                if format != 4 {
                    object::File::parse(a.as_slice()).expect("writer produced malformed object");
                }
                successes += 1;
            }
            (Err(a), Err(b)) => {
                assert_eq!(a.0, b.0);
                assert!((1..=5).contains(&a.0), "internal writer panic at mutation {index}: {a:?}");
                rejections += 1;
            }
            _ => panic!("nondeterministic success/failure at mutation {index}"),
        }
    }
    assert!(successes > 1000 && rejections > 1000);
}
